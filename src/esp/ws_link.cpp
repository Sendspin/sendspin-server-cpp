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

#include "ws_link.h"

#include "clock.h"
#include "esp_random.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "thread_setup.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <utility>

namespace sendspin::server {

namespace {

/// How long an ending writer task waits between looks at a wake_writer() still in progress
constexpr TickType_t WAKING_WRITER_POLL_TICKS = 1;
constexpr size_t STATUS_CODE_BYTES = 2;
constexpr int64_t US_PER_MS = 1000;
/// The keystreams of the core task's and the frame lock holder's masking keys
constexpr uint32_t CORE_KEY_STREAM = 0;
constexpr uint32_t FRAME_KEY_STREAM = 1;

void skip_sent(msghdr& message, size_t sent) {
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

TransportSettings bounded(TransportSettings settings) {
    settings.read_message_max = std::min(settings.read_message_max, MAX_MESSAGE_BYTES);
    return settings;
}

}  // namespace

WsLink::WsLink(SharedPtr<LoopCore> loop, const TransportSettings& settings, bool masks)
    : settings_(bounded(settings)),
      loop_(std::move(loop)),
      masks_(masks),
      hold_(
          [this](MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us) {
              return this->loop().inbound().push(this->endpoint_, kind, payload, rx_us);
          },
          [this] { this->loop().park(this->weak_from_this()); },
          [this](CloseReason reason) {
              this->loop().inbound().raise(this->endpoint_, InboundEndpoint::Event::CLOSED, reason);
          },
          WS_KEPT_MESSAGE_BYTES) {}

WsLink::~WsLink() = default;

// ==============================================================================
// Core task
// ==============================================================================

bool WsLink::start() {
    this->loop().task_started();
    // The task publishes its own handle (writer_entry()); a request made before then is seen by
    // its first look at requests_. A detached pthread, so its stack can be in PSRAM
    // (profile::LINK_WRITER_STACK) and is freed when it ends.
    try {
        start_thread(ThreadRole::LINK_WRITER, [self = this->shared_from_this()]() mutable {
            WsLink::writer_entry(std::move(self));
        }).detach();
    } catch (const std::exception&) {
        this->loop().task_ended();
        return false;
    }
    return true;
}

void WsLink::bind(Wakeup::Handle wake, SharedPtr<InboundEndpoint> endpoint) {
    this->wake_ = std::move(wake);
    this->wake_bound_.store(true, std::memory_order_release);
    this->endpoint_ = std::move(endpoint);
    // A close reported before the bind is raised now
    this->hold_.bind();
    this->signal_core();
}

void WsLink::start_inbound() {
    this->hold_.start();
    this->resume_inbound();
}

void WsLink::request_room() {
    this->request(REQUEST_ROOM);
}

void WsLink::stage_write(MessageKind kind, std::span<const uint8_t> payload) {
    this->write_batch_.append(kind, payload, this->mask_key(this->core_keys_));
}

void WsLink::drop_staged() {
    this->write_batch_.clear();
}

void WsLink::request_write() {
    this->request(REQUEST_WRITE);
}

void WsLink::request_ping() {
    this->request(REQUEST_PING);
}

void WsLink::request_close(std::optional<uint16_t> code) {
    this->close_code_ = code;
    this->drop_inbound();
    this->request(REQUEST_CLOSE);
}

void WsLink::drop_inbound() {
    this->hold_.drop();
    // A receive task holding a message stops holding it
    this->resume_inbound();
}

std::optional<CloseReason> WsLink::closed() const {
    const int reason = this->closed_.load(std::memory_order_acquire);
    if (reason < 0) {
        return std::nullopt;
    }
    return static_cast<CloseReason>(reason);
}

// ==============================================================================
// Receive task
// ==============================================================================

bool WsLink::begin_frame(uint8_t opcode, size_t payload_bytes) {
    if (this->dropping_) {
        return false;
    }
    const WsFrameStep step =
        this->assembly_.begin(opcode, payload_bytes, this->settings_.read_message_max);
    if (step == WsFrameStep::VIOLATION) {
        this->dropping_ = true;
        this->fail_protocol();
        return false;
    }
    if (step == WsFrameStep::OPENS && this->hold_.begin_message() == nullptr) {
        this->dropping_ = true;
        this->report_closed(CloseReason::FAILED);
        return false;
    }
    this->hold_.message().resize(this->assembly_.message_bytes() + payload_bytes);
    return true;
}

bool WsLink::end_frame(bool fin, int64_t rx_us) {
    if (this->dropping_) {
        return false;
    }
    if (!this->assembly_.end(fin)) {
        return true;
    }
    if (!ws_message_valid(this->assembly_.kind(),
                          std::span<const uint8_t>(this->hold_.message().data(),
                                                   this->assembly_.message_bytes()))) {
        this->dropping_ = true;
        this->fail_protocol(close_code::INVALID_PAYLOAD);
        return false;
    }
    const InboundHold::Outcome offered =
        this->hold_.offer(this->assembly_.kind(), this->assembly_.message_bytes(), rx_us);
    if (offered.result == InboundHold::Result::HELD && !this->hold_receiving(offered.generation)) {
        this->dropping_ = true;
        return false;
    }
    return true;
}

void WsLink::report_connected(std::string remote) {
    this->remote_endpoint_ = std::move(remote);
    this->connected_.store(true, std::memory_order_release);
    this->signal_core();
    this->wake_writer();
}

void WsLink::report_closed(CloseReason reason) {
    int open = -1;
    this->closed_.compare_exchange_strong(open, static_cast<int>(reason),
                                          std::memory_order_acq_rel);
    this->hold_.report_closed(reason);
    this->wake_writer();
}

void WsLink::raise(InboundEndpoint::Event event) {
    this->loop().inbound().raise(this->endpoint_, event);
}

void WsLink::ask_pong() {
    this->request(REQUEST_PONG);
}

void WsLink::store_ping(const uint8_t* payload, size_t len) {
    const std::lock_guard<std::mutex> lock(this->socket_mutex_);
    std::memcpy(this->pong_.data(), payload, len);
    this->pong_bytes_ = len;
}

bool WsLink::answer_close(const uint8_t* payload, size_t len) {
    if (const std::optional<uint16_t> fault =
            ws_close_payload_fault(std::span<const uint8_t>(payload, len))) {
        this->fail_protocol(*fault);
        return false;
    }
    this->close_within_slice(payload, std::min(len, STATUS_CODE_BYTES));
    this->report_closed(CloseReason::PEER_CLOSED);
    return true;
}

void WsLink::fail_protocol(uint16_t code) {
    const std::array<uint8_t, STATUS_CODE_BYTES> body{static_cast<uint8_t>(code >> 8U),
                                                      static_cast<uint8_t>(code)};
    this->close_within_slice(body.data(), body.size());
    this->report_closed(CloseReason::PROTOCOL_VIOLATION);
}

void WsLink::close_within_slice(const uint8_t* body, size_t len) {
    const int64_t deadline_us = now_us() + static_cast<int64_t>(WS_SEND_SLICE_MS) * US_PER_MS;
    std::unique_lock<std::timed_mutex> lock(this->frame_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::milliseconds(WS_SEND_SLICE_MS))) {
        return;
    }
    this->write_frame(ws_opcode::CLOSE, body, len, deadline_us);
}

void WsLink::signal_core() {
    // One before the bind is signalled by the bind
    if (this->wake_bound_.load(std::memory_order_acquire)) {
        Wakeup::signal(this->wake_);
    }
}

void WsLink::wake_writer() {
    // Sequentially consistent with writer_entry()'s clear and count: a writer that read the count
    // as 0 after clearing the handle is one this load reads as null
    this->waking_writer_.fetch_add(1);
    if (TaskHandle_t writer = this->writer_.load()) {
        xTaskNotifyGive(writer);
    }
    this->waking_writer_.fetch_sub(1);
}

void WsLink::request(uint32_t bits) {
    this->requests_.fetch_or(bits, std::memory_order_acq_rel);
    this->wake_writer();
}

// ==============================================================================
// Writer task
// ==============================================================================

void WsLink::writer_entry(SharedPtr<WsLink> self) {
    self->writer_.store(xTaskGetCurrentTaskHandle());
    self->writer_main();
    self->writer_.store(nullptr);
    // A wake_writer() that read the handle has not notified yet: the task stays until it has
    while (self->waking_writer_.load() != 0) {
        vTaskDelay(WAKING_WRITER_POLL_TICKS);
    }
    self->loop().task_ended();
}

void WsLink::writer_main() {
    if (this->masks_) {
        // One RNG draw per connection: esp_random() spaces its reads, so a draw per frame would
        // stall the core task
        std::array<uint8_t, CHACHA20_KEY_BYTES> seed{};
        esp_fill_random(seed.data(), seed.size());
        this->core_keys_.seed(seed, CORE_KEY_STREAM);
        this->frame_keys_.seed(seed, FRAME_KEY_STREAM);
    }
    if (this->open()) {
        constexpr uint32_t OPERATIONS = REQUEST_ROOM | REQUEST_WRITE | REQUEST_PING | REQUEST_PONG;
        while (!this->close_requested()) {
            const uint32_t requests =
                this->requests_.fetch_and(~OPERATIONS, std::memory_order_acq_rel) & OPERATIONS;
            if (requests == 0) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            if ((requests & REQUEST_PONG) != 0) {
                this->send_pong();
            }
            if ((requests & REQUEST_PING) != 0) {
                this->send_ping();
            }
            if ((requests & REQUEST_WRITE) != 0) {
                const bool sent = this->send_batch(this->write_batch_);
                this->write_batch_.clear();
                if (sent) {
                    this->raise(InboundEndpoint::Event::WRITE_DONE);
                } else {
                    // The close is the write's outcome
                    this->report_closed(CloseReason::PEER_CLOSED);
                }
            }
            if ((requests & REQUEST_ROOM) != 0) {
                this->wait_for_room();
            }
        }
    }
    // close_code_ is set before REQUEST_CLOSE, and only then read
    this->shut(this->close_requested() ? this->close_code_ : std::nullopt);
}

bool WsLink::serve_control() {
    const uint32_t requests =
        this->requests_.fetch_and(~(REQUEST_PING | REQUEST_PONG), std::memory_order_acq_rel);
    if ((requests & REQUEST_PONG) != 0) {
        this->send_pong();
    }
    if ((requests & REQUEST_PING) != 0) {
        this->send_ping();
    }
    return !this->close_requested();
}

void WsLink::wait_for_room() {
    while (this->serve_control() && !this->closed()) {
        const int polled = this->poll_room(WS_ROOM_POLL_MS);
        if (polled > 0) {
            this->raise(InboundEndpoint::Event::WRITABLE);
            return;
        }
        if (polled < 0) {
            this->report_closed(CloseReason::PEER_CLOSED);
            return;
        }
    }
}

int WsLink::poll_room(int timeout_ms) {
    if (this->fd_closed_.load(std::memory_order_acquire)) {
        return -1;
    }
    // Outside the socket mutex: lwIP holds a socket for the whole of a select() on it
    // (LWIP_NETCONN_FULLDUPLEX), so a close meanwhile frees the descriptor only after it; one
    // closed before the select() starts fails it, or, reused, answers spuriously, and the write
    // that follows checks fd_closed_ under the socket mutex
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(this->fd_, &writable);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    return select(this->fd_ + 1, nullptr, &writable, nullptr, &timeout);
}

bool WsLink::send_batch(const WsBatch& batch) {
    const std::lock_guard<std::timed_mutex> lock(this->frame_mutex_);
    if (this->close_sent_ || this->frame_broken_) {
        return false;
    }
    const std::span<const uint8_t> bytes = batch.bytes();
    std::array<iovec, 1> parts{};
    parts[0].iov_base = const_cast<uint8_t*>(bytes.data());
    parts[0].iov_len = bytes.size();
    return this->send_parts(parts.data(), parts.size(), now_us() + this->settings_.write_stall_us);
}

bool WsLink::send_ping() {
    return this->send_frame(ws_opcode::PING, nullptr, 0);
}

bool WsLink::send_pong() {
    std::array<uint8_t, WS_CONTROL_PAYLOAD_MAX> payload{};
    size_t bytes = 0;
    {
        const std::lock_guard<std::mutex> lock(this->socket_mutex_);
        bytes = this->pong_bytes_;
        std::memcpy(payload.data(), this->pong_.data(), bytes);
    }
    return this->send_frame(ws_opcode::PONG, payload.data(), bytes);
}

bool WsLink::send_close(uint16_t code) {
    const std::array<uint8_t, STATUS_CODE_BYTES> body{static_cast<uint8_t>(code >> 8U),
                                                      static_cast<uint8_t>(code)};
    return this->send_frame(ws_opcode::CLOSE, body.data(), body.size());
}

void WsLink::shutdown_socket(int how) {
    const std::lock_guard<std::mutex> lock(this->socket_mutex_);
    if (this->fd_ >= 0 && !this->fd_closed_) {
        shutdown(this->fd_, how);
    }
}

std::optional<WsMaskKey> WsLink::mask_key(WsMaskKeys& keys) const {
    if (!this->masks_) {
        return std::nullopt;
    }
    return keys.next();
}

bool WsLink::send_frame(uint8_t opcode, const uint8_t* data, size_t len) {
    const std::lock_guard<std::timed_mutex> lock(this->frame_mutex_);
    return this->write_frame(opcode, data, len, now_us() + this->settings_.write_stall_us);
}

bool WsLink::write_frame(uint8_t opcode, const uint8_t* data, size_t len, int64_t deadline_us) {
    // RFC 6455 section 5.5.1: nothing follows a close frame
    if (this->close_sent_ || this->frame_broken_) {
        return false;
    }
    if (opcode == ws_opcode::CLOSE) {
        this->close_sent_ = true;
    }
    std::array<uint8_t, WS_FRAME_HEADER_MAX> header{};
    const std::optional<WsMaskKey> mask = this->mask_key(this->frame_keys_);
    const size_t header_bytes = ws_frame_header(opcode, len, header, mask);
    std::array<uint8_t, WS_CONTROL_PAYLOAD_MAX> masked{};
    if (mask && len > 0) {
        ws_mask_copy(masked.data(), data, len, *mask);
        data = masked.data();
    }
    // Header and payload in one call, so they leave in as few segments as fit
    std::array<iovec, 2> parts{};
    parts[0].iov_base = header.data();
    parts[0].iov_len = header_bytes;
    parts[1].iov_base = const_cast<uint8_t*>(data);
    parts[1].iov_len = len;
    return this->send_parts(parts.data(), len > 0 ? 2 : 1, deadline_us);
}

bool WsLink::send_parts(iovec* parts, size_t count, int64_t deadline_us) {
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
            std::min<int64_t>((left_us / US_PER_MS) + 1, static_cast<int64_t>(WS_SEND_SLICE_MS)));
        if (this->poll_room(wait_ms) < 0) {
            return fail();
        }
    }
    return true;
}

// ==============================================================================
// Helpers
// ==============================================================================

std::string peer_endpoint(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (fd < 0 || getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {};
    }
    // INET6_ADDRSTRLEN, which lwIP defines only with IPv6
    constexpr size_t ADDRESS_TEXT_BYTES = 46;
    char text[ADDRESS_TEXT_BYTES] = {};
    if (address.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&address);
        inet_ntop(AF_INET, &v4->sin_addr, text, sizeof(text));
        return std::string(text) + ":" + std::to_string(ntohs(v4->sin_port));
    }
#if LWIP_IPV6
    if (address.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&address);
        const uint16_t port = ntohs(v6->sin6_port);
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            inet_ntop(AF_INET, &v6->sin6_addr.s6_addr[12], text, sizeof(text));
            return std::string(text) + ":" + std::to_string(port);
        }
        inet_ntop(AF_INET6, &v6->sin6_addr, text, sizeof(text));
        return "[" + std::string(text) + "]:" + std::to_string(port);
    }
#endif
    return {};
}

}  // namespace sendspin::server
