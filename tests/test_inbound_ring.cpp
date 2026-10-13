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

/// @file test_inbound_ring.cpp
/// @brief The hand-off from the network threads to core: messages in push order with their
/// stamps, a drain per message and event in its place, refusal and room, the wrap of the payload
/// storage, a close after the messages before it, and a producer thread against a draining one

#include "inbound_ring.h"
#include <gtest/gtest.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::CloseReason;
using sendspin::server::InboundEndpoint;
using sendspin::server::InboundRing;
using sendspin::server::MessageKind;
using Event = InboundEndpoint::Event;
using Bytes = std::vector<uint8_t>;

Bytes bytes_of(size_t size, uint8_t seed) {
    Bytes out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(seed + (i * 31));
    }
    return out;
}

// Records what the ring delivers to it, as one log shared with the other endpoints
class Endpoint : public InboundEndpoint {
public:
    struct Message {
        std::string endpoint;
        MessageKind kind;
        Bytes payload;
        int64_t rx_us;
    };

    Endpoint(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(log) {}

    std::vector<Message> messages;
    /// Runs after each message is logged
    std::function<void()> message_hook;

protected:
    void deliver_message(MessageKind kind, std::span<const uint8_t> payload,
                         int64_t rx_us) override {
        this->messages.push_back({this->name_, kind, {payload.begin(), payload.end()}, rx_us});
        this->log_.push_back(this->name_ + " message " + std::to_string(rx_us));
        if (this->message_hook) {
            this->message_hook();
        }
    }

    void deliver_writable() override {
        this->log_.push_back(this->name_ + " writable");
    }

    void deliver_write_done() override {
        this->log_.push_back(this->name_ + " write done");
    }

    void deliver_closed(CloseReason reason) override {
        this->log_.push_back(this->name_ + " closed " + std::to_string(static_cast<int>(reason)));
    }

private:
    std::string name_;
    std::vector<std::string>& log_;
};

struct Fixture {
    explicit Fixture(size_t bytes = 1024, size_t messages = 16)
        : ring(
              bytes, messages, [this](uint64_t place) { this->wakes.push_back(place); },
              [this] { ++this->rooms; }) {}

    std::shared_ptr<Endpoint> endpoint(const std::string& name) {
        return std::make_shared<Endpoint>(name, this->log);
    }

    std::vector<std::string> log;
    std::vector<uint64_t> wakes;
    int rooms{0};
    InboundRing ring;
};

constexpr uint64_t ALL = std::numeric_limits<uint64_t>::max();

TEST(InboundRing, DeliversMessagesInPushOrderWithTheirStamps) {
    Fixture f;
    auto a = f.endpoint("a");
    auto b = f.endpoint("b");
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(10, 1), 100));
    ASSERT_TRUE(f.ring.push(b, MessageKind::BINARY, bytes_of(300, 2), 101));
    ASSERT_TRUE(f.ring.push(a, MessageKind::BINARY, {}, 102));
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(1, 3), 103));
    f.ring.drain(ALL);

    EXPECT_EQ(f.log, (std::vector<std::string>{"a message 100", "b message 101", "a message 102",
                                               "a message 103"}));
    ASSERT_EQ(a->messages.size(), 3U);
    EXPECT_EQ(a->messages[0].kind, MessageKind::TEXT);
    EXPECT_EQ(a->messages[0].payload, bytes_of(10, 1));
    EXPECT_EQ(a->messages[1].kind, MessageKind::BINARY);
    EXPECT_TRUE(a->messages[1].payload.empty());
    EXPECT_EQ(a->messages[2].payload, bytes_of(1, 3));
    ASSERT_EQ(b->messages.size(), 1U);
    EXPECT_EQ(b->messages[0].kind, MessageKind::BINARY);
    EXPECT_EQ(b->messages[0].payload, bytes_of(300, 2));
}

