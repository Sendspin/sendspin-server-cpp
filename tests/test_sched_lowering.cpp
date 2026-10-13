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

/// @file test_sched_lowering.cpp
/// @brief A live input's latency lowering: the excess a window of writes kept becomes a debt, never
/// more than the window measured nor a write's own excess, played out at a bounded slew with
/// `G_eff` following the lead down

#include "sched/lowering.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::sched::live_slew_ppm;
using sendspin::server::sched::LiveLowering;
using sendspin::server::sched::LOWERING_MIN_US;
using sendspin::server::sched::LOWERING_SLEW_PPM;
using sendspin::server::sched::LOWERING_WINDOW_US;
using sendspin::server::sched::observe_live_write;
using sendspin::server::sched::record_slew;

constexpr int64_t WINDOW = LOWERING_WINDOW_US;
constexpr int64_t MIN = LOWERING_MIN_US;
constexpr uint32_t RATE = 48000;

/// The slew took slewed_us out; `G_eff` is not what these tests look at
void slew(LiveLowering& lowering, int64_t slewed_us) {
    (void)record_slew(lowering, slewed_us, 0, 0);
}

/// One write: when, its excess with nothing slewed, and the debt it should see; the debt it sees
/// is slewed out at once
struct Write {
    int64_t at_us;
    int64_t excess_us;
    int64_t debt_us;
};

struct Row {
    std::string name;
    std::vector<Write> writes;
};

