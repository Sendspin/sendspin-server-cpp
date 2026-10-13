// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Listener on esp_http_server: httpd's task accepts, upgrades and receives for every accepted
// connection (SessionLink), and each delivered connection's writer task writes its frames to the
// socket directly, so httpd's task never writes for the server

#include "clock.h"
#include "connection_esp.h"
#include "esp_http_server.h"
#include "esp_loop.h"
#include "lwip/sockets.h"
#include "memory.h"
#include "profile.h"
#include "sdkconfig.h"
#include "thread_esp.h"
#include "transport.h"
#include "ws_link.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace sendspin::server {

namespace {

/// httpd's task, the receive task of every accepted connection; its stack where
/// profile::LISTENER_STACK places it
constexpr size_t HTTPD_STACK_BYTES = 4096;
/// As a dialed connection's receive task (dial_esp.cpp)
constexpr unsigned HTTPD_PRIORITY = 12;
/// Accepted connections a run keeps open, past those in their handshake: the embedded profile's
/// players
constexpr size_t ACCEPTED_CONNECTIONS = 8;
/// httpd's sessions: every accepted connection in its upgrade or handshake and every one past it
constexpr size_t HTTPD_SESSIONS = profile::MAX_ACCEPTED_HANDSHAKES + ACCEPTED_CONNECTIONS;
/// httpd's control socket's port; other httpd servers in the application (a web server,
/// sendspin-cpp's server) take the default and the one after it
constexpr uint16_t HTTPD_CTRL_PORT = ESP_HTTPD_DEF_CTRL_PORT + 2;
/// The stopper task: httpd_stop() waits for httpd's task to end. Internal RAM: it lives only
/// while the listener stops, and deletes itself
constexpr uint32_t HTTPD_STOPPER_STACK_BYTES = 2560;
/// httpd's wait for each read of a session, the upgrade's request and every frame's bytes
/// included. httpd's one task reads for every accepted connection, so a peer that sends part of a
/// frame and stops holds all of their reads for this long.
constexpr int HTTPD_RECV_WAIT_S = 2;
/// A control frame's payload (RFC 6455 section 5.5)
constexpr size_t CONTROL_PAYLOAD_MAX = 125;
/// The longest a send waits for room at a time (SessionLink::write_frame()) before it checks for a
/// close asked for; the longest httpd's task waits to answer a close
constexpr int SEND_SLICE_MS = 200;
/// Close codes a close frame never carries (RFC 6455 section 7.4.1)
constexpr uint16_t CLOSE_NO_STATUS = 1005;
constexpr uint16_t CLOSE_ABNORMAL = 1006;
constexpr uint16_t CLOSE_TLS_HANDSHAKE = 1015;

/// What httpd's callbacks reach, as its global user context: held by the Listener and by itself
/// while httpd runs, so a Listener destroyed while sessions stay open leaves it alive
struct ListenerState {
    WeakPtr<LoopCore> loop;
    /// The Listener::Impl, which posted handlers reach on the core task
    WeakPtr<void> owner;
    TransportSettings settings;
    httpd_handle_t server{nullptr};
    /// Set by close(): no session is taken any more, and httpd stops once the last one closes
    std::atomic<bool> closed{false};
    std::atomic<bool> stopping{false};
    std::atomic<int> sessions{0};
    SharedPtr<ListenerState> keep;
};

void stop_when_idle(ListenerState& state);

/// An accepted connection. Its session's context holds it until httpd frees the session.
class SessionLink final : public WsLink {
public:
    SessionLink(SharedPtr<LoopCore> loop, ListenerState& state, int fd)
        : WsLink(std::move(loop), state.settings), fd_(fd) {}

    // ---- httpd task ----

    /// The upgrade's GET completed the WebSocket handshake
    void on_upgraded() {
        this->note_rx(now_us());
        this->report_connected(peer_endpoint(this->fd_));
    }

