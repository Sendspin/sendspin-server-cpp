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

// dial() on esp_websocket_client: the client's task connects, upgrades and receives (ClientLink's
// event handler); the link's writer task starts the client, writes and tears the client down

#include "clock.h"
#include "connection_esp.h"
#include "esp_loop.h"
#include "esp_tls.h"
#include "esp_transport.h"
#include "esp_transport_internal.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "lwip/sockets.h"
#include "memory.h"
#include "profile.h"
#include "thread_esp.h"
#include "transport.h"
#include "ws_link.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sendspin::server {

namespace {

/// The client task's stack, where profile::DIAL_RECEIVE_STACK places it when the client takes
/// task_caps, else in internal RAM: its receive path, the event handler and its error logs, which
/// esp-tls's failed connects take deepest (2.5 KB, peers reset during a redial)
constexpr int WS_CLIENT_STACK_BYTES = 3584;
/// Above the writers and the core task, so a message is stamped as soon as it is read
constexpr int WS_CLIENT_PRIORITY = 12;
/// The client's receive and send buffers (one each, from the default heap): a message up to this
/// size goes as one frame; a larger one goes as several (RFC 6455 section 5.4)
constexpr size_t WS_CLIENT_BUFFER_MAX_BYTES = 16384;
/// The client's own pings are left at about one a day: the idle timer on the core task pings
constexpr size_t WS_CLIENT_PING_INTERVAL_S = 86400;
constexpr int64_t US_PER_MS = 1000;

TickType_t ticks_for_us(int64_t us) {
    return pdMS_TO_TICKS(std::max<int64_t>(us / US_PER_MS, 1));
}

int ms_for_us(int64_t us) {
    return static_cast<int>(std::max<int64_t>(us / US_PER_MS, 1));
}

timeval timeval_for_ms(int ms) {
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
    return tv;
}

/// select() on one descriptor; > 0 ready, 0 timed out, < 0 an error
int poll_fd(int fd, bool for_write, int timeout_ms) {
    if (fd < 0) {
        return -1;
    }
    fd_set ready;
    fd_set errors;
    FD_ZERO(&ready);
    FD_ZERO(&errors);
    FD_SET(fd, &ready);
    FD_SET(fd, &errors);
    timeval timeout = timeval_for_ms(std::max(timeout_ms, 0));
    const int polled = for_write ? select(fd + 1, nullptr, &ready, &errors, &timeout)
                                 : select(fd + 1, &ready, nullptr, &errors, &timeout);
    if (polled > 0 && FD_ISSET(fd, &errors) && !FD_ISSET(fd, &ready)) {
        return -1;
    }
    return polled;
}

/// A dialed connection: an esp_websocket_client over a WebSocket transport whose TCP layer is the
/// link's own socket (ClientLink::socket_*), not esp_transport_tcp. esp_websocket_client closes
/// its transport from whichever task finds an error, with only its receive lock held, while a send
/// (the writer's, or the client task's pong) may be between two writes. Here a close only shuts
/// the socket down (which fails later reads and writes and wakes a select() on it), and the
/// descriptor is closed when the transport is destroyed, after the client task has ended and on
/// the writer, so no task's call or select() reaches a descriptor another connection has reused;
/// reads and writes take it under one mutex, held only for the call that moves the bytes. Owning
/// the socket also sets
/// TCP_NODELAY (esp_transport_ws writes a frame's header and payload as two writes, so a small
/// message could otherwise wait for the peer's delayed ACK) and lets the address tried differ
/// from the Host header's name.
class ClientLink final : public WsLink {
public:
    ClientLink(SharedPtr<LoopCore> loop, const TransportSettings& settings, DialTarget target)
        : WsLink(std::move(loop), settings), target_(std::move(target)) {
        // RFC 3986 section 3.2.2: an IPv6 literal is bracketed
        this->host_header_ = this->target_.host.find(':') == std::string::npos
                                 ? this->target_.host
                                 : "[" + this->target_.host + "]";
    }

    ~ClientLink() override {
        this->destroy_client();
        if (this->resume_ != nullptr) {
            vSemaphoreDelete(this->resume_);
        }
    }

