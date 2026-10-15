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

/// @file test_send_queues.cpp
/// @brief The per-connection queues: their caps, fragments ahead of everything, and the markers
/// and activations of the control FIFO

#include "fragmentation.h"
#include "send_queues.h"
#include "wire_messages.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::ClientTime;
using sendspin::server::ControlKind;
using sendspin::server::MessageKind;
using sendspin::server::OutboundPlaintext;
using sendspin::server::SendQueueCaps;
using sendspin::server::SendQueues;
using SendChoice = sendspin::server::sched::Choice;
using sendspin::server::sched::RoleSet;
using sendspin::server::sched::StreamRole;
using Bytes = std::vector<uint8_t>;
namespace wire = sendspin::server::test::wire;

constexpr size_t FRAGMENT = 16384;

OutboundPlaintext json_of_size(size_t size) {
    Bytes bytes(size, '{');
    bytes[0] = sendspin::server::MESSAGE_ID_JSON;
    return {MessageKind::BINARY, bytes};
}

// A marker is not queued behind another, since the tail one runs after everything queued; markers
// count as entries, and the drain watch reads how many are queued
TEST(SendQueueMarkers, CoalesceAtTheTailAndCountAsEntries) {
    SendQueueCaps caps;
    caps.control_entries = 3;
    SendQueues queues(caps);
    EXPECT_TRUE(queues.push_marker());
    EXPECT_TRUE(queues.push_marker());
    EXPECT_EQ(queues.markers_queued(), 1U);
    // Control: a message between them lets a second marker in
    EXPECT_TRUE(queues.push_control(json_of_size(4)));
    EXPECT_TRUE(queues.push_marker());
    EXPECT_EQ(queues.markers_queued(), 2U);
    EXPECT_FALSE(queues.push_control(json_of_size(4)));

    EXPECT_EQ(queues.head_kind(), ControlKind::MARKER);
    queues.pop_marker();
    EXPECT_EQ(queues.head_kind(), ControlKind::MESSAGE);
    EXPECT_EQ(queues.markers_queued(), 1U);
}