    /// Reads one frame; an error closes the session
    esp_err_t on_frame(httpd_req_t* request) {
        httpd_ws_frame_t frame{};
        if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK) {
            return ESP_FAIL;
        }
        switch (frame.type) {
            case HTTPD_WS_TYPE_TEXT:
            case HTTPD_WS_TYPE_BINARY:
            case HTTPD_WS_TYPE_CONTINUE: {
                if (!this->begin_frame(static_cast<uint8_t>(frame.type), frame.len)) {
                    return ESP_FAIL;
                }
                if (frame.len > 0) {
                    frame.payload = this->frame_data();
                    if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
                        return ESP_FAIL;
                    }
                }
                // The frame's last byte has just been read (spec messaging.md "Receive
                // timestamps")
                const int64_t rx_us = now_us();
                this->note_rx(rx_us);
                this->request_ = request;
                const bool receiving = this->end_frame(frame.final, rx_us);
                this->request_ = nullptr;
                // A message held that could not stop the reads: httpd ends the session
                return receiving ? ESP_OK : ESP_FAIL;
            }
            case HTTPD_WS_TYPE_PING:
            case HTTPD_WS_TYPE_PONG:
            case HTTPD_WS_TYPE_CLOSE: {
                std::array<uint8_t, CONTROL_PAYLOAD_MAX> payload{};
                if (frame.len > payload.size()) {
                    return ESP_FAIL;
                }
                if (frame.len > 0) {
                    frame.payload = payload.data();
                    if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
                        return ESP_FAIL;
                    }
                }
                this->note_rx(now_us());
                if (frame.type == HTTPD_WS_TYPE_PING) {
                    {
                        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
                        std::memcpy(this->pong_.data(), payload.data(), frame.len);
                        this->pong_bytes_ = frame.len;
                    }
                    this->ask_pong();
                } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
                    // httpd closes the session once this returns
                    this->answer_close(payload.data(), frame.len);
                    this->report_closed(CloseReason::PEER_CLOSED);
                }
                return ESP_OK;
            }
            default:
                return ESP_FAIL;
        }
    }

    /// httpd closes the session: after this, the descriptor may be another session's
    void on_session_closed() {
        // A paused session closes only as httpd stops; its pause ends with it
        if (void* paused = this->hold_.take_pause()) {
            httpd_req_async_handler_complete(static_cast<httpd_req_t*>(paused));
        }
        {
            // Waits for a write in progress
            const std::lock_guard<std::mutex> lock(this->socket_mutex_);
            this->fd_closed_ = true;
        }
        this->report_closed(CloseReason::PEER_CLOSED);
        this->wake_writer();
    }

    // ---- Any task ----

    /// Ends the session: httpd's next read sees the socket shut and closes it, and a read
    /// waiting for an inbound slot stops waiting
    void end_session() {
        this->drop_inbound();
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        if (!this->fd_closed_) {
            shutdown(this->fd_, SHUT_RDWR);
        }
    }

    // ---- Core task ----

    /// Pushes the message held, and lets httpd read the session again once that message is
    /// pushed or dropped
    void resume_inbound() override {
        const InboundHold::Outcome resumed = this->hold_.resume();
        if (resumed.unpause != nullptr) {
            httpd_req_async_handler_complete(static_cast<httpd_req_t*>(resumed.unpause));
        }
    }