    /// Core task, or the task closing the link: wakes the client task holding a message
    void resume_inbound() override {
        if (this->resume_ != nullptr) {
            xSemaphoreGive(this->resume_);
        }
    }

protected:
    /// The client task, holding a message: waits until it is pushed or dropped. It holds
    /// esp_websocket_client's receive lock meanwhile: sends take only the send lock, but a failed
    /// send takes the receive lock before it aborts the connection, so a writer whose send fails
    /// waits until the message is released, which core's close at the write stall does at the
    /// latest.
    bool hold_receiving(uint64_t generation) override {
        while (!this->hold_.released(generation)) {
            xSemaphoreTake(this->resume_, portMAX_DELAY);
            this->hold_.resume();
        }
        return true;
    }

    bool open() override {
        if (this->resume_ == nullptr) {
            this->report_closed(CloseReason::FAILED);
            return false;
        }
        std::vector<std::string> hosts = this->target_.addresses;
        if (hosts.empty()) {
            hosts.push_back(this->target_.host);
        }
        CloseReason failure = CloseReason::FAILED;
        for (const std::string& host : hosts) {
            if (this->close_requested()) {
                return false;
            }
            if (this->connect_to(host)) {
                return true;
            }
            failure = static_cast<CloseReason>(this->attempt_failure_.load());
            this->destroy_client();
        }
        if (!this->close_requested()) {
            this->report_closed(failure);
        }
        return false;
    }

    int poll_room(int timeout_ms) override {
        return poll_fd(this->socket_snapshot(), true, timeout_ms);
    }

    bool send_message(MessageKind kind, const uint8_t* data, size_t len) override {
        const TickType_t timeout = ticks_for_us(this->settings_.write_stall_us);
        const char* bytes = reinterpret_cast<const char*>(data);
        const int size = static_cast<int>(len);
        const int sent = kind == MessageKind::TEXT
                             ? esp_websocket_client_send_text(this->client_, bytes, size, timeout)
                             : esp_websocket_client_send_bin(this->client_, bytes, size, timeout);
        return sent == size;
    }

    bool send_ping() override {
        return esp_websocket_client_send_with_opcode(
                   this->client_, WS_TRANSPORT_OPCODES_PING, nullptr, 0,
                   ticks_for_us(this->settings_.write_stall_us)) >= 0;
    }

    void shut(std::optional<uint16_t> code) override {
        if (this->client_ == nullptr) {
            return;
        }
        if (code && this->connected() && !this->closed() &&
            esp_websocket_client_is_connected(this->client_)) {
            // Sends the close frame (within the write stall, socket_write()), then waits for the
            // peer's and for the TCP close (esp_transport_ws_poll_connection_closed() on
            // socket_get()), within the handshake timeout; stops the client either way
            esp_websocket_client_close_with_code(
                this->client_, *code, nullptr, 0,
                ticks_for_us(this->settings_.handshake_timeout_us));
        }
        this->destroy_client();
    }

private:
    static void on_event(void* arg, esp_event_base_t /*base*/, int32_t id, void* data) {
        // First, so a message's stamp is as close to its last byte's arrival as this task gets
        const int64_t rx_us = now_us();
        auto* link = static_cast<ClientLink*>(arg);
        const auto* event = static_cast<const esp_websocket_event_data_t*>(data);
        switch (id) {
            case WEBSOCKET_EVENT_CONNECTED:
                link->on_connected();
                break;
            case WEBSOCKET_EVENT_DATA:
                link->on_data(*event, rx_us);
                break;
            case WEBSOCKET_EVENT_DISCONNECTED:
                link->on_lost(link->reason_of(*event));
                break;
            case WEBSOCKET_EVENT_CLOSED:
                link->on_lost(CloseReason::PEER_CLOSED);
                break;
            case WEBSOCKET_EVENT_FINISH:
                // An attempt that failed keeps the reason its DISCONNECTED gave
                if (link->connected()) {
                    link->report_closed(CloseReason::PEER_CLOSED);
                }
                link->finished_.store(true, std::memory_order_release);
                link->wake_writer();
                break;
            default:
                break;
        }
    }

