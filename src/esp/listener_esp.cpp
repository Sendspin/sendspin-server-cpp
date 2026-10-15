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
// socket directly (WsLink), so httpd's task never writes for the server but to answer a close

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
#include "ws_frame.h"
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
/// As a dialed connection's receive task (ThreadRole::LINK_READER)
constexpr unsigned HTTPD_PRIORITY = 12;
/// httpd's sessions: every accepted connection in its upgrade or handshake and every one activated
constexpr size_t HTTPD_SESSIONS = profile::MAX_ACCEPTED_HANDSHAKES + profile::MAX_ACCEPTED_CLIENTS;
/// The sockets httpd takes besides its sessions': its listening socket and its control socket's
/// two; httpd_start() refuses more sessions than CONFIG_LWIP_MAX_SOCKETS less these
constexpr size_t HTTPD_OWN_SOCKETS = 3;
static_assert(HTTPD_SESSIONS + HTTPD_OWN_SOCKETS <= CONFIG_LWIP_MAX_SOCKETS,
              "the listener's httpd needs CONFIG_LWIP_MAX_SOCKETS of at least its sessions "
              "(profile::MAX_ACCEPTED_HANDSHAKES + profile::MAX_ACCEPTED_CLIENTS) plus its own 3 "
              "sockets; raise CONFIG_LWIP_MAX_SOCKETS, leaving room for dials and mDNS");
/// Connections lwIP holds for httpd before it accepts them, as when its sessions are all taken
constexpr size_t HTTPD_BACKLOG = profile::MAX_ACCEPTED_HANDSHAKES;
/// lwIP's active TCP connections the caps fill at once: httpd's sessions and backlog and the dials
/// in their handshake (httpd's listening socket takes a listening one, its control sockets are
/// UDP). Each activated dialed connection, which no cap bounds, takes one more.
constexpr size_t CAPPED_TCP_CONNECTIONS = HTTPD_SESSIONS + HTTPD_BACKLOG + profile::MAX_DIALING;
static_assert(CAPPED_TCP_CONNECTIONS <= CONFIG_LWIP_MAX_ACTIVE_TCP,
              "CONFIG_LWIP_MAX_ACTIVE_TCP must cover httpd's sessions "
              "(profile::MAX_ACCEPTED_HANDSHAKES + profile::MAX_ACCEPTED_CLIENTS) and backlog "
              "(profile::MAX_ACCEPTED_HANDSHAKES) plus the dials in their handshake "
              "(profile::MAX_DIALING); raise it, leaving room for each activated dialed "
              "connection");
/// httpd's control socket's port; other httpd servers in the application (a web server,
/// sendspin-cpp's server) take the default and the one after it
constexpr uint16_t HTTPD_CTRL_PORT = ESP_HTTPD_DEF_CTRL_PORT + 2;
/// The stopper task: httpd_stop() waits for httpd's task to end. Internal RAM: it lives only
/// while the listener stops, and deletes itself
constexpr uint32_t HTTPD_STOPPER_STACK_BYTES = 2560;
/// The listener's destruction checks this often whether the stopper task's httpd_stop() returned
constexpr uint32_t HTTPD_STOPPED_POLL_MS = 10;
/// httpd's wait for each read of a session, the upgrade's request and every frame's bytes
/// included. httpd's one task reads for every accepted connection, so a peer that sends part of a
/// frame and stops holds all of their reads for this long.
constexpr int HTTPD_RECV_WAIT_S = 2;

/// What httpd's callbacks reach, as its global user context: held by the Listener, and through
/// keep by itself from listen()'s success until httpd stops (by the stopper task while it runs),
/// or for good when httpd fails to stop
struct ListenerState {
    WeakPtr<LoopCore> loop;
    /// The Listener::Impl, which posted handlers reach on the core task
    WeakPtr<void> owner;
    TransportSettings settings;
    httpd_handle_t server{nullptr};
    /// Set by close(): no session is taken any more, and httpd stops once the last one closes
    std::atomic<bool> closed{false};
    /// Taken by whoever calls httpd_stop(): the stopper task, the listener's destruction, or
    /// listen() when registering its handler fails; given back when the stopper task cannot start
    /// or its stop fails
    std::atomic<bool> stopping{false};
    /// Set once httpd_stop() returned: httpd's ports are free
    std::atomic<bool> stopped{false};
    std::atomic<int> sessions{0};
    SharedPtr<ListenerState> keep;
};

void stop_when_idle(ListenerState& state);