protected:
    // ---- httpd task ----

    /// The ring refused the message, or core has not started the connection: httpd stops reading
    /// this session (its request continued asynchronously, which keeps it out of httpd's select())
    /// until that message is released. httpd's task never waits for the ring.
    bool hold_receiving(uint64_t generation) override {
        httpd_req_t* paused = nullptr;
        if (httpd_req_async_handler_begin(this->request_, &paused) != ESP_OK) {
            // The session cannot stop being read, so it ends: httpd closes it as the handler
            // fails, and the held message goes with it
            this->report_closed(CloseReason::FAILED);
            this->drop_inbound();
            return false;
        }
        // Released meanwhile: the pause is this task's to end
        if (!this->hold_.pause(generation, paused)) {
            httpd_req_async_handler_complete(paused);
        }
        return true;
    }

    // ---- Writer task ----

    int poll_room(int timeout_ms) override {
        if (this->fd_closed_.load(std::memory_order_acquire)) {
            return -1;
        }
        // Outside the socket mutex: lwIP holds a socket for the whole of a select() on it
        // (LWIP_NETCONN_FULLDUPLEX), so httpd's close meanwhile frees the descriptor only after
        // it; one closed before the select() starts fails it, or, reused, answers spuriously, and
        // the write that follows checks fd_closed_ under the socket mutex
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(this->fd_, &writable);
        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        return select(this->fd_ + 1, nullptr, &writable, nullptr, &timeout);
    }

    // The batch's frames in one send, as it holds them
    bool send_batch(const WsBatch& batch) override {
        const std::lock_guard<std::timed_mutex> lock(this->frame_mutex_);
        if (this->close_sent_ || this->frame_broken_) {
            return false;
        }
        const std::span<const uint8_t> bytes = batch.bytes();
        std::array<iovec, 1> parts{};
        parts[0].iov_base = const_cast<uint8_t*>(bytes.data());
        parts[0].iov_len = bytes.size();
        return this->send_parts(parts.data(), parts.size(),
                                now_us() + this->settings_.write_stall_us);
    }

    bool send_ping() override {
        return this->send_frame(ws_opcode::PING, nullptr, 0);
    }

    bool send_pong() override {
        std::array<uint8_t, CONTROL_PAYLOAD_MAX> payload{};
        size_t bytes = 0;
        {
            const std::lock_guard<std::mutex> lock(this->socket_mutex_);
            bytes = this->pong_bytes_;
            std::memcpy(payload.data(), this->pong_.data(), bytes);
        }
        return this->send_frame(ws_opcode::PONG, payload.data(), bytes);
    }

    void shut(std::optional<uint16_t> code) override {
        // send_frame() refuses a second close frame and any frame after one left part written, so
        // a close the peer began is not answered twice and none lands inside a broken frame
        if (code && !this->closed()) {
            const std::array<uint8_t, 2> body{static_cast<uint8_t>(*code >> 8U),
                                              static_cast<uint8_t>(*code)};
            if (this->send_frame(ws_opcode::CLOSE, body.data(), body.size())) {
                // The peer answers with its close and closes the TCP connection, which httpd
                // sees; past the handshake timeout the session is ended anyway
                {
                    const std::lock_guard<std::mutex> lock(this->socket_mutex_);
                    if (!this->fd_closed_) {
                        shutdown(this->fd_, SHUT_WR);
                    }
                }
                const int64_t deadline_us = now_us() + this->settings_.handshake_timeout_us;
                while (!this->fd_closed_.load(std::memory_order_acquire)) {
                    const int64_t left_us = deadline_us - now_us();
                    if (left_us <= 0) {
                        break;
                    }
                    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(left_us / 1000 + 1));
                }
            }
        }
        this->end_session();
    }

