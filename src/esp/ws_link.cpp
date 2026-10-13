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

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "thread_setup.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

namespace sendspin::server {

namespace {

/// How long an ending writer task waits between looks at a wake_writer() still in progress
constexpr TickType_t WAKING_WRITER_POLL_TICKS = 1;

TransportSettings bounded(TransportSettings settings) {
    settings.read_message_max = std::min(settings.read_message_max, MAX_MESSAGE_BYTES);
    return settings;
}

}  // namespace

WsLink::WsLink(SharedPtr<LoopCore> loop, const TransportSettings& settings)
    : settings_(bounded(settings)),
      loop_(std::move(loop)),
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

void WsLink::request_write(MessageKind kind, std::span<const uint8_t> payload) {
    this->write_kind_ = kind;
    this->write_bytes_.assign(payload.begin(), payload.end());
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
    if (opcode == ws_opcode::CONTINUATION) {
        if (!this->assembling_) {
            this->dropping_ = true;
            this->report_closed(CloseReason::PROTOCOL_VIOLATION);
            return false;
        }
    } else {
        // A text or binary frame opens a message; one may not open inside another
        if (this->assembling_) {
            this->dropping_ = true;
            this->report_closed(CloseReason::PROTOCOL_VIOLATION);
            return false;
        }
        this->assembling_ = true;
        this->message_kind_ = opcode == ws_opcode::TEXT ? MessageKind::TEXT : MessageKind::BINARY;
        this->message_bytes_ = 0;
        if (this->hold_.begin_message() == nullptr) {
            this->dropping_ = true;
            this->report_closed(CloseReason::FAILED);
            return false;
        }
    }
    if (payload_bytes > this->settings_.read_message_max - this->message_bytes_) {
        this->dropping_ = true;
        this->report_closed(CloseReason::PROTOCOL_VIOLATION);
        return false;
    }
    this->hold_.message().resize(this->message_bytes_ + payload_bytes);
    this->frame_bytes_ = payload_bytes;
    return true;
}

bool WsLink::end_frame(bool fin, int64_t rx_us) {
    if (this->dropping_ || !this->assembling_) {
        return !this->dropping_;
    }
    this->message_bytes_ += this->frame_bytes_;
    if (!fin) {
        return true;
    }
    this->assembling_ = false;
    const InboundHold::Outcome offered =
        this->hold_.offer(this->message_kind_, this->message_bytes_, rx_us);
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
                if (this->send_message(this->write_kind_, this->write_bytes_.data(),
                                       this->write_bytes_.size())) {
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