/// An accepted connection. Its session's context holds it until httpd frees the session.
class SessionLink final : public WsLink {
public:
    SessionLink(SharedPtr<LoopCore> loop, ListenerState& state, int fd)
        : WsLink(std::move(loop), state.settings, false) {
        this->fd_ = fd;
    }

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
        // httpd checks neither the opcode nor a control frame's FIN bit and length. The RSV bits
        // and whether a length takes its fewest bytes, which it does not expose, go unchecked.
        if (!ws_frame_allowed(static_cast<uint8_t>(frame.type), frame.final, frame.len)) {
            this->fail_protocol();
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
                // At most WS_CONTROL_PAYLOAD_MAX bytes (ws_frame_allowed())
                std::array<uint8_t, WS_CONTROL_PAYLOAD_MAX> payload{};
                if (frame.len > 0) {
                    frame.payload = payload.data();
                    if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK) {
                        return ESP_FAIL;
                    }
                }
                this->note_rx(now_us());
                if (frame.type == HTTPD_WS_TYPE_PING) {
                    this->store_ping(payload.data(), frame.len);
                    this->ask_pong();
                } else if (frame.type == HTTPD_WS_TYPE_CLOSE) {
                    // httpd closes the session once this returns
                    return this->answer_close(payload.data(), frame.len) ? ESP_OK : ESP_FAIL;
                }
                return ESP_OK;
            }
            default:
                // Refused by ws_frame_allowed()
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
        this->shutdown_socket(SHUT_RDWR);
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

    void shut(std::optional<uint16_t> code) override {
        // send_close() refuses a second close frame and any frame after one left part written, so
        // a close the peer began is not answered twice and none lands inside a broken frame
        if (code && !this->closed()) {
            if (this->send_close(*code)) {
                // The peer answers with its close and closes the TCP connection, which httpd
                // sees; past the handshake timeout the session is ended anyway
                this->shutdown_socket(SHUT_WR);
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
    /// httpd's task: the request of the frame being read
    httpd_req_t* request_{nullptr};
};

SharedPtr<SessionLink> link_of(httpd_handle_t server, int fd) {
    auto* slot = static_cast<SharedPtr<SessionLink>*>(httpd_sess_get_ctx(server, fd));
    return slot != nullptr ? *slot : nullptr;
}

/// httpd's task, queued ahead of httpd_stop(): httpd closes its control socket before the sessions
/// still open, and ending a pause writes to it, so every session ends here first, its held message
/// dropped and its pause ended
void end_sessions(void* arg) {
    auto* server = static_cast<httpd_handle_t>(arg);
    std::array<int, HTTPD_SESSIONS> fds{};
    size_t count = fds.size();
    if (httpd_get_client_list(server, &count, fds.data()) != ESP_OK) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (const SharedPtr<SessionLink> link = link_of(server, fds[i])) {
            link->end_session();
        }
    }
}

/// Stops httpd, ending its sessions first; false when httpd_stop() failed and httpd runs on
bool stop_httpd(httpd_handle_t server) {
    // Unqueued, a session still paused has its pause ended as httpd closes it, which writes to
    // httpd's control socket after httpd closed it
    httpd_queue_work(server, &end_sessions, server);
    return httpd_stop(server) == ESP_OK;
}

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
        config.backlog_conn = static_cast<uint8_t>(HTTPD_BACKLOG);
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
            // Stopped here, so the port is free once listen() returns. The stop is taken before
            // closed is set, so a session that opened meanwhile never takes it as it closes.
            state->stopping.store(true, std::memory_order_release);
            state->closed.store(true, std::memory_order_release);
            if (!stop_httpd(state->server)) {
                // httpd runs on, holding its state
                state->keep = state;
            }
            return false;
        }
        state->keep = state;
        this->state_ = std::move(state);
        this->port_ = port;
        return true;
    }

    uint16_t port() const {
        return this->port_;
    }

    /// Returns once httpd has stopped, so a listener started next finds its ports free: stops it
    /// here, ending the sessions still open, unless the stopper task has begun to
    void stop_now() {
        if (!this->state_) {
            return;
        }
        ListenerState& state = *this->state_;
        while (!state.stopped.load(std::memory_order_acquire)) {
            if (!state.stopping.exchange(true, std::memory_order_acq_rel)) {
                if (stop_httpd(state.server)) {
                    state.stopped.store(true, std::memory_order_release);
                    state.keep.reset();
                }
                // Otherwise httpd runs on, holding its state, and its ports stay bound
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(HTTPD_STOPPED_POLL_MS));
        }
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
        // A scope of its own, since vTaskDelete() runs no destructor. The loop is locked first: a
        // failed stop hands the state back, and the listener's destruction may then free it.
        {
            const SharedPtr<LoopCore> stopped_loop = (*stopping)->loop.lock();
            ListenerState& stopped = **stopping;
            if (stop_httpd(stopped.server)) {
                stopped.stopped.store(true, std::memory_order_release);
            } else {
                // httpd runs on, holding its state; the listener's destruction tries again
                stopped.keep = std::move(*stopping);
                stopped.stopping.store(false, std::memory_order_release);
            }
            if (stopped_loop) {
                stopped_loop->task_ended();
            }
        }
        delete stopping;
        vTaskDelete(nullptr);
    };
    if (xTaskCreatePinnedToCore(stopper, "ss_httpd_stop", HTTPD_STOPPER_STACK_BYTES, held,
                                HTTPD_PRIORITY, nullptr, tskNO_AFFINITY) != pdPASS) {
        // httpd keeps running, idle, holding its state, until the listener's destruction stops it
        state.keep = std::move(*held);
        delete held;
        state.stopping.store(false, std::memory_order_release);
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
    this->impl_->stop_now();
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