// Each message and event asks for a drain with its place, and a drain delivers only what came up
// to its place, so core takes each after what was posted to it before it came
TEST(InboundRing, DrainsEachMessageAndEventInItsPlace) {
    Fixture f;
    auto a = f.endpoint("a");
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(1, 0), 1));
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(1, 0), 2));
    f.ring.raise(a, Event::WRITABLE);
    EXPECT_EQ(f.wakes, (std::vector<uint64_t>{1, 2, 3}));
    f.ring.drain(1);
    EXPECT_EQ(f.log, (std::vector<std::string>{"a message 1"}));
    f.ring.drain(3);
    EXPECT_EQ(f.log, (std::vector<std::string>{"a message 1", "a message 2", "a writable"}));
    // Control: a drain after its place was taken by a later one delivers nothing
    f.ring.drain(2);
    EXPECT_EQ(f.log.size(), 3U);
}

// A refused push makes every later push wait behind it, also one that would fit, until a drain
// has made room for it; then on_room() is called once
TEST(InboundRing, RefusesPushesWithoutRoomUntilTheDrainMakesRoom) {
    Fixture f(100, 16);
    auto a = f.endpoint("a");
    auto b = f.endpoint("b");
    // Control: a message the size of the storage fits an empty ring
    ASSERT_TRUE(f.ring.push(a, MessageKind::BINARY, bytes_of(100, 0), 1));
    f.ring.drain(ALL);
    ASSERT_TRUE(f.ring.push(a, MessageKind::BINARY, bytes_of(60, 1), 2));
    EXPECT_FALSE(f.ring.push(b, MessageKind::BINARY, bytes_of(60, 2), 3));
    EXPECT_FALSE(f.ring.push(a, MessageKind::BINARY, bytes_of(10, 3), 4)) << "fits, but waits";
    EXPECT_EQ(f.rooms, 0);
    f.ring.drain(ALL);
    EXPECT_EQ(f.rooms, 1);
    ASSERT_TRUE(f.ring.push(b, MessageKind::BINARY, bytes_of(60, 2), 3));
    ASSERT_TRUE(f.ring.push(a, MessageKind::BINARY, bytes_of(10, 3), 4));
    f.ring.drain(ALL);
    EXPECT_EQ(f.rooms, 1);
    EXPECT_EQ(f.log, (std::vector<std::string>{"a message 1", "a message 2", "b message 3",
                                               "a message 4"}));

    // The message count is a cap of its own
    Fixture counted(1000, 2);
    auto c = counted.endpoint("c");
    ASSERT_TRUE(counted.ring.push(c, MessageKind::TEXT, bytes_of(1, 0), 1));
    ASSERT_TRUE(counted.ring.push(c, MessageKind::TEXT, bytes_of(1, 0), 2));
    EXPECT_FALSE(counted.ring.push(c, MessageKind::TEXT, bytes_of(1, 0), 3));
    counted.ring.drain(1);
    EXPECT_EQ(counted.rooms, 1);
    EXPECT_TRUE(counted.ring.push(c, MessageKind::TEXT, bytes_of(1, 0), 3));
}

// Against a model FIFO: messages of random sizes pushed until refused and drained a few at a time
// run the storage past its end and on from its start many times over, and each arrives intact
TEST(InboundRing, KeepsEveryPayloadIntactAcrossTheWrap) {
    constexpr size_t CAPACITY = 997;
    Fixture f(CAPACITY, 64);
    auto a = f.endpoint("a");
    std::mt19937 random(20261011);
    std::uniform_int_distribution<size_t> size_of(0, 300);
    std::uniform_int_distribution<size_t> drain_of(1, 5);
    std::deque<Bytes> expected;
    size_t delivered = 0;
    int64_t stamp = 0;
    for (int round = 0; round < 2000; ++round) {
        for (;;) {
            Bytes payload = bytes_of(size_of(random), static_cast<uint8_t>(stamp));
            if (!f.ring.push(a, MessageKind::BINARY, payload, stamp)) {
                break;
            }
            expected.push_back(std::move(payload));
            ++stamp;
        }
        // Message i is at place i + 1
        f.ring.drain(delivered + drain_of(random));
        for (; delivered < a->messages.size(); ++delivered) {
            ASSERT_FALSE(expected.empty());
            ASSERT_EQ(a->messages[delivered].payload, expected.front()) << "message " << delivered;
            ASSERT_EQ(a->messages[delivered].rx_us, static_cast<int64_t>(delivered));
            expected.pop_front();
        }
    }
    EXPECT_GT(stamp, static_cast<int64_t>(20 * CAPACITY / 150)) << "the storage wrapped";
}