private:
    /// The writer: one whole frame, waiting for the frame lock while httpd's task answers a close
    bool send_frame(uint8_t opcode, const uint8_t* data, size_t len) {
        const std::lock_guard<std::timed_mutex> lock(this->frame_mutex_);
        return this->write_frame(opcode, data, len, now_us() + this->settings_.write_stall_us);
    }

    /// httpd's task: the close frame that answers the peer's, echoing its status code (RFC 6455
    /// section 5.5.1), unless a close was sent already. A frame the writer has begun cannot be
    /// interrupted; past SEND_SLICE_MS in all, waiting for it and sending, the close goes
    /// unanswered.
    void answer_close(const uint8_t* payload, size_t len) {
        const int64_t deadline_us = now_us() + static_cast<int64_t>(SEND_SLICE_MS) * 1000;
        std::unique_lock<std::timed_mutex> lock(this->frame_mutex_, std::defer_lock);
        if (!lock.try_lock_for(std::chrono::milliseconds(SEND_SLICE_MS))) {
            return;
        }
        // A code a close frame never carries is answered with none
        constexpr size_t STATUS_CODE_BYTES = 2;
        size_t echoed = 0;
        if (len >= STATUS_CODE_BYTES) {
            const auto code = static_cast<uint16_t>((payload[0] << 8U) | payload[1]);
            if (code != CLOSE_NO_STATUS && code != CLOSE_ABNORMAL && code != CLOSE_TLS_HANDSHAKE) {
                echoed = STATUS_CODE_BYTES;
            }
        }
        this->write_frame(ws_opcode::CLOSE, payload, echoed, deadline_us);
    }

    /// Under frame_mutex_: writes one frame (send_parts())
    bool write_frame(uint8_t opcode, const uint8_t* data, size_t len, int64_t deadline_us) {
        // RFC 6455 section 5.5.1: nothing follows a close frame
        if (this->close_sent_ || this->frame_broken_) {
            return false;
        }
        if (opcode == ws_opcode::CLOSE) {
            this->close_sent_ = true;
        }
        std::array<uint8_t, WS_FRAME_HEADER_MAX> header{};
        const size_t header_bytes = ws_frame_header(opcode, len, header);
        // Header and payload in one call, so they leave in as few segments as fit
        std::array<iovec, 2> parts{};
        parts[0].iov_base = header.data();
        parts[0].iov_len = header_bytes;
        parts[1].iov_base = const_cast<uint8_t*>(data);
        parts[1].iov_len = len;
        return this->send_parts(parts.data(), len > 0 ? 2 : 1, deadline_us);
    }

    /// Under frame_mutex_: sends whole frames until they are all sent, deadline_us passes or a
    /// close is asked for. The socket mutex is held only while lwIP copies what fits, so httpd's
    /// close of the session waits no longer than that. A frame left part written breaks the
    /// stream: every later frame, a close included, is refused, and the session just ends.
    bool send_parts(iovec* parts, size_t count, int64_t deadline_us) {
        msghdr message{};
        message.msg_iov = parts;
        message.msg_iovlen = static_cast<decltype(message.msg_iovlen)>(count);
        size_t total = 0;
        for (size_t i = 0; i < count; ++i) {
            total += parts[i].iov_len;
        }
        size_t left = total;
        // Frames some of whose bytes went are broken by any failure from here
        const auto fail = [this, &left, total] {
            if (left < total) {
                this->frame_broken_ = true;
            }
            return false;
        };
        while (left > 0) {
            ssize_t sent = 0;
            {
                const std::lock_guard<std::mutex> lock(this->socket_mutex_);
                if (this->fd_closed_) {
                    return fail();
                }
                sent = sendmsg(this->fd_, &message, MSG_DONTWAIT);
            }
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return fail();
            }
            if (sent > 0) {
                left -= static_cast<size_t>(sent);
                skip_sent(message, static_cast<size_t>(sent));
                continue;
            }
            const int64_t left_us = deadline_us - now_us();
            if (left_us <= 0 || this->close_requested()) {
                return fail();
            }
            const auto wait_ms = static_cast<int>(
                std::min<int64_t>(left_us / 1000 + 1, static_cast<int64_t>(SEND_SLICE_MS)));
            if (this->poll_room(wait_ms) < 0) {
                return fail();
            }
        }
        return true;
    }

    static void skip_sent(msghdr& message, size_t sent) {
        while (sent > 0 && message.msg_iovlen > 0) {
            iovec& part = message.msg_iov[0];
            const size_t taken = std::min(sent, part.iov_len);
            part.iov_base = static_cast<uint8_t*>(part.iov_base) + taken;
            part.iov_len -= taken;
            sent -= taken;
            if (part.iov_len == 0) {
                ++message.msg_iov;
                --message.msg_iovlen;
            }
        }
    }

    const int fd_;
    /// httpd's task: the request of the frame being read
    httpd_req_t* request_{nullptr};
    /// Held for one whole frame, so frames from the writer and httpd's task never interleave;
    /// taken before socket_mutex_
    std::timed_mutex frame_mutex_;
    /// Guarded by frame_mutex_: a close frame was sent or begun
    bool close_sent_{false};
    /// Guarded by frame_mutex_: a frame was left part written
    bool frame_broken_{false};
    std::mutex socket_mutex_;
    /// Set under socket_mutex_ once httpd closes the session
    std::atomic<bool> fd_closed_{false};
    /// Guarded by socket_mutex_, which no one holds long: the last ping's payload
    std::array<uint8_t, CONTROL_PAYLOAD_MAX> pong_{};
    size_t pong_bytes_{0};
};

}  // namespace