// A marker evaluates each role against the first activation queued behind it, the roles written
// when there is none; a rekey's frozen activation counts as one
TEST(SendQueueMarkers, SeeTheRolesOfTheFirstActivationBehindThem) {
    const RoleSet written = RoleSet::of(StreamRole::PLAYER);
    struct Queued {
        bool rekey;
        RoleSet roles;
    };
    struct Row {
        const char* name;
        std::vector<Queued> activations;
        RoleSet expected;
    };
    const Row rows[] = {
        {"Control: none queued gives the roles written", {}, written},
        {"one removing the player", {{false, RoleSet{}}}, RoleSet{}},
        // A role removed and re-added still gets its end before the removing activation
        {"the first of two", {{false, RoleSet{}}, {false, written}}, RoleSet{}},
        {"a rekey whose activation removes the player", {{true, RoleSet{}}}, RoleSet{}},
        {"a rekey ahead of an activation", {{true, RoleSet{}}, {false, written}}, RoleSet{}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        SendQueues queues{SendQueueCaps{}};
        ASSERT_TRUE(queues.push_marker());
        ASSERT_TRUE(queues.push_control(json_of_size(4)));
        for (const Queued& queued : row.activations) {
            ASSERT_TRUE(queued.rekey ? queues.push_rekey(json_of_size(4), queued.roles)
                                     : queues.push_activation(json_of_size(4), queued.roles));
            ASSERT_TRUE(queues.push_marker());
        }
        EXPECT_EQ(queues.roles_ahead(written), row.expected);
    }
}

// Taking a rekey pops it and hands back its frozen activation and stream roles, for the session to
// write after the freeze
TEST(SendQueueMarkers, TakeARekeyWithItsFrozenActivation) {
    SendQueues queues{SendQueueCaps{}};
    const OutboundPlaintext activation = json_of_size(40);
    ASSERT_TRUE(queues.push_rekey(activation, RoleSet::of(StreamRole::PLAYER)));
    ASSERT_EQ(queues.head_kind(), ControlKind::REKEY);
    EXPECT_EQ(queues.head_roles(), RoleSet::of(StreamRole::PLAYER));
    EXPECT_EQ(queues.take_rekey().bytes, activation.bytes);
    EXPECT_FALSE(queues.snapshot().control_pending);
    EXPECT_EQ(queues.control_taken(), 1U);
}

// Overflowing a cap is refused, and the caller closes the connection
TEST(SendQueueCaps, RefuseTheEntryThatWouldPassACap) {
    SendQueueCaps caps;
    caps.time_entries = 2;
    caps.control_entries = 2;
    caps.control_bytes = 100;

    SendQueues time(caps);
    EXPECT_TRUE(time.push_time({1, 10}));
    EXPECT_TRUE(time.push_time({2, 20}));
    EXPECT_FALSE(time.push_time({3, 30}));
    // Taking one makes room for one
    EXPECT_EQ(time.take_time().client_transmitted, 1);
    EXPECT_TRUE(time.push_time({3, 30}));
    EXPECT_FALSE(time.push_time({4, 40}));

    SendQueues entries(caps);
    EXPECT_TRUE(entries.push_control(json_of_size(10)));
    EXPECT_TRUE(entries.push_control(json_of_size(10)));
    EXPECT_FALSE(entries.push_control(json_of_size(1)));

    SendQueues bytes(caps);
    EXPECT_TRUE(bytes.push_control(json_of_size(60)));
    EXPECT_FALSE(bytes.push_control(json_of_size(41)));
    EXPECT_TRUE(bytes.push_control(json_of_size(40)));
    // Taking a message frees its bytes
    EXPECT_EQ(bytes.take_control(FRAGMENT).bytes.size(), 60U);
    EXPECT_FALSE(bytes.push_control(json_of_size(61)));
    EXPECT_TRUE(bytes.push_control(json_of_size(60)));
}

// A message being fragmented counts against the byte cap until its last fragment is taken
TEST(SendQueueCaps, CountAFragmentedMessageUntilItsLastFragment) {
    const size_t size = sendspin::server::MAX_TRANSPORT_PLAINTEXT + 1;
    SendQueueCaps caps;
    caps.control_bytes = size + 10;
    SendQueues queues(caps);
    ASSERT_TRUE(queues.push_control(json_of_size(size)));
    EXPECT_EQ(queues.next(), SendChoice::CONTROL);
    queues.take_control(FRAGMENT);
    EXPECT_EQ(queues.next(), SendChoice::FRAGMENT);
    EXPECT_FALSE(queues.push_control(json_of_size(11)));
    while (queues.next() == SendChoice::FRAGMENT) {
        // The message is taken only with its last fragment
        EXPECT_EQ(queues.control_taken(), 0U);
        queues.take_control(FRAGMENT);
    }
    EXPECT_EQ(queues.control_taken(), 1U);
    EXPECT_TRUE(queues.push_control(json_of_size(size)));
}

// messaging.md "Fragmentation": nothing goes between the fragments of one message, a time reply
// included; the time reply then goes ahead of the control message queued before it
TEST(SendQueues, HoldATimeReplyUntilTheFragmentedMessageEnds) {
    SendQueues queues{SendQueueCaps{}};
    const size_t size = (2 * sendspin::server::MAX_TRANSPORT_PLAINTEXT) + 1;
    ASSERT_TRUE(queues.push_control(json_of_size(size)));
    ASSERT_TRUE(queues.push_control({MessageKind::BINARY, {sendspin::server::MESSAGE_ID_JSON}}));

    const OutboundPlaintext first = queues.take_control(sendspin::server::MAX_TRANSPORT_PLAINTEXT);
    EXPECT_EQ(first.bytes[0], wire::FRAGMENT_ID);
    ASSERT_TRUE(queues.push_time({7, 70}));

    std::vector<SendChoice> taken;
    while (queues.next() != SendChoice::NONE) {
        const SendChoice choice = queues.next();
        taken.push_back(choice);
        if (choice == SendChoice::TIME) {
            EXPECT_EQ(queues.take_time().client_transmitted, 7);
        } else {
            queues.take_control(sendspin::server::MAX_TRANSPORT_PLAINTEXT);
        }
    }
    const std::vector<SendChoice> expected{SendChoice::FRAGMENT, SendChoice::FRAGMENT,
                                           SendChoice::TIME, SendChoice::CONTROL};
    EXPECT_EQ(taken, expected);
}

TEST(SendQueues, PassATextOrSmallMessageThroughWhole) {
    SendQueues queues{SendQueueCaps{}};
    const Bytes text(sendspin::server::MAX_TRANSPORT_PLAINTEXT + 1, 'x');
    ASSERT_TRUE(queues.push_control({MessageKind::TEXT, text}));
    ASSERT_TRUE(queues.push_control(json_of_size(sendspin::server::MAX_TRANSPORT_PLAINTEXT)));
    const OutboundPlaintext first = queues.take_control(FRAGMENT);
    EXPECT_EQ(first.kind, MessageKind::TEXT);
    EXPECT_EQ(first.bytes, text);
    EXPECT_EQ(queues.take_control(FRAGMENT).bytes.size(),
              sendspin::server::MAX_TRANSPORT_PLAINTEXT);
    EXPECT_EQ(queues.next(), SendChoice::NONE);
}

// A JSON message, and each fragment of one too large for one Noise transport message, has room
// for the tag its encryption appends, so that the encryption does not reallocate it
TEST(SendQueues, LeaveRoomForTheTransportTag) {
    const auto room = [](const OutboundPlaintext& message) {
        return message.bytes.capacity() - message.bytes.size();
    };
    EXPECT_GE(room(sendspin::server::json_plaintext(R"({"type":"server/time"})")),
              sendspin::server::TRANSPORT_TAG_BYTES);
    SendQueues queues{SendQueueCaps{}};
    ASSERT_TRUE(queues.push_control(sendspin::server::json_plaintext(
        std::string((2 * sendspin::server::MAX_TRANSPORT_PLAINTEXT) + 1, 'x'))));
    size_t fragments = 0;
    while (queues.next() != SendChoice::NONE) {
        const OutboundPlaintext fragment = queues.take_control(FRAGMENT);
        EXPECT_EQ(fragment.bytes[0], wire::FRAGMENT_ID);
        EXPECT_GE(room(fragment), sendspin::server::TRANSPORT_TAG_BYTES) << fragments;
        ++fragments;
    }
    EXPECT_GT(fragments, 2U);
}

}  // namespace