// Each event goes in its own place: a close raised behind a message that came after an earlier
// event of the same connection waits for the message
TEST(InboundRing, DeliversACloseAfterTheMessagesPushedBeforeIt) {
    Fixture f;
    auto a = f.endpoint("a");
    auto b = f.endpoint("b");
    f.ring.raise(a, Event::WRITE_DONE);
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(4, 0), 2));
    f.ring.raise(a, Event::CLOSED, CloseReason::PEER_CLOSED);
    f.ring.drain(ALL);
    EXPECT_EQ(f.log, (std::vector<std::string>{"a write done", "a message 2", "a closed 0"}));

    // Control: no message waiting
    f.log.clear();
    f.ring.raise(b, Event::WRITABLE);
    f.ring.raise(b, Event::CLOSED, CloseReason::WRITE_STALLED);
    f.ring.drain(ALL);
    EXPECT_EQ(f.log, (std::vector<std::string>{"b writable", "b closed 2"}));
}

// What a network thread raises and pushes while core drains keeps its order: a write completed
// before the peer's next message arrived is delivered before that message
TEST(InboundRing, DeliversAnEventBeforeTheMessagesPushedAfterIt) {
    Fixture f;
    auto a = f.endpoint("a");
    auto b = f.endpoint("b");
    ASSERT_TRUE(f.ring.push(a, MessageKind::TEXT, bytes_of(4, 0), 1));
    a->message_hook = [&] {
        f.ring.raise(b, Event::WRITE_DONE);
        ASSERT_TRUE(f.ring.push(b, MessageKind::TEXT, bytes_of(4, 0), 3));
    };
    f.ring.drain(ALL);
    EXPECT_EQ(f.log, (std::vector<std::string>{"a message 1", "b write done", "b message 3"}));
}

// A producer thread pushing as a network thread does, holding a refused message until on_room(),
// against a core thread that drains each place it is asked to: every message arrives once, in
// order, intact
TEST(InboundRing, HandsMessagesFromAProducerThreadToADrainingThread) {
    constexpr int MESSAGES = 20000;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<uint64_t> drains;
    bool room = false;
    std::vector<std::string> log;
    InboundRing ring(
        512, 8,
        [&](uint64_t place) {
            const std::lock_guard<std::mutex> lock(mutex);
            drains.push_back(place);
            changed.notify_all();
        },
        [&] {
            const std::lock_guard<std::mutex> lock(mutex);
            room = true;
            changed.notify_all();
        });
    auto a = std::make_shared<Endpoint>("a", log);

    std::thread producer([&] {
        for (int i = 0; i < MESSAGES; ++i) {
            const Bytes payload = bytes_of(static_cast<size_t>(i % 200), static_cast<uint8_t>(i));
            while (!ring.push(a, MessageKind::BINARY, payload, i)) {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return room; });
                room = false;
            }
        }
        ring.raise(a, Event::CLOSED, CloseReason::PEER_CLOSED);
    });
    while (log.empty() || log.back() != "a closed 0") {
        uint64_t place = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return !drains.empty(); });
            place = drains.front();
            drains.pop_front();
        }
        ring.drain(place);
    }
    producer.join();

    ASSERT_EQ(a->messages.size(), static_cast<size_t>(MESSAGES));
    for (int i = 0; i < MESSAGES; ++i) {
        ASSERT_EQ(a->messages[i].rx_us, i);
        ASSERT_EQ(a->messages[i].payload,
                  bytes_of(static_cast<size_t>(i % 200), static_cast<uint8_t>(i)));
    }
}

}  // namespace