    /// One attempt at the address host; true once the WebSocket is open, false when it failed or
    /// a close was asked for
    bool connect_to(const std::string& host) {
        this->finished_.store(false, std::memory_order_release);
        this->connect_timed_out_ = false;
        this->attempt_failure_.store(static_cast<int>(CloseReason::FAILED));
        this->address_ = host;
        if (!this->create_client()) {
            return false;
        }
        while (true) {
            if (this->connected()) {
                return true;
            }
            if (this->finished_.load(std::memory_order_acquire) || this->close_requested()) {
                return false;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }

    bool create_client() {
        this->transports_ = esp_transport_list_init();
        if (this->transports_ == nullptr) {
            return false;
        }
        this->socket_transport_ = esp_transport_init();
        if (this->socket_transport_ == nullptr) {
            return false;
        }
        esp_transport_set_func(this->socket_transport_, &ClientLink::socket_connect,
                               &ClientLink::socket_read, &ClientLink::socket_write,
                               &ClientLink::socket_close, &ClientLink::socket_poll_read,
                               &ClientLink::socket_poll_write, &ClientLink::socket_destroy);
        esp_transport_set_context_data(this->socket_transport_, this);
        // For esp_transport_ws_poll_connection_closed(), which selects on the socket itself
        this->socket_transport_->_get_socket = &ClientLink::socket_get;
        esp_transport_list_add(this->transports_, this->socket_transport_, "_tcp");
        this->ws_ = esp_transport_ws_init(this->socket_transport_);
        if (this->ws_ == nullptr) {
            return false;
        }
        esp_transport_list_add(this->transports_, this->ws_, "ws");
        esp_transport_ws_config_t ws_config{};
        ws_config.ws_path = this->target_.path.c_str();
        // As esp_websocket_client sets it for the transports it builds: control frames reach the
        // client, which answers pings and closes
        ws_config.propagate_control_frames = true;
        esp_transport_ws_set_config(this->ws_, &ws_config);

        esp_websocket_client_config_t config{};
        // The upgrade's Host header; the socket connects to address_ (socket_connect())
        config.host = this->host_header_.c_str();
        config.port = this->target_.port;
        config.path = this->target_.path.c_str();
        config.disable_auto_reconnect = true;
        config.task_prio = WS_CLIENT_PRIORITY;
        config.task_name = "ss_ws_rx";
        config.task_stack = WS_CLIENT_STACK_BYTES;
#ifdef ESP_WEBSOCKET_CLIENT_HAS_TASK_CAPS
        config.task_caps = stack_caps(profile::DIAL_RECEIVE_STACK, WS_CLIENT_STACK_BYTES);
#endif
        config.buffer_size =
            static_cast<int>(std::clamp(this->settings_.write_buffer_bytes,
                                        ROOM_AFTER_WRITE_MAX_BYTES, WS_CLIENT_BUFFER_MAX_BYTES));
        // The TCP connect, the upgrade's reads and, until the connection opens, a send
        config.network_timeout_ms = ms_for_us(this->settings_.connect_timeout_us);
        config.ping_interval_sec = WS_CLIENT_PING_INTERVAL_S;
        config.disable_pingpong_discon = true;
        config.ext_transport = this->ws_;
        this->client_ = esp_websocket_client_init(&config);
        if (this->client_ == nullptr) {
            return false;
        }
        esp_websocket_register_events(this->client_, WEBSOCKET_EVENT_ANY, &ClientLink::on_event,
                                      this);
        return esp_websocket_client_start(this->client_) == ESP_OK;
    }

    /// Stops the client task (waiting for it) and frees the client and its transports
    void destroy_client() {
        if (this->client_ != nullptr) {
            esp_websocket_client_destroy(this->client_);
            this->client_ = nullptr;
        }
        if (this->transports_ != nullptr) {
            // The client never owns an external transport; the socket transport's destroy closes
            // the socket
            esp_transport_list_destroy(this->transports_);
            this->transports_ = nullptr;
        }
        this->socket_transport_ = nullptr;
        this->ws_ = nullptr;
    }

    // ---- The socket transport: the client task, and the writer for writes, closes and the
    // transport's destruction ----

    static ClientLink& link_of(esp_transport_handle_t t) {
        return *static_cast<ClientLink*>(esp_transport_get_context_data(t));
    }

    static int socket_connect(esp_transport_handle_t t, const char* /*host*/, int port,
                              int timeout_ms) {
        return link_of(t).connect_socket(port, timeout_ms);
    }

    static int socket_read(esp_transport_handle_t t, char* buffer, int len, int timeout_ms) {
        ClientLink& link = link_of(t);
        return link.read_socket(buffer, len, link.bounded_ms(timeout_ms));
    }

    static int socket_write(esp_transport_handle_t t, const char* buffer, int len, int timeout_ms) {
        ClientLink& link = link_of(t);
        return link.write_socket(buffer, len, link.bounded_ms(timeout_ms));
    }

    static int socket_close(esp_transport_handle_t t) {
        link_of(t).shut_socket();
        return 0;
    }

    static int socket_poll_read(esp_transport_handle_t t, int timeout_ms) {
        ClientLink& link = link_of(t);
        return poll_fd(link.socket_snapshot(), false, link.bounded_ms(timeout_ms));
    }

    static int socket_poll_write(esp_transport_handle_t t, int timeout_ms) {
        ClientLink& link = link_of(t);
        return poll_fd(link.socket_snapshot(), true, link.bounded_ms(timeout_ms));
    }

    static int socket_get(esp_transport_handle_t t) {
        ClientLink& link = link_of(t);
        const std::lock_guard<std::mutex> lock(link.socket_mutex_);
        return link.fd_;
    }

    static int socket_destroy(esp_transport_handle_t t) {
        link_of(t).release_socket();
        return 0;
    }

    /// esp_websocket_client waits with -1 (portMAX_DELAY) for a close frame's send, which would
    /// never end on a peer that stops reading: a wait is at most the write stall
    int bounded_ms(int timeout_ms) const {
        const int stall_ms = ms_for_us(this->settings_.write_stall_us);
        return timeout_ms < 0 ? stall_ms : std::min(timeout_ms, stall_ms);
    }

    /// The descriptor to poll, or -1 once the socket is shut down
    int socket_snapshot() {
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        return this->shut_ ? -1 : this->fd_;
    }

    /// The client task, connecting: to the address being tried, whatever the Host header names
    int connect_socket(int port, int timeout_ms) {
        this->release_socket();
        esp_tls_cfg_t config{};
        config.timeout_ms = timeout_ms;
        esp_tls_last_error_t error{};
        int fd = -1;
        const esp_err_t result = esp_tls_plain_tcp_connect(this->address_.c_str(),
                                                           static_cast<int>(this->address_.size()),
                                                           port, &config, &error, &fd);
        if (result != ESP_OK) {
            this->connect_timed_out_ = result == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT;
            return -1;
        }
        const int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        this->fd_ = fd;
        this->shut_ = false;
        return 0;
    }

    /// The client task, after a poll found data: as esp_transport_tcp reads, with its codes
    int read_socket(char* buffer, int len, int timeout_ms) {
        const int polled = poll_fd(this->socket_snapshot(), false, timeout_ms);
        if (polled < 0) {
            return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        }
        if (polled == 0) {
            return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
        }
        ssize_t got = 0;
        {
            const std::lock_guard<std::mutex> lock(this->socket_mutex_);
            if (this->fd_ < 0 || this->shut_) {
                return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            }
            got = recv(this->fd_, buffer, static_cast<size_t>(len), MSG_DONTWAIT);
        }
        if (got < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT
                                                           : ERR_TCP_TRANSPORT_CONNECTION_FAILED;
        }
        if (got == 0) {
            return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
        }
        return static_cast<int>(got);
    }

    /// The writer, or the client task for its pongs, pings and closes (esp_websocket_client's
    /// send lock serializes them): all of buffer within timeout_ms, or -1, since esp_transport_ws
    /// would send the rest of a short write as a frame of its own
    int write_socket(const char* buffer, int len, int timeout_ms) {
        const int64_t deadline_us = now_us() + static_cast<int64_t>(timeout_ms) * US_PER_MS;
        int written = 0;
        while (written < len) {
            ssize_t sent = 0;
            int fd = -1;
            {
                // Held only while lwIP copies what fits, so a close waits no longer than that
                const std::lock_guard<std::mutex> lock(this->socket_mutex_);
                fd = this->fd_;
                if (fd < 0 || this->shut_) {
                    return -1;
                }
                sent = send(fd, buffer + written, static_cast<size_t>(len - written), MSG_DONTWAIT);
            }
            if (sent > 0) {
                written += static_cast<int>(sent);
                continue;
            }
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
            const int64_t left_us = deadline_us - now_us();
            if (left_us <= 0 || this->close_requested()) {
                return -1;
            }
            if (poll_fd(fd, true, ms_for_us(left_us)) < 0) {
                return -1;
            }
        }
        return written;
    }

    /// Any task, as esp_websocket_client closes its transport: shuts the socket down, which ends
    /// its connection, fails later reads and writes and wakes a select() on it, and keeps the
    /// descriptor, so another task's select() or call never meets one reused
    void shut_socket() {
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        if (this->fd_ >= 0 && !this->shut_) {
            shutdown(this->fd_, SHUT_RDWR);
            this->shut_ = true;
        }
    }

    /// The writer as it destroys the transport, after the client task has ended, or the client
    /// task before a connect: closes the descriptor
    void release_socket() {
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        if (this->fd_ >= 0) {
            ::close(this->fd_);
            this->fd_ = -1;
        }
        this->shut_ = false;
    }

    // ---- Client task ----

    void on_connected() {
        const int fd = this->socket_snapshot();
        this->note_rx(now_us());
        this->report_connected(peer_endpoint(fd));
    }

    void on_data(const esp_websocket_event_data_t& event, int64_t rx_us) {
        this->note_rx(rx_us);
        const uint8_t opcode = event.op_code;
        // The client answers pings and closes itself
        if (opcode != ws_opcode::TEXT && opcode != ws_opcode::BINARY &&
            opcode != ws_opcode::CONTINUATION) {
            return;
        }
        // One frame arrives in as many events as the client's buffer needs
        const auto offset = static_cast<size_t>(event.payload_offset);
        const auto chunk = static_cast<size_t>(event.data_len);
        const auto frame = static_cast<size_t>(event.payload_len);
        if (offset == 0) {
            this->in_frame_ = this->begin_frame(opcode, frame);
        }
        if (!this->in_frame_) {
            return;
        }
        if (chunk > 0 && offset + chunk <= frame) {
            std::memcpy(this->frame_data() + offset, event.data_ptr, chunk);
        }
        if (offset + chunk >= frame) {
            this->in_frame_ = false;
            // The stamp of the event that completes the frame (spec messaging.md "Receive
            // timestamps")
            this->end_frame(event.fin, rx_us);
        }
    }

    void on_lost(CloseReason reason) {
        if (this->connected()) {
            this->report_closed(reason);
        } else {
            // Kept for the writer, which may try the next address
            this->attempt_failure_.store(static_cast<int>(reason));
        }
    }

    CloseReason reason_of(const esp_websocket_event_data_t& event) const {
        const esp_websocket_error_codes_t& error = event.error_handle;
        if (error.error_type == WEBSOCKET_ERROR_TYPE_PONG_TIMEOUT ||
            (!this->connected() && this->connect_timed_out_)) {
            return CloseReason::TIMED_OUT;
        }
        if (error.error_type == WEBSOCKET_ERROR_TYPE_SERVER_CLOSE ||
            (error.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT && this->connected())) {
            return CloseReason::PEER_CLOSED;
        }
        return CloseReason::FAILED;
    }

    DialTarget target_;
    std::string host_header_;
    /// Writer task, read by the client task while it runs: the address being tried
    std::string address_;
    esp_websocket_client_handle_t client_{nullptr};
    esp_transport_list_handle_t transports_{nullptr};
    esp_transport_handle_t socket_transport_{nullptr};
    esp_transport_handle_t ws_{nullptr};
    std::mutex socket_mutex_;
    /// Guarded by socket_mutex_; -1 when no socket is open
    int fd_{-1};
    /// Guarded by socket_mutex_: fd_ is shut down, to be closed when the transport is destroyed
    bool shut_{false};
    /// The client task has ended
    std::atomic<bool> finished_{false};
    std::atomic<int> attempt_failure_{static_cast<int>(CloseReason::FAILED)};
    /// Client task only: the attempt's TCP connect timed out
    bool connect_timed_out_{false};
    /// A frame begun with begin_frame() is arriving
    bool in_frame_{false};
    /// Given by resume_inbound(), taken by the client task holding a message
    SemaphoreHandle_t resume_{xSemaphoreCreateBinary()};
};

}  // namespace

// ==============================================================================
// PendingDial
// ==============================================================================

class PendingDial::Impl : public EnableSharedFromThis<Impl> {
public:
    Impl(EventLoop& loop, const DialTarget& target, const TransportSettings& settings,
         DialFn on_done)
        : timer_(loop),
          link_(make_shared_external<ClientLink>(loop.impl().core(), settings, target)),
          connection_(Connection::Impl::make(loop, link_, settings)),
          settings_(settings),
          on_done_(std::move(on_done)) {}

