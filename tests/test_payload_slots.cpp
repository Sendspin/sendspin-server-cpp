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

/// @file test_payload_slots.cpp
/// @brief PayloadSlots and FramePayload: a slot reserved, written and committed, returned in any
/// order by destroying its payload, and the store kept alive by the payloads it gave

#include "frame_payload.h"
#include "payload_slots.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace {

using sendspin::server::FramePayload;
using sendspin::server::PayloadSlots;
using sendspin::server::SharedPtr;

constexpr size_t SLOT_BYTES = 16;

/// A payload of size bytes, each byte value, in the next free slot
FramePayload filled(PayloadSlots& slots, size_t size, uint8_t value) {
    const std::span<uint8_t> room = slots.reserve();
    EXPECT_EQ(room.size(), SLOT_BYTES);
    std::fill_n(room.begin(), size, value);
    return slots.commit(size);
}

// A commit holds the first bytes written into the slot reserve() gave; reserve() gives the same
// slot until it is committed, and another after
TEST(PayloadSlots, CommitsTheBytesWrittenIntoTheReservedSlot) {
    const SharedPtr<PayloadSlots> slots = PayloadSlots::create(SLOT_BYTES, 2);
    ASSERT_TRUE(slots);
    EXPECT_EQ(slots->slot_bytes(), SLOT_BYTES);
    const std::span<uint8_t> room = slots->reserve();
    ASSERT_EQ(room.size(), SLOT_BYTES);
    // Uncommitted: the same slot again
    EXPECT_EQ(slots->reserve().data(), room.data());
    room[0] = 7;
    room[1] = 8;
    const FramePayload payload = slots->commit(2);
    EXPECT_EQ(payload.size(), 2U);
    EXPECT_EQ(payload.bytes().data(), room.data());
    EXPECT_EQ(payload.bytes()[0], 7);
    EXPECT_EQ(payload.bytes()[1], 8);
    // Control: committed, the next reserve gives the other slot
    EXPECT_NE(slots->reserve().data(), room.data());
    EXPECT_FALSE(slots->starved());
}

// With every slot held, reserve() gives nothing and the store is starved; destroying any one
// payload, in any order, returns exactly its slot
TEST(PayloadSlots, ReturnsEachSlotInAnyOrder) {
    struct Row {
        const char* name;
        size_t released;
    };
    const std::array<Row, 3> rows{{{"the first", 0}, {"the middle", 1}, {"the last", 2}}};
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const SharedPtr<PayloadSlots> slots = PayloadSlots::create(SLOT_BYTES, 3);
        ASSERT_TRUE(slots);
        std::vector<std::optional<FramePayload>> held;
        for (uint8_t i = 0; i < 3; ++i) {
            held.emplace_back(filled(*slots, 4, i));
        }
        EXPECT_TRUE(slots->reserve().empty());
        EXPECT_TRUE(slots->starved());
        const uint8_t* freed = held[row.released]->bytes().data();
        held[row.released].reset();
        const std::span<uint8_t> room = slots->reserve();
        EXPECT_EQ(room.data(), freed);
        EXPECT_FALSE(slots->starved());
        // The others still hold their bytes
        for (size_t i = 0; i < held.size(); ++i) {
            if (held[i]) {
                EXPECT_EQ(held[i]->bytes()[3], i);
            }
        }
        const FramePayload refilled = slots->commit(0);
        EXPECT_TRUE(slots->reserve().empty());
    }
}

// A payload moved from returns nothing; the one moved to returns the slot once, and so does one
// assigned over
TEST(PayloadSlots, ReturnsASlotOnceThroughMoves) {
    const SharedPtr<PayloadSlots> slots = PayloadSlots::create(SLOT_BYTES, 1);
    ASSERT_TRUE(slots);
    std::optional<FramePayload> first(filled(*slots, 3, 1));
    std::optional<FramePayload> second(std::move(*first));
    EXPECT_EQ(first->size(), 0U);
    EXPECT_TRUE(first->bytes().empty());
    first.reset();
    EXPECT_TRUE(slots->reserve().empty());
    EXPECT_EQ(second->size(), 3U);
    // Assigned an empty payload: its slot returns
    *second = FramePayload{};
    EXPECT_EQ(second->size(), 0U);
    EXPECT_FALSE(slots->reserve().empty());
    second.reset();
    // Control: the slot came back once, so one commit fills the store again
    const FramePayload again = slots->commit(1);
    EXPECT_TRUE(slots->reserve().empty());
}

// A payload keeps its store, and its bytes, alive after the store's owner lets it go
TEST(PayloadSlots, OutlivesItsOwnerThroughItsPayloads) {
    SharedPtr<PayloadSlots> slots = PayloadSlots::create(SLOT_BYTES, 2);
    ASSERT_TRUE(slots);
    const FramePayload payload = filled(*slots, 5, 9);
    slots.reset();
    ASSERT_EQ(payload.size(), 5U);
    EXPECT_EQ(payload.bytes()[4], 9);
}

// A store of no slots, or of slots of no bytes, is refused
TEST(PayloadSlots, RefusesAnEmptyStore) {
    EXPECT_FALSE(PayloadSlots::create(0, 4));
    EXPECT_FALSE(PayloadSlots::create(SLOT_BYTES, 0));
    // Control
    EXPECT_TRUE(PayloadSlots::create(1, 1));
}

}  // namespace