// ==============================================================================
// Listener
// ==============================================================================

class Listener::Impl : public EnableSharedFromThis<Impl> {
public:
    Impl(EventLoop& loop, TransportSettings settings, AcceptFn on_accept, AdmitFn admit)
        : loop_(loop),
          settings_(std::move(settings)),
          on_accept_(std::move(on_accept)),
          admit_(std::move(admit)) {}

    bool listen(const std::string& address, uint16_t port) {
        // httpd binds the wildcard of the one family lwIP was built for: IPv6, taking IPv4 peers
        // too, when it has IPv6
        const bool v6 = address == "::";
        if (!v6 && address != "0.0.0.0") {
            return false;
        }
#if CONFIG_LWIP_IPV6
        if (!v6) {
            return false;
        }
#else
        if (v6) {
            this->family_unavailable_ = true;
            return false;
        }
#endif
        this->family_unavailable_ = false;
        auto state = make_shared_ptr<ListenerState>();
        state->loop = this->loop_.impl().core();
        state->owner = this->weak_from_this();
        state->settings = this->settings_;

        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = port;
        config.ctrl_port = HTTPD_CTRL_PORT;
        config.stack_size = HTTPD_STACK_BYTES;
        config.task_caps = stack_caps(profile::LISTENER_STACK, HTTPD_STACK_BYTES);
        config.task_priority = HTTPD_PRIORITY;
        config.max_open_sockets = HTTPD_SESSIONS;
        config.max_uri_handlers = 1;
        config.backlog_conn = static_cast<uint8_t>(profile::MAX_ACCEPTED_HANDSHAKES);
        config.lru_purge_enable = false;
        config.recv_wait_timeout = HTTPD_RECV_WAIT_S;
        config.open_fn = &Impl::on_session_open;
        config.close_fn = &Impl::on_session_close;
        config.global_user_ctx = state.get();
        // The state's own reference keeps it; httpd must not free it
        config.global_user_ctx_free_fn = [](void* /*ctx*/) {};
        if (httpd_start(&state->server, &config) != ESP_OK) {
            return false;
        }
        state->keep = state;

        httpd_uri_t uri{};
        uri.uri = this->settings_.path.c_str();
        uri.method = HTTP_GET;
        uri.handler = &Impl::on_request;
        uri.is_websocket = true;
        // Pings, pongs and closes come to on_frame(), so httpd itself never writes to a session
        uri.handle_ws_control_frames = true;
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
        uri.ws_post_handshake_cb = &Impl::on_upgrade;
#endif
        if (httpd_register_uri_handler(state->server, &uri) != ESP_OK) {
            state->closed.store(true, std::memory_order_release);
            stop_when_idle(*state);
            return false;
        }
        this->state_ = std::move(state);
        this->port_ = port;
        return true;
    }

    uint16_t port() const {
        return this->port_;
    }