    void start() {
        this->connection_->await_open(
            [weak = this->weak_from_this()](std::optional<CloseReason> outcome) {
                if (const SharedPtr<Impl> self = weak.lock()) {
                    self->on_open(outcome);
                }
            });
        if (!this->link_->start()) {
            // Reported from a handler of its own, as every outcome is
            this->timer_.start(now_us(), [this] { this->finish(CloseReason::FAILED); });
            return;
        }
        // The client task bounds the connect and the upgrade's reads; this bounds the whole dial
        this->timer_.start(
            now_us() + this->settings_.connect_timeout_us + this->settings_.handshake_timeout_us,
            [this] { this->finish(CloseReason::TIMED_OUT); });
    }

    void cancel() {
        if (this->done_) {
            return;
        }
        this->done_ = true;
        this->on_done_ = nullptr;
        this->timer_.cancel();
        this->connection_->close(close_code::GOING_AWAY);
    }

private:
    void on_open(std::optional<CloseReason> outcome) {
        if (outcome) {
            this->finish(*outcome);
            return;
        }
        if (this->done_) {
            return;
        }
        this->done_ = true;
        this->timer_.cancel();
        const DialFn on_done = std::move(this->on_done_);
        on_done(Connection(std::move(this->connection_)));
    }

    void finish(CloseReason reason) {
        if (this->done_) {
            return;
        }
        // on_done may drop the last handle
        const SharedPtr<Impl> self = this->shared_from_this();
        this->done_ = true;
        this->timer_.cancel();
        this->connection_->close(close_code::GOING_AWAY);
        const DialFn on_done = std::move(this->on_done_);
        on_done(reason);
    }

    Timer timer_;
    SharedPtr<ClientLink> link_;
    SharedPtr<Connection::Impl> connection_;
    TransportSettings settings_;
    DialFn on_done_;
    bool done_{false};
};

PendingDial::PendingDial(SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

PendingDial::~PendingDial() {
    if (this->impl_) {
        this->impl_->cancel();
    }
}

PendingDial::PendingDial(PendingDial&& other) noexcept = default;

PendingDial& PendingDial::operator=(PendingDial&& other) noexcept {
    if (this != &other) {
        if (this->impl_) {
            this->impl_->cancel();
        }
        this->impl_ = std::move(other.impl_);
    }
    return *this;
}

void PendingDial::cancel() {
    this->impl_->cancel();
}

PendingDial dial(EventLoop& loop, const DialTarget& target, const TransportSettings& settings,
                 DialFn on_done) {
    auto impl = make_shared_external<PendingDial::Impl>(loop, target, settings, std::move(on_done));
    impl->start();
    return PendingDial(std::move(impl));
}

}  // namespace sendspin::server
