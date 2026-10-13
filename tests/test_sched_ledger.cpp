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

/// @file test_sched_ledger.cpp
/// @brief The buffer ledger

#include "sched/constants.h"
#include "sched/ledger.h"
#include <gtest/gtest.h>

#include <cstdint>

namespace {

using sendspin::server::sched::Ledger;
using sendspin::server::sched::NEVER;

constexpr int64_t MS = 1000;

// Player A, pcm 44.1 kHz, 14,125-byte chunks of 80 ms, capacity 100,000, od 0
constexpr int64_t A_CAPACITY = 100000;
constexpr int64_t A_CHUNK = 14125;
// Player B, flac 48 kHz, 10,013-byte chunks of 100 ms, capacity 32,000, od 50
constexpr int64_t B_CAPACITY = 32000;
constexpr int64_t B_CHUNK = 10013;

/// Epoch 2's A chunk j ends at 2690 + 80j ms
int64_t a2_key(int64_t j) {
    return (2690 + 80 * j) * MS;
}

/// Epoch 2's B chunk j ends at 2710 + 100j ms (ts 2610 + 100j)
int64_t b2_key(int64_t j) {
    return (2710 + 100 * j) * MS;
}

// Spec player/v1.md "Player Buffer Accounting": an entry counts while its completion time is in
// the future, strictly; the output delay is applied when evaluated
TEST(SchedLedger, CountsWhileCompletionIsInTheFuture) {
    struct Row {
        const char* name;
        bool player;
        int64_t output_delay_us;
        int64_t now_us;
        int64_t count;
    };
    const Row rows[] = {
        {"completion exactly now does not count", true, 0, 1000 * MS, 0},
        {"one microsecond earlier counts", true, 0, 1000 * MS - 1, 500},
        {"od moves the completion earlier: at it", true, 200 * MS, 800 * MS, 0},
        {"od moves the completion earlier: just before", true, 200 * MS, 800 * MS - 1, 500},
        {"visualizer ignores od", false, 200 * MS, 1000 * MS - 1, 500},
        {"visualizer at its timestamp", false, 200 * MS, 1000 * MS, 0},
        {"Control: well before", true, 0, 0, 500},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Ledger ledger(A_CAPACITY, row.player);
        ledger.set_output_delay_us(row.output_delay_us);
        ledger.add(1000 * MS, 500);
        EXPECT_EQ(ledger.count(row.now_us), row.count);
    }
}

// At 1.6 ms: A has A0..A6 out and waits for A0's completion; B has B0..B2 out and waits for B0's
// completion minus its 50 ms output delay
TEST(SchedLedger, StartFillsThenWaitsForRoom) {
    Ledger a(A_CAPACITY, true);
    for (int64_t j = 0; j < 7; ++j) {
        a.add((640 + 80 * j) * MS, A_CHUNK);
    }
    // Control: A count 98,875
    EXPECT_EQ(a.count(1600), 98875);
    EXPECT_FALSE(a.admits(A_CHUNK, 1600));
    EXPECT_EQ(a.room_time(A_CHUNK, 1600), 640 * MS);
    // At 640 A0 has completed: count 84,750, and A7 fits
    EXPECT_EQ(a.count(640 * MS), 84750);
    EXPECT_TRUE(a.admits(A_CHUNK, 640 * MS));
    EXPECT_EQ(a.room_time(A_CHUNK, 640 * MS), 640 * MS);
    a.add(1200 * MS, A_CHUNK);
    EXPECT_EQ(a.room_time(A_CHUNK, 640 * MS), 720 * MS);

    Ledger b(B_CAPACITY, true);
    b.set_output_delay_us(50 * MS);
    for (int64_t j = 0; j < 3; ++j) {
        b.add((660 + 100 * j) * MS, B_CHUNK);
    }
    EXPECT_EQ(b.count(1600), 30039);
    EXPECT_EQ(b.room_time(B_CHUNK, 1600), 610 * MS);
    EXPECT_EQ(b.count(610 * MS), 20026);
    b.add(960 * MS, B_CHUNK);
    EXPECT_EQ(b.room_time(B_CHUNK, 610 * MS), 710 * MS);
}

// B's full ledger holds a chunk back until 4060 while a time reply goes ahead
TEST(SchedLedger, SlowPlayerWaitsForItsRoomTime) {
    Ledger b(B_CAPACITY, true);
    b.set_output_delay_us(50 * MS);
    for (int64_t j = 14; j <= 16; ++j) {
        b.add(b2_key(j), B_CHUNK);
    }
    EXPECT_EQ(b.count(4052 * MS), 30039);
    EXPECT_FALSE(b.admits(B_CHUNK, 4052 * MS));
    EXPECT_EQ(b.room_time(B_CHUNK, 4052 * MS), 4060 * MS);
    EXPECT_TRUE(b.admits(B_CHUNK, 4060 * MS));
}

// A 300 ms freeze: completions at 6050..6290 free A42..A45 before message 2 at 6300
TEST(SchedLedger, FreezeKeepsCountingByTime) {
    Ledger a(A_CAPACITY, true);
    for (int64_t j = 42; j <= 48; ++j) {
        a.add(a2_key(j), A_CHUNK);
    }
    EXPECT_EQ(a.count(6000 * MS), 98875);
    EXPECT_EQ(a.count(6300 * MS), 42375);
}

// Spec player/v1.md "Player Buffer Accounting": a delay update re-times every chunk since the
// reset, including ones that had stopped counting
TEST(SchedLedger, OutputDelayChangeRetimesEveryEntry) {
    Ledger a(A_CAPACITY, true);
    for (int64_t j = 42; j <= 48; ++j) {
        a.add(a2_key(j), A_CHUNK);
    }
    // od 0 to 200 at 6000: A44..A48 count
    a.set_output_delay_us(200 * MS);
    EXPECT_EQ(a.count(6000 * MS), 70625);
    a.add(a2_key(49), A_CHUNK);
    a.add(a2_key(50), A_CHUNK);
    EXPECT_EQ(a.count(6000 * MS), 98875);
    EXPECT_EQ(a.room_time(A_CHUNK, 6000 * MS), 6010 * MS);

    // Under od 200 chunk j is written at 1930 + 80j, the last before 7000 being A63
    for (int64_t j = 51; j <= 63; ++j) {
        const int64_t written = (1930 + 80 * j) * MS;
        a.prune(written);
        ASSERT_TRUE(a.admits(A_CHUNK, written)) << j;
        a.add(a2_key(j), A_CHUNK);
    }
    a.prune(7000 * MS);
    // od 200 to 0 at 7000: A54..A63 count again, over the capacity
    a.set_output_delay_us(0);
    EXPECT_EQ(a.count(7000 * MS), 141250);
    EXPECT_EQ(a.room_time(A_CHUNK, 7000 * MS), 7250 * MS);
    EXPECT_EQ(a.count(7250 * MS), 84750);
}

TEST(SchedLedger, ResetPruneAndLastCompletion) {
    Ledger ledger(A_CAPACITY, true);
    EXPECT_EQ(ledger.last_completion(), NEVER);
    ledger.set_output_delay_us(10 * MS);
    ledger.add(100 * MS, 1000);
    ledger.add(200 * MS, 2000);
    EXPECT_EQ(ledger.last_completion(), 190 * MS);

    // Prune keeps entries with key > now, so a later od decrease can still count them
    ledger.prune(100 * MS);
    EXPECT_EQ(ledger.entries(), 1U);
    ledger.set_output_delay_us(0);
    EXPECT_EQ(ledger.count(150 * MS), 2000);
    ledger.prune(200 * MS);
    EXPECT_EQ(ledger.entries(), 0U);

    ledger.add(300 * MS, 3000);
    ledger.reset();
    EXPECT_EQ(ledger.count(0), 0);
    EXPECT_EQ(ledger.last_completion(), NEVER);
    // Control: after a reset the ledger counts again from empty
    ledger.add(400 * MS, 4000);
    EXPECT_EQ(ledger.count(0), 4000);
}

// A ledger counts every entry within the horizon, however far past the room it was made with a
// client's lead pushes it
TEST(SchedLedger, CountsEntriesPastItsFirstRoom) {
    Ledger ledger(A_CAPACITY, true);
    const auto entries = static_cast<int64_t>(sendspin::server::sched::LEDGER_ENTRIES) * 2;
    for (int64_t j = 1; j <= entries; ++j) {
        ledger.add(j * MS, 10);
    }
    EXPECT_EQ(ledger.entries(), static_cast<size_t>(entries));
    EXPECT_EQ(ledger.count(0), entries * 10);
    // The entries complete in order: half of them by the middle key
    EXPECT_EQ(ledger.count((entries / 2) * MS), (entries / 2) * 10);
    EXPECT_EQ(ledger.last_completion(), entries * MS);
}

TEST(SchedLedger, RoomTimeEdges) {
    Ledger ledger(10000, true);
    ledger.add(100 * MS, 4000);
    ledger.add(200 * MS, 4000);
    ledger.add(300 * MS, 2000);
    // An exact fit is room
    EXPECT_EQ(ledger.room_time(0, 0), 0);
    // 2,000 more needs the first entry gone; 6,000 more the first two
    EXPECT_EQ(ledger.room_time(2000, 0), 100 * MS);
    EXPECT_EQ(ledger.room_time(6000, 0), 200 * MS);
    // Everything gone
    EXPECT_EQ(ledger.room_time(10000, 0), 300 * MS);
    // More than the capacity never fits
    EXPECT_EQ(ledger.room_time(10001, 0), NEVER);
    // Control: room now
    EXPECT_EQ(ledger.room_time(2000, 100 * MS), 100 * MS);
}

}  // namespace
