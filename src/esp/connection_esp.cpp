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

#include "connection_esp.h"

#include "clock.h"
#include "memory.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

Connection::Impl::Impl(EventLoop& loop, SharedPtr<WsLink> link, const TransportSettings& settings)
    : link_(std::move(link)),
      wakeup_(loop, [this] { this->on_wake(); }),
      stall_timer_(loop),
      idle_timer_(loop),
      write_stall_us_(settings.write_stall_us),
      idle_timeout_us_(settings.idle_timeout_us) {}

Connection::Impl::~Impl() {
    if (this->endpoint_) {
        this->endpoint_->detach();
    }
    if (!this->closed_) {
        this->link_->request_close(std::nullopt);
    }
}

SharedPtr<Connection::Impl> Connection::Impl::make(EventLoop& loop, SharedPtr<WsLink> link,
                                                   const TransportSettings& settings) {
    auto impl = make_shared_external<Impl>(loop, std::move(link), settings);
    impl->endpoint_ = make_shared_external<LinkEndpoint>(*impl);
    impl->link_->bind(impl->wakeup_.handle(), impl->endpoint_);
    return impl;
}

void Connection::Impl::await_open(std::function<void(std::optional<CloseReason>)> on_open) {
    this->on_open_ = std::move(on_open);
    this->wakeup_.signal();
}

void Connection::Impl::start(ConnectionHandler& handler) {
    this->handler_ = &handler;
    if (this->closed_) {
        // A close delivered before the start is reported from a handler of its own
        if (this->closed_before_start_) {
            this->wakeup_.signal();
        }
        return;
    }
    const int64_t now = now_us();
    this->idle_since_us_ = now;
    this->arm_idle(now);
    // Messages that came before the start, held by the link, are pushed now
    this->link_->start_inbound();
}

bool Connection::Impl::wait_writable() {
    if (this->closed_ || this->handler_ == nullptr || this->wait_in_flight_ ||
        this->write_in_flight_ || this->batch_count_ > 0) {
        return false;
    }
    this->room_ = false;
    this->wait_in_flight_ = true;
    // lwIP has no TCP_NOTSENT_LOWAT, so a small write that completed leaves room: it is reported
    // with no wait on the socket, once the write's on_write_done() has returned, or else from the
    // next on_wake()
    if (std::exchange(this->wrote_, false)) {
        if (this->in_write_done_) {
            this->room_pending_ = true;
        } else {
            // The stall timer may still be armed: an earlier I/O's start must not count
            this->io_started_us_ = now_us();
            this->room_deferred_ = true;
            this->wakeup_.signal();
        }
        return true;
    }
    this->start_io();
    this->link_->request_room();
    return true;
}

bool Connection::Impl::write(MessageKind kind, std::span<const uint8_t> payload) {
    if (!this->can_write()) {
        return false;
    }
    this->link_->stage_write(kind, payload);
    ++this->batch_count_;
    this->batch_bytes_ += payload.size();
    return true;
}

void Connection::Impl::start_write() {
    if (this->batch_count_ == 0) {
        return;
    }
    // The writer may be past its check for the close already, so a write requested now could
    // still go out
    if (this->closed_) {
        this->link_->drop_staged();
        this->batch_count_ = 0;
        this->batch_bytes_ = 0;
        return;
    }
    this->small_write_ = this->batch_bytes_ <= ROOM_AFTER_WRITE_MAX_BYTES;
    this->write_in_flight_ = true;
    this->start_io();
    this->link_->request_write();
}

void Connection::Impl::close(uint16_t code) {
    if (this->closed_) {
        return;
    }
    this->closed_ = true;
    this->room_ = false;
    this->stop_timers();
    // A close frame would queue behind the write in flight, which may never complete
    const bool graceful =
        !this->write_in_flight_ && this->link_->connected() && !this->link_->closed();
    this->link_->request_close(graceful ? std::optional<uint16_t>(code) : std::nullopt);
}

void Connection::Impl::on_wake() {
    // A handler may drop the last handle
    const SharedPtr<Impl> self = this->shared_from_this();
    if (this->on_open_) {
        // A close before the opening comes as on_closed()
        if (this->link_->connected()) {
            const std::function<void(std::optional<CloseReason>)> on_open =
                std::move(this->on_open_);
            this->on_open_ = nullptr;
            on_open(std::nullopt);
        }
        return;
    }
    if (this->closed_before_start_ && this->handler_ != nullptr) {
        const CloseReason reason = *this->closed_before_start_;
        this->closed_before_start_.reset();
        ConnectionHandler* handler = std::exchange(this->handler_, nullptr);
        handler->on_closed(reason);
        return;
    }
    if (std::exchange(this->room_deferred_, false)) {
        this->on_wait();
    }
}

void Connection::Impl::on_message(MessageKind kind, std::span<const uint8_t> payload,
                                  int64_t rx_us) {
    if (this->closed_ || this->handler_ == nullptr) {
        return;
    }
    this->idle_since_us_ = std::max(this->idle_since_us_, rx_us);
    this->handler_->on_message(kind, payload, rx_us);
}

void Connection::Impl::on_wait() {
    this->wait_in_flight_ = false;
    if (this->closed_) {
        return;
    }
    this->room_ = true;
    this->handler_->on_writable();
    this->room_ = false;
    this->start_write();
}