    bool family_unavailable() const {
        return this->family_unavailable_;
    }

    void close() {
        this->closed_ = true;
        for (Pending& pending : this->pending_) {
            pending.link->end_session();
        }
        this->pending_.clear();
        if (this->state_) {
            this->state_->closed.store(true, std::memory_order_release);
            stop_when_idle(*this->state_);
        }
    }

    size_t upgrades_in_progress() const {
        return this->pending_.size();
    }

    // ---- Core task, posted from httpd's task ----

    void on_opened(const SharedPtr<SessionLink>& link) {
        if (this->closed_ || (this->admit_ && !this->admit_())) {
            link->end_session();
            return;
        }
        Pending& pending = this->pending_.emplace_back(link, this->loop_);
        pending.timer.start(now_us() + this->settings_.handshake_timeout_us,
                            [this, raw = link.get()] { this->drop_pending(raw); });
    }

    void on_upgraded(const SharedPtr<SessionLink>& link) {
        if (!this->take_pending(link.get())) {
            return;
        }
        auto connection = Connection::Impl::make(this->loop_, link, this->settings_);
        if (!link->start()) {
            link->end_session();
            return;
        }
        this->on_accept_(Connection(std::move(connection)));
    }

    void on_closed(SessionLink* link) {
        this->take_pending(link);
    }

private:
    struct Pending {
        Pending(SharedPtr<SessionLink> pending_link, EventLoop& loop)
            : link(std::move(pending_link)), timer(loop) {}
        SharedPtr<SessionLink> link;
        Timer timer;
    };

    bool take_pending(const SessionLink* link) {
        for (auto it = this->pending_.begin(); it != this->pending_.end(); ++it) {
            if (it->link.get() == link) {
                this->pending_.erase(it);
                return true;
            }
        }
        return false;
    }

    void drop_pending(SessionLink* link) {
        for (Pending& pending : this->pending_) {
            if (pending.link.get() == link) {
                pending.link->end_session();
                break;
            }
        }
        this->take_pending(link);
    }

    // ---- httpd's task ----

    static ListenerState* state_of(httpd_handle_t server) {
        return static_cast<ListenerState*>(httpd_get_global_user_ctx(server));
    }

    static SharedPtr<SessionLink> link_of(httpd_handle_t server, int fd) {
        auto* slot = static_cast<SharedPtr<SessionLink>*>(httpd_sess_get_ctx(server, fd));
        return slot != nullptr ? *slot : nullptr;
    }

    static void post(ListenerState& state, std::function<void(Impl&)> fn) {
        if (const SharedPtr<LoopCore> loop = state.loop.lock()) {
            loop->post([owner = state.owner, fn = std::move(fn)] {
                if (const SharedPtr<void> self = owner.lock()) {
                    fn(*std::static_pointer_cast<Impl>(self));
                }
            });
        }
    }

    static esp_err_t on_session_open(httpd_handle_t server, int fd) {
        ListenerState* state = state_of(server);
        if (state == nullptr || state->closed.load(std::memory_order_acquire)) {
            return ESP_FAIL;
        }
        SharedPtr<LoopCore> loop = state->loop.lock();
        if (!loop) {
            return ESP_FAIL;
        }
        const int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        auto link = make_shared_external<SessionLink>(std::move(loop), *state, fd);
        state->sessions.fetch_add(1, std::memory_order_acq_rel);
        httpd_sess_set_ctx(server, fd, new SharedPtr<SessionLink>(link),
                           [](void* ctx) { delete static_cast<SharedPtr<SessionLink>*>(ctx); });
        post(*state, [link](Impl& self) { self.on_opened(link); });
        return ESP_OK;
    }

