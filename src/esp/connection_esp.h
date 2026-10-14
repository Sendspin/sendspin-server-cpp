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

/// @file connection_esp.h
/// @brief ESP-IDF's Connection::Impl: the core task's half of a connection over a WsLink. It keeps
/// the write invariant, the write-stall timer and the idle timer, and turns what the inbound ring
/// delivers (through its LinkEndpoint) into ConnectionHandler events; its Wakeup reports the
/// link's opening and room after a small write.

#pragma once

#include "shared.h"
#include "transport.h"
#include "ws_link.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace sendspin::server {

class LinkEndpoint;

class Connection::Impl : public EnableSharedFromThis<Impl> {
public:
    Impl(EventLoop& loop, SharedPtr<WsLink> link, const TransportSettings& settings);
    ~Impl();

    /// @brief Binds the link to this connection's Wakeup and endpoint; once made
    static SharedPtr<Impl> make(EventLoop& loop, SharedPtr<WsLink> link,
                                const TransportSettings& settings);
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    /// @brief Until the link opens or closes, its reports go to on_open instead: nullopt once
    /// open, else why it closed. Called at most once.
    void await_open(std::function<void(std::optional<CloseReason>)> on_open);

    void start(ConnectionHandler& handler);
    bool wait_writable();
    bool can_write() const {
        return !this->closed_ && this->room_ &&
               write_batch_has_room(this->batch_count_, this->batch_bytes_);
    }
    bool write(MessageKind kind, std::span<const uint8_t> payload);
    void close(uint16_t code);

    bool is_open() const {
        return !this->closed_;
    }

    const std::string& remote_endpoint() const {
        return this->link_->remote_endpoint();
    }

private:
    friend class LinkEndpoint;

    void on_wake();
    void on_message(MessageKind kind, std::span<const uint8_t> payload, int64_t rx_us);
    void on_wait();
    /// Starts the write of the messages on_writable() took, unless it took none or closed
    void start_write();
    void on_write();
    void on_closed(CloseReason reason);
    void fail(CloseReason reason);
    void stop_timers();
    void start_io();
    void on_stall();
    void arm_idle(int64_t now);
    void on_idle();

    SharedPtr<WsLink> link_;
    /// What the inbound ring delivers to; it may outlive this connection
    SharedPtr<LinkEndpoint> endpoint_;
    Wakeup wakeup_;
    Timer stall_timer_;
    Timer idle_timer_;
    ConnectionHandler* handler_{nullptr};
    std::function<void(std::optional<CloseReason>)> on_open_;
    int64_t write_stall_us_;
    int64_t idle_timeout_us_;
    // When the wait or write outstanding started
    int64_t io_started_us_{0};
    // The idle timer counts from the last inbound frame, or from start() before one
    int64_t idle_since_us_{0};
    // The last inbound frame a ping was sent after
    int64_t pinged_after_us_{-1};
    // The stall timer is armed
    bool stall_armed_{false};
    // The last write, no larger than ROOM_AFTER_WRITE_MAX_BYTES, completed and no wait has started
    // since
    bool wrote_{false};
    bool small_write_{false};
    // on_write_done() is running; a wait it starts after a completed write is reported once it
    // returns
    bool in_write_done_{false};
    bool room_pending_{false};
    // A wait started after a completed write, reported from the next on_wake()
    bool room_deferred_{false};
    // A close the ring delivered before start(), reported from the on_wake() start() signals
    std::optional<CloseReason> closed_before_start_;
    bool closed_{false};
    bool wait_in_flight_{false};
    bool write_in_flight_{false};
    // The messages staged in the link for the write being built or in flight, and their bytes; 0
    // once it completes
    size_t batch_count_{0};
    size_t batch_bytes_{0};
    // True only while on_writable() runs
    bool room_{false};
};

/// @brief What the inbound ring holds for a connection: forwards to its Connection::Impl on the
/// core task, which it does not own, so a ring the loop leaves behind keeps no core state alive
class LinkEndpoint final : public InboundEndpoint {
public:
    explicit LinkEndpoint(Connection::Impl& connection) : connection_(&connection) {}

    /// @brief Core task, as the connection is destroyed: what the ring delivers later is dropped
    void detach() {
        this->connection_ = nullptr;
    }

protected:
    void deliver_message(MessageKind kind, std::span<const uint8_t> payload,
                         int64_t rx_us) override;
    void deliver_writable() override;
    void deliver_write_done() override;
    void deliver_closed(CloseReason reason) override;

private:
    /// Null once detached
    Connection::Impl* connection_;
};

}  // namespace sendspin::server