void Connection::Impl::on_write() {
    this->write_in_flight_ = false;
    this->batch_count_ = 0;
    this->batch_bytes_ = 0;
    if (this->closed_) {
        return;
    }
    this->wrote_ = this->small_write_;
    this->in_write_done_ = true;
    this->handler_->on_write_done();
    this->in_write_done_ = false;
    if (std::exchange(this->room_pending_, false)) {
        this->on_wait();
    }
}

void Connection::Impl::on_closed(CloseReason reason) {
    if (this->on_open_) {
        // A dial that did not open
        const std::function<void(std::optional<CloseReason>)> on_open = std::move(this->on_open_);
        this->on_open_ = nullptr;
        on_open(reason);
        return;
    }
    if (this->closed_) {
        return;
    }
    if (this->handler_ == nullptr) {
        this->closed_ = true;
        this->stop_timers();
        this->link_->request_close(std::nullopt);
        this->closed_before_start_ = reason;
        return;
    }
    this->fail(reason);
}

void Connection::Impl::fail(CloseReason reason) {
    this->closed_ = true;
    this->room_ = false;
    this->stop_timers();
    this->link_->request_close(std::nullopt);
    ConnectionHandler* handler = std::exchange(this->handler_, nullptr);
    if (handler != nullptr) {
        handler->on_closed(reason);
    }
}

void Connection::Impl::stop_timers() {
    this->stall_timer_.cancel();
    this->stall_armed_ = false;
    this->idle_timer_.cancel();
}

// The write stall: one wait or write outstanding for write_stall_us_. The timer is armed when I/O
// starts with none armed and checks the I/O outstanding when it fires, so I/O that keeps
// completing arms it about once per write_stall_us_, not once per wait and write.
void Connection::Impl::start_io() {
    this->io_started_us_ = now_us();
    if (!this->stall_armed_) {
        this->stall_armed_ = true;
        this->stall_timer_.start(this->io_started_us_ + this->write_stall_us_,
                                 [this] { this->on_stall(); });
    }
}

void Connection::Impl::on_stall() {
    this->stall_armed_ = false;
    if (this->closed_ || (!this->wait_in_flight_ && !this->write_in_flight_)) {
        return;
    }
    if (now_us() - this->io_started_us_ >= this->write_stall_us_) {
        this->fail(CloseReason::WRITE_STALLED);
        return;
    }
    this->stall_armed_ = true;
    this->stall_timer_.start(this->io_started_us_ + this->write_stall_us_,
                             [this] { this->on_stall(); });
}

// The idle timeout (TransportSettings::idle_timeout_us): a ping once half of it passes without an
// inbound frame, and the close once all of it does
void Connection::Impl::arm_idle(int64_t now) {
    if (this->idle_timeout_us_ <= 0) {
        return;
    }
    const int64_t since = std::max(this->idle_since_us_, this->link_->last_rx_us());
    const int64_t half_us = this->idle_timeout_us_ / 2;
    const int64_t at = now - since >= half_us ? since + this->idle_timeout_us_ : since + half_us;
    this->idle_timer_.start(at, [this] { this->on_idle(); });
}

void Connection::Impl::on_idle() {
    if (this->closed_) {
        return;
    }
    const int64_t now = now_us();
    const int64_t since = std::max(this->idle_since_us_, this->link_->last_rx_us());
    if (now - since >= this->idle_timeout_us_) {
        this->fail(CloseReason::TIMED_OUT);
        return;
    }
    if (now - since >= this->idle_timeout_us_ / 2 && since != this->pinged_after_us_) {
        this->pinged_after_us_ = since;
        this->link_->request_ping();
    }
    this->arm_idle(now);
}

// ==============================================================================
// LinkEndpoint
// ==============================================================================

// A handler may destroy the connection. on_message() touches nothing of it after its handler
// returns; the events are delivered holding it.

void LinkEndpoint::deliver_message(MessageKind kind, std::span<const uint8_t> payload,
                                   int64_t rx_us) {
    if (this->connection_ != nullptr) {
        this->connection_->on_message(kind, payload, rx_us);
    }
}

void LinkEndpoint::deliver_writable() {
    if (this->connection_ != nullptr) {
        this->connection_->shared_from_this()->on_wait();
    }
}

void LinkEndpoint::deliver_write_done() {
    if (this->connection_ != nullptr) {
        this->connection_->shared_from_this()->on_write();
    }
}

void LinkEndpoint::deliver_closed(CloseReason reason) {
    if (this->connection_ != nullptr) {
        this->connection_->shared_from_this()->on_closed(reason);
    }
}

// ==============================================================================
// Connection
// ==============================================================================

Connection::Connection(SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

Connection::~Connection() {
    if (this->impl_) {
        this->impl_->close(close_code::GOING_AWAY);
    }
}

Connection::Connection(Connection&& other) noexcept = default;

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (this->impl_) {
            this->impl_->close(close_code::GOING_AWAY);
        }
        this->impl_ = std::move(other.impl_);
    }
    return *this;
}

void Connection::start(ConnectionHandler& handler) {
    this->impl_->start(handler);
}

bool Connection::wait_writable() {
    return this->impl_->wait_writable();
}

bool Connection::can_write() const {
    return this->impl_->can_write();
}

void Connection::debug_time_reply(const TimeReplyTrace& /*trace*/) {}

bool Connection::write(MessageKind kind, std::span<const uint8_t> payload) {
    return this->impl_->write(kind, payload);
}

void Connection::close(uint16_t code) {
    this->impl_->close(code);
}

bool Connection::is_open() const {
    return this->impl_->is_open();
}

const std::string& Connection::remote_endpoint() const {
    return this->impl_->remote_endpoint();
}

}  // namespace sendspin::server