    static void on_session_close(httpd_handle_t server, int fd) {
        ListenerState* state = state_of(server);
        const SharedPtr<SessionLink> link = link_of(server, fd);
        if (link) {
            link->on_session_closed();
        }
        // httpd closes the descriptor only without a close_fn; shutting reads first keeps lwIP
        // from delivering to a socket being torn down
        shutdown(fd, SHUT_RD);
        ::close(fd);
        // A session refused in on_session_open() has no link and was never counted
        if (link && state != nullptr) {
            post(*state, [raw = link.get()](Impl& self) { self.on_closed(raw); });
            state->sessions.fetch_sub(1, std::memory_order_acq_rel);
            stop_when_idle(*state);
        }
    }

    static esp_err_t on_upgrade(httpd_req_t* request) {
        const int fd = httpd_req_to_sockfd(request);
        ListenerState* state = state_of(request->handle);
        const SharedPtr<SessionLink> link = link_of(request->handle, fd);
        if (state == nullptr || !link) {
            return ESP_FAIL;
        }
        link->on_upgraded();
        post(*state, [link](Impl& self) { self.on_upgraded(link); });
        return ESP_OK;
    }

    static esp_err_t on_request(httpd_req_t* request) {
        if (request->method == HTTP_GET) {
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
            // An upgrade goes to on_upgrade(); this GET is not one
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, nullptr);
            return ESP_FAIL;
#else
            if (httpd_req_get_hdr_value_len(request, "Upgrade") == 0) {
                httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, nullptr);
                return ESP_FAIL;
            }
            return on_upgrade(request);
#endif
        }
        const SharedPtr<SessionLink> link = link_of(request->handle, httpd_req_to_sockfd(request));
        return link ? link->on_frame(request) : ESP_FAIL;
    }

    EventLoop& loop_;
    TransportSettings settings_;
    AcceptFn on_accept_;
    AdmitFn admit_;
    SharedPtr<ListenerState> state_;
    std::list<Pending> pending_;
    uint16_t port_{0};
    bool closed_{false};
    bool family_unavailable_{false};
};

namespace {

/// Stops httpd once the listener is closed and its last session is gone: on a task of its own,
/// since httpd_stop() waits for httpd's task to end, which may be the caller
void stop_when_idle(ListenerState& state) {
    if (!state.closed.load(std::memory_order_acquire) ||
        state.sessions.load(std::memory_order_acquire) != 0 ||
        state.stopping.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto* held = new SharedPtr<ListenerState>(std::move(state.keep));
    const SharedPtr<LoopCore> loop = (*held)->loop.lock();
    if (loop) {
        loop->task_started();
    }
    const auto stopper = [](void* arg) {
        auto* stopping = static_cast<SharedPtr<ListenerState>*>(arg);
        httpd_stop((*stopping)->server);
        if (const SharedPtr<LoopCore> stopped_loop = (*stopping)->loop.lock()) {
            stopped_loop->task_ended();
        }
        delete stopping;
        vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCore(stopper, "ss_httpd_stop", HTTPD_STOPPER_STACK_BYTES, held,
                                HTTPD_PRIORITY, nullptr, tskNO_AFFINITY) != pdPASS) {
        // httpd keeps running, idle, holding its state
        state.keep = std::move(*held);
        delete held;
        if (loop) {
            loop->task_ended();
        }
    }
}

}  // namespace

Listener::Listener(EventLoop& loop, TransportSettings settings, AcceptFn on_accept, AdmitFn admit)
    : impl_(make_shared_ptr<Impl>(loop, std::move(settings), std::move(on_accept),
                                  std::move(admit))) {}

Listener::~Listener() {
    this->impl_->close();
}

bool Listener::listen(const std::string& address, uint16_t port) {
    return this->impl_->listen(address, port);
}

uint16_t Listener::port() const {
    return this->impl_->port();
}

bool Listener::family_unavailable() const {
    return this->impl_->family_unavailable();
}

void Listener::close() {
    this->impl_->close();
}

size_t Listener::upgrades_in_progress() const {
    return this->impl_->upgrades_in_progress();
}

}  // namespace sendspin::server
