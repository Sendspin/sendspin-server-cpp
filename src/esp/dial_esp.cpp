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

// dial() without a WebSocket library: a dialed connection's reader task resolves, connects,
// upgrades (ws_handshake.h) and reads its frames (ws_frame.h); its writer task, WsLink's, frames
// and masks what it sends, as an accepted connection's does unmasked

#include "clock.h"
#include "connection_esp.h"
#include "esp_loop.h"
#include "esp_random.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "memory.h"
#include "thread_setup.h"
#include "transport.h"
#include "ws_frame.h"
#include "ws_handshake.h"
#include "ws_link.h"
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sendspin::server {

namespace {

/// How often the reader, waiting on its socket, looks whether the writer is stopping it
constexpr int READ_POLL_MS = 100;
/// The reader's buffer: the upgrade's response, then the frames' headers and the payloads that
/// arrive with them; the rest of a longer payload is read into its message in place
constexpr size_t READ_BUFFER_BYTES = WS_UPGRADE_RESPONSE_MAX;
constexpr int64_t US_PER_MS = 1000;
constexpr int64_t NO_DEADLINE = std::numeric_limits<int64_t>::max();

/// A dialed connection: the reader task (ThreadRole::LINK_READER) resolves the target, connects,
/// upgrades and then reads every frame; the writer task (WsLink) starts it, writes, and in shut()
/// stops it and waits for its end before closing the descriptor, so no task's call or select()
/// reaches a descriptor another connection has reused.
class DialLink final : public WsLink {
public:
    DialLink(SharedPtr<LoopCore> loop, const TransportSettings& settings, DialTarget target)
        : WsLink(std::move(loop), settings, true), target_(std::move(target)) {}

    ~DialLink() override {
        if (this->resume_ != nullptr) {
            vSemaphoreDelete(this->resume_);
        }
    }

    /// Core task, or the task closing the link: wakes the reader holding a message
    void resume_inbound() override {
        if (this->resume_ != nullptr) {
            xSemaphoreGive(this->resume_);
        }
    }

protected:
    /// The reader, holding a message: waits until it is pushed or dropped
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
        // A detached pthread, as the writer is, so its stack can be in PSRAM
        // (profile::DIAL_RECEIVE_STACK) and is freed when it ends
        this->loop().task_started();
        try {
            start_thread(ThreadRole::LINK_READER, [self = this->shared_from_this()]() mutable {
                auto* link = static_cast<DialLink*>(self.get());
                link->reader_main();
                link->reader_done_.store(true, std::memory_order_release);
                link->wake_writer();
                link->loop().task_ended();
            }).detach();
        } catch (const std::exception&) {
            this->loop().task_ended();
            this->report_closed(CloseReason::FAILED);
            return false;
        }
        this->reader_started_ = true;
        while (!this->connected()) {
            if (this->reader_done_.load(std::memory_order_acquire) || this->close_requested()) {
                return false;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        return true;
    }

    void shut(std::optional<uint16_t> code) override {
        if (!this->reader_started_) {
            return;
        }
        // RFC 6455 section 7.1.1: the server closes the TCP connection after its close frame,
        // which the reader sees; past the handshake timeout the connection is dropped anyway
        if (code && this->connected() && !this->closed() && this->send_close(*code)) {
            const int64_t deadline_us = now_us() + this->settings_.handshake_timeout_us;
            while (!this->closed() && !this->reader_done_.load(std::memory_order_acquire)) {
                const int64_t left_us = deadline_us - now_us();
                if (left_us <= 0) {
                    break;
                }
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((left_us / US_PER_MS) + 1));
            }
        }
        this->stopping_.store(true, std::memory_order_release);
        // Ends a read or select() the reader is in
        this->shutdown_socket(SHUT_RDWR);
        while (!this->reader_done_.load(std::memory_order_acquire)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(READ_POLL_MS));
        }
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        if (this->fd_ >= 0 && !this->fd_closed_) {
            ::close(this->fd_);
            this->fd_closed_ = true;
        }
    }

private:
    // ---- Reader task ----

    void reader_main() {
        if (!this->dial()) {
            return;
        }
        this->note_rx(now_us());
        this->report_connected(peer_endpoint(this->fd_));
        this->receive();
    }

    bool stopping() const {
        return this->stopping_.load(std::memory_order_acquire);
    }

    /// Each address in turn, the target's literal addresses or else what its host resolves to;
    /// true once one is upgraded, with fd_ set
    bool dial() {
        std::vector<std::string> hosts = this->target_.addresses;
        const bool numeric = !hosts.empty();
        if (!numeric) {
            hosts.push_back(this->target_.host);
        }
        const std::string port = std::to_string(this->target_.port);
        CloseReason failure = CloseReason::FAILED;
        for (const std::string& host : hosts) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = numeric ? AI_NUMERICHOST : 0;
            addrinfo* found = nullptr;
            if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0 || found == nullptr) {
                failure = CloseReason::FAILED;
                continue;
            }
            bool open = false;
            for (const addrinfo* at = found; at != nullptr && !open && !this->stopping();
                 at = at->ai_next) {
                open = this->attempt(*at, failure);
            }
            freeaddrinfo(found);
            if (open) {
                return true;
            }
            if (this->stopping()) {
                return false;
            }
        }
        if (!this->stopping()) {
            this->report_closed(failure);
        }
        return false;
    }

    /// One address: the TCP connect within connect_timeout_us, then the upgrade within
    /// handshake_timeout_us. Sets failure when it fails.
    bool attempt(const addrinfo& address, CloseReason& failure) {
        failure = CloseReason::FAILED;
        const int fd = socket(address.ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return false;
        }
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        bool open = false;
        if (connect(fd, address.ai_addr, address.ai_addrlen) == 0 || errno == EINPROGRESS) {
            const int ready = this->await(fd, true, now_us() + this->settings_.connect_timeout_us);
            int error = 0;
            socklen_t length = sizeof(error);
            if (ready == 0) {
                failure = CloseReason::TIMED_OUT;
            } else if (ready > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                       error == 0) {
                // A small frame is not held back for the peer's delayed ACK
                const int on = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                open = this->upgrade(fd, failure);
            }
        }
        if (!open) {
            ::close(fd);
            return false;
        }
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        this->fd_ = fd;
        return true;
    }

    /// The opening handshake (RFC 6455 section 4.1); the bytes after the response stay buffered
    /// as frames
    bool upgrade(int fd, CloseReason& failure) {
        std::array<uint8_t, WS_KEY_NONCE_BYTES> nonce{};
        esp_fill_random(nonce.data(), nonce.size());
        const std::string key = ws_handshake_key(nonce);
        const std::string request = ws_upgrade_request(this->target_, key);
        const int64_t deadline_us = now_us() + this->settings_.handshake_timeout_us;
        size_t sent = 0;
        while (sent < request.size()) {
            const ssize_t wrote =
                send(fd, request.data() + sent, request.size() - sent, MSG_DONTWAIT);
            if (wrote > 0) {
                sent += static_cast<size_t>(wrote);
                continue;
            }
            if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
            const int ready = this->await(fd, true, deadline_us);
            if (ready <= 0) {
                failure = ready == 0 ? CloseReason::TIMED_OUT : CloseReason::FAILED;
                return false;
            }
        }
        this->buffer_begin_ = 0;
        this->buffer_end_ = 0;
        while (true) {
            const std::string_view received(reinterpret_cast<const char*>(this->buffer_.data()),
                                            this->buffer_end_);
            const WsUpgradeCheck check = check_upgrade_response(received, key);
            if (check.status == WsUpgradeStatus::ACCEPTED) {
                this->buffer_begin_ = check.response_bytes;
                return true;
            }
            if (check.status == WsUpgradeStatus::REJECTED) {
                return false;
            }
            const int got = this->read_some(fd, deadline_us);
            if (got <= 0) {
                failure = got == 0 ? CloseReason::TIMED_OUT : CloseReason::FAILED;
                return false;
            }
        }
    }

    /// Reads every frame until the connection ends. A message's stamp is that of the read that
    /// brought its last byte (spec messaging.md "Receive timestamps").
    void receive() {
        while (true) {
            const WsHeaderParse parse = ws_parse_frame_header(this->buffered(), false);
            if (parse.status == WsHeaderStatus::INVALID) {
                this->report_closed(CloseReason::PROTOCOL_VIOLATION);
                return;
            }
            if (parse.status == WsHeaderStatus::INCOMPLETE) {
                if (this->read_some(this->fd_, NO_DEADLINE) <= 0) {
                    this->report_read_end();
                    return;
                }
                continue;
            }
            const WsFrameHeader& header = parse.header;
            this->buffer_begin_ += header.header_bytes;
            if (header.opcode >= ws_opcode::CLOSE) {
                // A control frame (RFC 6455 section 5.5), at most WS_CONTROL_PAYLOAD_MAX bytes
                std::array<uint8_t, WS_CONTROL_PAYLOAD_MAX> payload{};
                const auto len = static_cast<size_t>(header.payload_bytes);
                if (!this->read_exact(payload.data(), len)) {
                    this->report_read_end();
                    return;
                }
                this->note_rx(this->rx_us_);
                if (header.opcode == ws_opcode::PING) {
                    this->store_ping(payload.data(), len);
                    this->ask_pong();
                } else if (header.opcode == ws_opcode::CLOSE) {
                    this->answer_close(payload.data(), len);
                    this->report_closed(CloseReason::PEER_CLOSED);
                    return;
                }
                continue;
            }
            if (header.payload_bytes > this->settings_.read_message_max) {
                this->report_closed(CloseReason::PROTOCOL_VIOLATION);
                return;
            }
            const auto len = static_cast<size_t>(header.payload_bytes);
            if (!this->begin_frame(header.opcode, len)) {
                return;
            }
            if (!this->read_exact(this->frame_data(), len)) {
                this->report_read_end();
                return;
            }
            this->note_rx(this->rx_us_);
            if (!this->end_frame(header.fin, this->rx_us_)) {
                return;
            }
        }
    }

    std::span<const uint8_t> buffered() const {
        return std::span<const uint8_t>(this->buffer_.data() + this->buffer_begin_,
                                        this->buffer_end_ - this->buffer_begin_);
    }

    /// len bytes into out: those buffered first, then the rest read into out in place
    bool read_exact(uint8_t* out, size_t len) {
        const size_t taken = std::min(len, this->buffer_end_ - this->buffer_begin_);
        if (taken > 0) {
            std::memcpy(out, this->buffer_.data() + this->buffer_begin_, taken);
            this->buffer_begin_ += taken;
        }
        size_t done = taken;
        while (done < len) {
            if (this->await(this->fd_, false, NO_DEADLINE) <= 0) {
                return false;
            }
            const ssize_t got = recv(this->fd_, out + done, len - done, MSG_DONTWAIT);
            this->rx_us_ = now_us();
            if (got > 0) {
                done += static_cast<size_t>(got);
            } else if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                return false;
            }
        }
        return true;
    }

    /// Reads what fd has into the buffer, after the bytes kept (moved to its start)
    /// @return the bytes read, 0 when deadline_us passed, -1 at the connection's end, an error or
    /// a stop
    int read_some(int fd, int64_t deadline_us) {
        if (this->buffer_begin_ > 0) {
            std::memmove(this->buffer_.data(), this->buffer_.data() + this->buffer_begin_,
                         this->buffer_end_ - this->buffer_begin_);
            this->buffer_end_ -= this->buffer_begin_;
            this->buffer_begin_ = 0;
        }
        if (this->buffer_end_ == this->buffer_.size()) {
            return -1;
        }
        while (true) {
            const int ready = this->await(fd, false, deadline_us);
            if (ready <= 0) {
                return ready;
            }
            const ssize_t got = recv(fd, this->buffer_.data() + this->buffer_end_,
                                     this->buffer_.size() - this->buffer_end_, MSG_DONTWAIT);
            this->rx_us_ = now_us();
            if (got > 0) {
                this->buffer_end_ += static_cast<size_t>(got);
                return static_cast<int>(got);
            }
            if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                return -1;
            }
        }
    }

    /// select() on fd in READ_POLL_MS slices until it is ready
    /// @return > 0 ready, 0 when deadline_us passed, < 0 on an error or once the writer stops the
    /// reader
    int await(int fd, bool for_write, int64_t deadline_us) {
        while (!this->stopping()) {
            const int64_t left_us = deadline_us - now_us();
            if (left_us <= 0) {
                return 0;
            }
            const int64_t slice_ms =
                std::min<int64_t>((left_us / US_PER_MS) + 1, static_cast<int64_t>(READ_POLL_MS));
            fd_set ready;
            fd_set errors;
            FD_ZERO(&ready);
            FD_ZERO(&errors);
            FD_SET(fd, &ready);
            FD_SET(fd, &errors);
            timeval timeout{};
            timeout.tv_sec = static_cast<time_t>(slice_ms / 1000);
            timeout.tv_usec = static_cast<suseconds_t>((slice_ms % 1000) * US_PER_MS);
            const int polled = for_write ? select(fd + 1, nullptr, &ready, &errors, &timeout)
                                         : select(fd + 1, &ready, nullptr, &errors, &timeout);
            if (polled < 0 || (polled > 0 && FD_ISSET(fd, &errors) && !FD_ISSET(fd, &ready))) {
                return -1;
            }
            if (polled > 0) {
                return polled;
            }
        }
        return -1;
    }

    /// The connection ended under a read: the peer's close of TCP, an error, or the writer's stop
    void report_read_end() {
        if (!this->stopping()) {
            this->report_closed(CloseReason::PEER_CLOSED);
        }
    }

    DialTarget target_;
    /// Given by resume_inbound(), taken by the reader holding a message
    SemaphoreHandle_t resume_{xSemaphoreCreateBinary()};
    /// Writer task: the reader was started, and shut() waits for its end
    bool reader_started_{false};
    /// Set by the writer's shut(): the reader stops at its next look
    std::atomic<bool> stopping_{false};
    /// Set by the reader as it ends
    std::atomic<bool> reader_done_{false};
    /// Reader task only: the bytes read and not yet parsed, buffer_[buffer_begin_, buffer_end_)
    std::array<uint8_t, READ_BUFFER_BYTES> buffer_{};
    size_t buffer_begin_{0};
    size_t buffer_end_{0};
    /// Reader task only: now_us() at the last read that returned
    int64_t rx_us_{0};
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
          link_(make_shared_external<DialLink>(loop.impl().core(), settings, target)),
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
        // The reader task bounds each connect and upgrade; this bounds the whole dial
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
    SharedPtr<DialLink> link_;
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
