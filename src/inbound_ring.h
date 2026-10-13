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

/// @file inbound_ring.h
/// @brief InboundRing: how a transport's network threads hand received messages, with the receive
/// stamps they took, and connection events to the core thread. Independent of any transport, so
/// every transport hands off the same way.

#pragma once

#include "memory.h"
#include "shared.h"
#include "transport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief The core thread's half of one connection, which an InboundRing delivers to. A transport
/// derives its connection's core state from it; the ring holds it by shared_ptr while a message or
/// event for it is held.
class InboundEndpoint {
public:
    /// @brief Events a network thread raises for a connection
    enum class Event : uint8_t {
        WRITABLE,    ///< A wait for room ended with room
        WRITE_DONE,  ///< The write in flight completed
        CLOSED,      ///< The connection failed or the peer closed it; raised at most once, last
    };

    InboundEndpoint() = default;
    virtual ~InboundEndpoint() = default;
    InboundEndpoint(const InboundEndpoint&) = delete;
    InboundEndpoint& operator=(const InboundEndpoint&) = delete;

protected:
    friend class InboundRing;

    /// @brief One message, valid only during the call
    virtual void deliver_message(MessageKind kind, std::span<const uint8_t> payload,
                                 int64_t rx_us) = 0;
    virtual void deliver_writable() = 0;
    virtual void deliver_write_done() = 0;
    virtual void deliver_closed(CloseReason reason) = 0;
};

/// @brief A bounded FIFO of received messages, copied in by the threads that read them, and the
/// events they raise, delivered on the core thread in the order they came, under one mutex that is
/// a leaf. A message's bytes and its endpoint stay in the ring until core has delivered it, so a
/// message is copied once. Each message and event asks core for a drain of its own, so core takes
/// it after what was posted to it before it came, as it would a read's completion. A push that
/// finds no room is refused and its producer holds the message and stops reading that connection
/// (TCP back-pressure); every push is then refused until the drains have made room for the largest
/// one refused, and on_room() tells the producers to push again. Events are never refused and take
/// no room; a connection has at most a wait or a write outstanding and one close, so at most two.
class InboundRing {
public:
    /// @param bytes the payload storage; a message of up to this size fits once the ring is empty
    /// @param messages the most messages held at once
    /// @param wake called for each message pushed and event raised, on the pushing thread outside
    /// the mutex, with its place in the ring's order; posts drain(place) to core. The place is all
    /// it carries, so a post that holds the ring and the place needs no memory of its own
    /// @param on_room called on core when pushes are taken again after one was refused
    InboundRing(size_t bytes, size_t messages, std::function<void(uint64_t place)> wake,
                std::function<void()> on_room);
    InboundRing(const InboundRing&) = delete;
    InboundRing& operator=(const InboundRing&) = delete;

    /// @brief Copies a message for core. Any thread; never calls an endpoint.
    /// @param rx_us the receive stamp, read when the message's last byte was in
    /// @return false when refused for room: the producer keeps the message and pushes it again
    /// after on_room()
    bool push(const SharedPtr<InboundEndpoint>& to, MessageKind kind,
              std::span<const uint8_t> payload, int64_t rx_us);

    /// @brief Raises an event for core. Any thread; CLOSED after the endpoint's last push.
    void raise(const SharedPtr<InboundEndpoint>& to, InboundEndpoint::Event event,
               CloseReason reason = CloseReason::FAILED);

    /// @brief Delivers, in the order they came, the messages and events up to place through that
    /// earlier drains left. Core thread; an endpoint called may call into its transport, which may
    /// push and raise.
    void drain(uint64_t through);

    /// @brief Drops every message and event held. Core thread, once no producer pushes.
    void clear();

private:
    struct Record {
        SharedPtr<InboundEndpoint> to;
        size_t offset{0};
        size_t size{0};
        int64_t rx_us{0};
        uint64_t place{0};
        MessageKind kind{MessageKind::TEXT};
    };

    struct Raised {
        SharedPtr<InboundEndpoint> to;
        uint64_t place;
        InboundEndpoint::Event event;
        CloseReason reason;
    };

    /// Where a message of size bytes would go, or capacity_ when it does not fit. Under the mutex.
    size_t place(size_t size) const;
    void deliver_taken();

    const size_t capacity_;
    const std::function<void(uint64_t place)> wake_;
    const std::function<void()> on_room_;
    /// In PSRAM where there is one: copied in and out once per message
    PlacedArray<uint8_t> bytes_;

    std::mutex mutex_;
    /// Fixed size, used as a circular FIFO from head_
    std::vector<Record> records_;
    size_t head_{0};
    size_t count_{0};
    /// The oldest message's offset and the next free byte; wrapped_ when the messages run past the
    /// end of the storage and on from its start
    size_t byte_head_{0};
    size_t byte_tail_{0};
    bool wrapped_{false};
    /// A push was refused: pushes are refused until refused_bytes_ fit
    bool refused_{false};
    size_t refused_bytes_{0};
    /// The last place given to a message or event
    uint64_t places_{0};
    /// The events raised and not yet taken, in place order
    std::vector<Raised> raised_;
    /// Core only: the events a drain is delivering
    std::vector<Raised> taking_;
};

}  // namespace sendspin::server
