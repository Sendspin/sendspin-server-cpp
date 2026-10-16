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

/// @file test_sched_frames.cpp
/// @brief The frame grid, the frame ring and the cursor's prepare step

#include "frame_payloads.h"
#include "payload_slots.h"
#include "profile.h"
#include "sched/frames.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace {

using sendspin::server::sched::Codec;
using sendspin::server::sched::Cursor;
using sendspin::server::sched::EpochId;
using sendspin::server::sched::Frame;
using sendspin::server::sched::FrameLayout;
using sendspin::server::sched::FrameRing;
using sendspin::server::sched::InstanceId;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr EpochId EPOCH{2};
constexpr EpochId OTHER_EPOCH{3};

/// A one-cell frame k of epoch 2 with T0 at 1000 ms
Frame frame(int64_t index, uint32_t cells = 1) {
    Frame f;
    f.epoch = EPOCH;
    f.index = index;
    f.ts_us = 1000 * MS + index * 20 * MS;
    f.duration_us = cells * 20 * MS;
    f.samples = 960 * cells;
    f.payload = sendspin::server::test::zero_payload(100);
    return f;
}

TEST(SchedFrames, FrameLayout) {
    struct Row {
        const char* name;
        uint32_t rate;
        Codec codec;
        FrameLayout layout;
    };
    const Row rows[] = {
        {"Control: 48 kHz", 48000, Codec::PCM, {1, 960, true}},
        {"44.1 kHz", 44100, Codec::PCM, {1, 882, true}},
        {"88.2 kHz", 88200, Codec::FLAC, {1, 1764, true}},
        {"96 kHz", 96000, Codec::FLAC, {1, 1920, true}},
        {"192 kHz", 192000, Codec::PCM, {1, 3840, true}},
        {"11,025 Hz PCM", 11025, Codec::PCM, {2, 441, true}},
        {"11,025 Hz FLAC", 11025, Codec::FLAC, {2, 441, true}},
        {"8,010 Hz", 8010, Codec::FLAC, {5, 801, true}},
        {"44,101 Hz needs 50 cells", 44101, Codec::PCM, {50, 44101, false}},
        {"Opus at 48 kHz", 48000, Codec::OPUS, {1, 960, true}},
        {"Opus at a rate it does not take", 44100, Codec::OPUS, {1, 882, false}},
        {"rate 0", 0, Codec::PCM, {0, 0, false}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const FrameLayout layout = sched::frame_layout(row.rate, row.codec);
        EXPECT_EQ(layout.cells, row.layout.cells);
        EXPECT_EQ(layout.samples, row.layout.samples);
        EXPECT_EQ(layout.usable, row.layout.usable);
    }
}

TEST(SchedFrames, FrameDuration) {
    // Control: a full frame is exactly one cell
    EXPECT_EQ(sched::frame_duration_us(882, 44100), 20 * MS);
    EXPECT_EQ(sched::frame_duration_us(441, 11025), 40 * MS);
    // The short last frames round up
    EXPECT_EQ(sched::frame_duration_us(454, 44100), 10295);
    EXPECT_EQ(sched::frame_duration_us(494, 48000), 10292);
}

TEST(SchedFrameRing, KeysFramesByCellIndex) {
    FrameRing ring(InstanceId{7}, 2);
    ring.restart(EPOCH, 128);
    // An empty ring's base is its start index
    EXPECT_EQ(ring.base(), 128);
    EXPECT_EQ(ring.at(EPOCH, 128), nullptr);
    // The first frame may start later than requested, at a multiple of m at or above the base
    EXPECT_FALSE(ring.append(frame(131, 2), 0));
    EXPECT_FALSE(ring.append(frame(126, 2), 0));
    Frame stale = frame(130, 2);
    stale.epoch = OTHER_EPOCH;
    EXPECT_FALSE(ring.append(std::move(stale), 0));
    EXPECT_EQ(ring.base(), 128);
    ASSERT_TRUE(ring.append(frame(130, 2), 0));
    EXPECT_EQ(ring.base(), 130);
    // After that, only the next index
    EXPECT_FALSE(ring.append(frame(134, 2), 0));
    ASSERT_TRUE(ring.append(frame(132, 2), 0));
    // Control: a held index
    ASSERT_NE(ring.at(EPOCH, 132), nullptr);
    EXPECT_EQ(ring.at(EPOCH, 132)->index, 132);
    // Not a multiple of m, another epoch, below the base, past the end
    EXPECT_EQ(ring.at(EPOCH, 131), nullptr);
    EXPECT_EQ(ring.at(OTHER_EPOCH, 132), nullptr);
    EXPECT_EQ(ring.at(EPOCH, 128), nullptr);
    EXPECT_EQ(ring.at(EPOCH, 134), nullptr);
    EXPECT_EQ(ring.next_expected(), 134);
    // A ring emptied by pruning keeps expecting its next index
    ring.prune(5000 * MS);
    EXPECT_EQ(ring.base(), 134);
    EXPECT_FALSE(ring.append(frame(136, 2), 0));
    EXPECT_TRUE(ring.append(frame(134, 2), 0));
}

// Frames go once ts <= now; an emptied ring's base is the next expected index
TEST(SchedFrameRing, PrunesByTimestamp) {
    FrameRing ring(InstanceId{1}, 1);
    ring.restart(EPOCH, 0);
    for (int64_t k = 0; k < 4; ++k) {
        ASSERT_TRUE(ring.append(frame(k), 0));
    }
    ring.prune(1020 * MS - 1);
    EXPECT_EQ(ring.base(), 1);
    ring.prune(1020 * MS);
    EXPECT_EQ(ring.base(), 2);
    ring.prune(5000 * MS);
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(ring.base(), 4);
    // A frame already late on arrival is dropped, and the base moves past it
    EXPECT_TRUE(ring.append(frame(4), 5000 * MS));
    EXPECT_EQ(ring.size(), 0U);
    EXPECT_EQ(ring.base(), 5);
}

// A visualizer analysis that fell behind restarts past cells late for every cursor; its ring then
// expects the later index, once pruning emptied it and only forward
TEST(SchedFrameRing, SkipsToALaterIndexOnceEmptied) {
    FrameRing ring(InstanceId{1}, 1);
    ring.restart(EPOCH, 0);
    for (int64_t k = 0; k < 4; ++k) {
        ASSERT_TRUE(ring.append(frame(k), 0));
    }
    // Control: frames still ahead of now, so the ring keeps expecting the next index
    ring.skip_to(EPOCH, 9, 0);
    EXPECT_EQ(ring.base(), 0);
    EXPECT_FALSE(ring.append(frame(9), 0));
    // Control: a frame of another epoch moves nothing
    ring.skip_to(OTHER_EPOCH, 9, 5000 * MS);
    EXPECT_EQ(ring.base(), 0);
    ring.skip_to(EPOCH, 9, 5000 * MS);
    EXPECT_EQ(ring.base(), 9);
    EXPECT_TRUE(ring.append(frame(9), 0));
    ring.prune(5000 * MS);
    ring.skip_to(EPOCH, 4, 5000 * MS);
    EXPECT_EQ(ring.base(), 10);
}

// A ring holds every frame within the horizon, however far past the room it was made with a
// client's lead pushes it
TEST(SchedFrameRing, HoldsFramesPastItsFirstRoom) {
    FrameRing ring(InstanceId{1}, 1);
    ring.restart(EPOCH, 0);
    const auto frames = static_cast<int64_t>(sched::payload_slots(1)) * 2;
    for (int64_t k = 0; k < frames; ++k) {
        ASSERT_TRUE(ring.append(frame(k), 0));
    }
    EXPECT_EQ(ring.size(), static_cast<size_t>(frames));
    for (int64_t k = 0; k < frames; ++k) {
        const Frame* held = ring.at(EPOCH, k);
        ASSERT_NE(held, nullptr) << k;
        EXPECT_EQ(held->index, k);
    }
}

// An instance's payload slots cover the ring's span and the frames core may leave unpruned past
// their play time, a part frame rounded up: 3 s and 0.5 s on the embedded profile, 12 s and 1 s
// on a host
TEST(SchedFrameRing, SizesPayloadSlotsPastTheRingsSpan) {
    constexpr bool EMBEDDED = sendspin::server::profile::EMBEDDED;
    EXPECT_EQ(sched::payload_slots(1), EMBEDDED ? 151U + 25U : 601U + 50U);
    EXPECT_EQ(sched::payload_slots(2), EMBEDDED ? 76U + 13U : 301U + 25U);
    EXPECT_EQ(sched::payload_slots(7), EMBEDDED ? 22U + 4U : 86U + 8U);
}

// A frame the ring drops gives its payload's slot back: pruned at its play time, cleared by a
// restart, or refused on append
TEST(SchedFrameRing, ReturnsTheSlotOfEachFrameItDrops) {
    using sendspin::server::FramePayload;
    using sendspin::server::PayloadSlots;
    struct Row {
        const char* name;
        /// Drops a frame, or not; spare holds the store's last slot
        void (*drop)(FrameRing&, FramePayload& spare);
        bool returned;
    };
    const Row rows[] = {
        {"Control: none dropped", [](FrameRing& ring, FramePayload&) { ring.prune(0); }, false},
        {"pruned", [](FrameRing& ring, FramePayload&) { ring.prune(frame(0).ts_us); }, true},
        {"restarted", [](FrameRing& ring, FramePayload&) { ring.restart(EPOCH, 0); }, true},
        {"refused",
         [](FrameRing& ring, FramePayload& spare) {
             Frame stale = frame(2);
             stale.epoch = OTHER_EPOCH;
             stale.payload = std::move(spare);
             EXPECT_FALSE(ring.append(std::move(stale), 0));
         },
         true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const auto slots = PayloadSlots::create(8, 3);
        ASSERT_TRUE(slots);
        const auto take = [&] {
            static_cast<void>(slots->reserve());
            return slots->commit(1);
        };
        FrameRing ring(InstanceId{1}, 1);
        ring.restart(EPOCH, 0);
        for (int64_t k = 0; k < 2; ++k) {
            Frame f = frame(k);
            f.payload = take();
            ASSERT_TRUE(ring.append(std::move(f), 0));
        }
        FramePayload spare = take();
        EXPECT_TRUE(slots->reserve().empty());
        row.drop(ring, spare);
        EXPECT_EQ(!slots->reserve().empty(), row.returned);
    }
}

TEST(SchedCursor, PrepareClampsSkipsAndFinishes) {
    FrameRing ring(InstanceId{1}, 1);
    ring.restart(EPOCH, 0);
    for (int64_t k = 0; k < 10; ++k) {
        ASSERT_TRUE(ring.append(frame(k), 0));
    }
    struct Row {
        const char* name;
        int64_t next;
        int64_t min_ts;
        int64_t od;
        int64_t now;
        int64_t prepared;
    };
    const Row rows[] = {
        {"Control: nothing late", 0, 0, 0, 0, 0},
        {"late for this player: ts - od <= now", 0, 0, 0, 1020 * MS, 2},
        {"od makes one more frame late", 0, 0, 20 * MS, 1020 * MS, 3},
        {"a frame before min_ts is skipped", 0, 1050 * MS, 0, 0, 3},
        {"an unproduced frame stops the skip", 8, 0, 0, 1500 * MS, 10},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Cursor cursor;
        cursor.next = row.next;
        cursor.min_ts = row.min_ts;
        sched::prepare_cursor(cursor, ring, EPOCH, row.od, row.now);
        EXPECT_EQ(cursor.next, row.prepared);
        EXPECT_FALSE(cursor.final_done);
    }

    // After a long freeze the ring dropped frames below the cursor: clamp to the base, then skip
    ring.prune(1060 * MS);
    Cursor behind;
    behind.next = 1;
    sched::prepare_cursor(behind, ring, EPOCH, 10 * MS, 1060 * MS);
    EXPECT_EQ(behind.next, 4);

    // An emptied ring's base never moves the cursor past an unproduced frame
    ring.prune(5000 * MS);
    Cursor empty;
    sched::prepare_cursor(empty, ring, EPOCH, 0, 5000 * MS);
    EXPECT_EQ(empty.next, 10);

    // final_done only past the instance's own final index
    ring.set_final_index(10);
    sched::prepare_cursor(empty, ring, EPOCH, 0, 5000 * MS);
    EXPECT_FALSE(empty.final_done);
    ring.set_final_index(9);
    sched::prepare_cursor(empty, ring, EPOCH, 0, 5000 * MS);
    EXPECT_TRUE(empty.final_done);
}

// Where a cursor moves to a target instance
TEST(SchedCursor, PlanSwitch) {
    struct Row {
        const char* name;
        int64_t next;
        std::optional<int64_t> planned;
        std::optional<int64_t> target_base;
        uint32_t from_cells;
        uint32_t to_cells;
        int64_t point;
    };
    const Row rows[] = {
        {"Control: one-cell frames both ways switch at max(next, base)", 235, std::nullopt, 200, 1,
         1, 235},
        {"the target ring's base above next", 235, std::nullopt, 240, 1, 1, 240},
        {"no target ring yet: P_prep past next", 235, std::nullopt, std::nullopt, 1, 1, 237},
        {"a planned point holds while the cursor is below it", 236, 237, std::nullopt, 1, 1, 237},
        {"a planned point already late moves up to next", 250, 237, 200, 1, 1, 250},
        {"the target's first frame arrives above the planned point", 236, 237, 245, 1, 1, 245},
        {"flac@11025 to pcm@48000: an odd base rounds up to an even cell", 234, std::nullopt, 237,
         2, 1, 238},
        {"pcm@48000 to flac@11025: next + P_prep from an odd next rounds up", 235, std::nullopt,
         std::nullopt, 1, 2, 238},
        {"two-cell frames to five-cell frames: a multiple of ten", 232, std::nullopt, 231, 2, 5,
         240},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::plan_switch(row.next, row.planned, row.target_base, row.from_cells,
                                     row.to_cells),
                  row.point);
    }
}

}  // namespace