/// The excess a write shows is lower by everything slewed before it
TEST(LiveLowering, OwesTheExcessAWholeWindowKept) {
    const Row rows[] = {
        {"Control: an excess below the minimum stays",
         {{0, MIN - 1, 0}, {WINDOW / 2, MIN + 5000, 0}, {WINDOW, MIN + 5000, 0}}},
        {"An excess of the minimum throughout is owed at the window's end",
         {{0, MIN, 0}, {WINDOW / 2, MIN + 5000, 0}, {WINDOW, MIN + 9000, MIN}}},
        {"The window's least excess is owed, the latest write landing as a fresh anchor would",
         {{0, 300000, 0}, {WINDOW / 2, 250000, 0}, {WINDOW, 280000, 250000}}},
        {"A window not yet over decides nothing", {{0, 300000, 0}, {WINDOW - 1, 300000, 0}}},
        {"Control: one late write in the window keeps the latency",
         {{0, 300000, 0}, {WINDOW / 2, -1000, 0}, {WINDOW, 300000, 0}}},
        {"Once slewed, the next window sees no excess left and owes nothing more",
         {{0, 100000, 0},
          {WINDOW, 100000, 100000},
          {WINDOW + 1, 100000, 0},
          {2 * WINDOW + 1, 100000, 0}}},
        {"An excess that rises after a slew is owed once a whole window kept it",
         {{0, 200000, 0},
          {WINDOW, 200000, 200000},
          {WINDOW + 10, 260000, 0},
          {2 * WINDOW + 10, 260000, 0},
          {3 * WINDOW + 10, 260000, 60000}}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        LiveLowering lowering;
        int64_t slewed = 0;
        for (size_t i = 0; i < row.writes.size(); ++i) {
            SCOPED_TRACE(i);
            const Write& write = row.writes[i];
            const int64_t debt =
                observe_live_write(lowering, write.excess_us - slewed, write.at_us);
            EXPECT_EQ(debt, write.debt_us);
            if (debt > 0) {
                slew(lowering, debt);
                slewed += debt;
            }
        }
    }
}

/// A debt is slewed out across many writes; a window that then measures less left than owed lowers
/// the debt to it, so the slew never passes the excess measured
TEST(LiveLowering, CarriesADebtAcrossWritesBoundedByTheLatestWindow) {
    LiveLowering lowering;
    EXPECT_EQ(observe_live_write(lowering, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(lowering, 500000, WINDOW), 500000);
    slew(lowering, 100000);
    EXPECT_EQ(observe_live_write(lowering, 400000, WINDOW + 20000), 400000);
    slew(lowering, 100000);
    // A rebase or late writes left only 30 ms of excess: below the minimum, the debt falls to it
    EXPECT_EQ(observe_live_write(lowering, 30000, 2 * WINDOW + 20000), 30000);
    slew(lowering, 30000);
    EXPECT_EQ(observe_live_write(lowering, 0, 2 * WINDOW + 40000), 0);
    // Control: a late write measures none left, which ends a debt
    LiveLowering late;
    EXPECT_EQ(observe_live_write(late, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(late, 500000, WINDOW), 500000);
    slew(late, 100000);
    EXPECT_EQ(observe_live_write(late, -20000, 2 * WINDOW), 0);
}

/// Every write caps the debt at its own excess: a joiner raising the group lead mid-slew, or a
/// write late after a stall, leaves less or nothing to slew, which a later write does not revive
TEST(LiveLowering, EndsWhatAWritesOwnExcessNoLongerHolds) {
    LiveLowering raised;
    EXPECT_EQ(observe_live_write(raised, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(raised, 500000, WINDOW), 500000);
    slew(raised, 100000);
    // A joiner raised G_next by 300 ms: 100 ms of the 400 ms owed is still excess
    EXPECT_EQ(observe_live_write(raised, 100000, WINDOW + 10000), 100000);
    slew(raised, 100000);
    EXPECT_EQ(observe_live_write(raised, 0, WINDOW + 20000), 0);

    LiveLowering stalled;
    EXPECT_EQ(observe_live_write(stalled, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(stalled, 500000, WINDOW), 500000);
    slew(stalled, 100000);
    // A stall left the next write late: nothing is owed, nor once writes are on time again
    EXPECT_EQ(observe_live_write(stalled, -50000, WINDOW + 600000), 0);
    EXPECT_EQ(observe_live_write(stalled, 400000, WINDOW + 610000), 0);
    // Control: on time throughout, the debt stays until slewed
    LiveLowering steady;
    EXPECT_EQ(observe_live_write(steady, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(steady, 500000, WINDOW), 500000);
    slew(steady, 100000);
    EXPECT_EQ(observe_live_write(steady, 400000, WINDOW + 10000), 400000);
}

// A write owing latency plays LOWERING_SLEW_PPM fast, or slower where that would take out more
// than it owes; its excess is measured against `G_next` from where its head lands
TEST(LiveLowering, PlaysAWriteFastAtMostAtTheSlewRate) {
    constexpr int64_t NOW = 5 * WINDOW;
    constexpr int64_t LEAD = 150000;
    struct SlewRow {
        std::string name;
        /// The debt left before the write, the write's frames, and its excess
        int64_t debt_us;
        uint64_t frames;
        int64_t excess_us;
        int32_t ppm;
    };
    const SlewRow rows[] = {
        {"Control: nothing owed plays at the rate it came", 0, RATE / 50, 200000, 0},
        {"A large debt plays at the slew rate", 200000, RATE / 50, 200000,
         static_cast<int32_t>(LOWERING_SLEW_PPM)},
        // 20 ms at 100 ppm takes out the 2 us owed
        {"A debt short of the rate's share plays only it out", 2, RATE / 50, 200000, 100},
        {"A write's own excess caps the debt it plays out", 200000, RATE / 50, 2, 100},
        {"No frames play nothing out", 200000, 0, 200000, 0},
    };
    for (const SlewRow& row : rows) {
        SCOPED_TRACE(row.name);
        // The window that ended at NOW owed the row's debt
        LiveLowering lowering{NOW, row.debt_us, 0, row.debt_us};
        EXPECT_EQ(live_slew_ppm(lowering, row.frames, RATE, NOW + LEAD + row.excess_us, NOW, LEAD),
                  row.ppm);
    }
}

// What a slew took out lowers the debt and `G_eff` by as much, `G_eff` never below `G_next`
TEST(LiveLowering, LowersGEffByWhatTheSlewTookOut) {
    struct FollowRow {
        std::string name;
        int64_t lead_eff_us;
        int64_t slewed_us;
        int64_t lead_eff_after_us;
        int64_t debt_after_us;
    };
    constexpr int64_t NEXT = 150000;
    const FollowRow rows[] = {
        {"Control: nothing taken out leaves G_eff", 400000, 0, 400000, 1000},
        {"G_eff follows the lead down", 400000, 600, 399400, 400},
        {"Never below G_next", NEXT + 100, 600, NEXT, 400},
        {"The debt never goes below nothing", 400000, 2000, 398000, 0},
    };
    for (const FollowRow& row : rows) {
        SCOPED_TRACE(row.name);
        LiveLowering lowering{0, 0, 0, 1000};
        EXPECT_EQ(record_slew(lowering, row.slewed_us, NEXT, row.lead_eff_us),
                  row.lead_eff_after_us);
        EXPECT_EQ(lowering.debt_us, row.debt_after_us);
        EXPECT_EQ(lowering.slewed_us, row.slewed_us);
    }
}

// Twenty minutes of 20 ms writes on time, their lead 250 ms past `G_next` after a high-lead member
// left: the slew takes out each write's share at the slew rate until the excess is gone, `G_eff`
// following the lead down to `G_next` and staying at or above it, the lead never below `G_next`.
// A joiner raising `G_next` mid-slew ends what its raise covers, and the slew ends sooner.
TEST(LiveLowering, SlewsAnExcessAwayAtTheRateWithGEffFollowing) {
    constexpr int64_t WRITE_US = 20000;
    constexpr auto FRAMES = static_cast<uint64_t>(RATE * WRITE_US / 1000000);
    constexpr int64_t EXCESS = 250000;
    for (const int64_t raise : {int64_t{0}, int64_t{200000}}) {
        SCOPED_TRACE(raise);
        int64_t next = 150000;
        // A lead and G_eff the leaver's raise left, the lead its rebase's
        int64_t lead = next + EXCESS;
        int64_t lead_eff = lead;
        LiveLowering lowering;
        // Frames the slew took out, a fraction kept from write to write as dropping frames does
        int64_t parts = 0;
        int64_t slewed_frames = 0;
        int64_t reported = 0;
        int64_t done_at = -1;
        for (int64_t now = 0; now < 20 * 60 * 1000000; now += WRITE_US) {
            if (raise > 0 && now == 60 * 1000000) {
                next += raise;
                lead_eff = std::max(lead_eff, next);
            }
            const int32_t ppm = live_slew_ppm(lowering, FRAMES, RATE, now + lead, now, next);
            ASSERT_GE(ppm, 0);
            ASSERT_LE(ppm, LOWERING_SLEW_PPM);
            parts += static_cast<int64_t>(FRAMES) * ppm;
            const int64_t dropped = parts / 1000000;
            parts -= dropped * 1000000;
            slewed_frames += dropped;
            const int64_t total_us = slewed_frames * 1000000 / RATE;
            // A write's frames play for less by what was dropped
            lead -= total_us - reported;
            lead_eff = record_slew(lowering, total_us - reported, next, lead_eff);
            reported = total_us;
            ASSERT_GE(lead_eff, next) << now;
            // The lead never falls below what every player needs
            ASSERT_GE(lead, next) << now;
            // G_eff follows the lead down exactly while above G_next
            if (lead_eff > next) {
                ASSERT_EQ(lead_eff, lead) << now;
            }
            if (done_at < 0 && lowering.debt_us == 0 && lead - next < MIN) {
                done_at = now;
            }
        }
        // All the excess the raise left, at the rate: 250 ms at 300 ppm takes about 833 s, and
        // the 50 ms a 200 ms raise leaves about 167 s, after the first window's wait
        EXPECT_LT(lead - next, 1000) << lead;
        EXPECT_EQ(lead_eff, next);
        const int64_t slew_us = (EXCESS - raise) * 1000000 / LOWERING_SLEW_PPM;
        EXPECT_GE(done_at, slew_us) << done_at;
        EXPECT_LE(done_at, slew_us + (2 * WINDOW)) << done_at;
    }
}

}  // namespace
