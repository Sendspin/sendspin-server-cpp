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
/// @brief A live input's latency lowering: the excess a window of writes kept, cut once the window
/// ends, never more than the window measured and never twice

#include "sched/lowering.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using sendspin::server::sched::LiveLowering;
using sendspin::server::sched::LOWERING_MIN_US;
using sendspin::server::sched::LOWERING_WINDOW_US;
using sendspin::server::sched::observe_live_write;
using sendspin::server::sched::record_cut;

constexpr int64_t WINDOW = LOWERING_WINDOW_US;
constexpr int64_t MIN = LOWERING_MIN_US;

/// One write: when, its excess with no cut made, and the debt it should see; the debt it sees is
/// cut at once
struct Write {
    int64_t at_us;
    int64_t excess_us;
    int64_t debt_us;
};

struct Row {
    std::string name;
    std::vector<Write> writes;
};

/// The excess a write shows is lower by every cut made before it, as the cut audio no longer lands
TEST(LiveLowering, CutsTheExcessAWholeWindowKept) {
    const Row rows[] = {
        {"Control: an excess below the minimum stays",
         {{0, MIN - 1, 0}, {WINDOW / 2, MIN + 5000, 0}, {WINDOW, MIN + 5000, 0}}},
        {"An excess of the minimum throughout is cut at the window's end",
         {{0, MIN, 0}, {WINDOW / 2, MIN + 5000, 0}, {WINDOW, MIN + 9000, MIN}}},
        {"The window's least excess is cut, the latest write landing as a fresh anchor would",
         {{0, 300000, 0}, {WINDOW / 2, 250000, 0}, {WINDOW, 280000, 250000}}},
        {"A window not yet over decides nothing", {{0, 300000, 0}, {WINDOW - 1, 300000, 0}}},
        {"Control: one late write in the window keeps the latency",
         {{0, 300000, 0}, {WINDOW / 2, -1000, 0}, {WINDOW, 300000, 0}}},
        {"Once cut, the next window sees no excess left and cuts nothing more",
         {{0, 100000, 0},
          {WINDOW, 100000, 100000},
          {WINDOW + 1, 100000, 0},
          {2 * WINDOW + 1, 100000, 0}}},
        {"An excess that rises after a cut is cut once a whole window kept it",
         {{0, 200000, 0},
          {WINDOW, 200000, 200000},
          {WINDOW + 10, 260000, 0},
          {2 * WINDOW + 10, 260000, 0},
          {3 * WINDOW + 10, 260000, 60000}}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        LiveLowering lowering;
        int64_t cut = 0;
        for (size_t i = 0; i < row.writes.size(); ++i) {
            SCOPED_TRACE(i);
            const Write& write = row.writes[i];
            const int64_t debt = observe_live_write(lowering, write.excess_us - cut, write.at_us);
            EXPECT_EQ(debt, write.debt_us);
            if (debt > 0) {
                record_cut(lowering, debt);
                cut += debt;
            }
        }
    }
}

/// A debt larger than one write's audio is cut across the writes that follow; a window that then
/// measures less left than owed lowers the debt to it, so a cut never passes the excess measured
TEST(LiveLowering, CarriesADebtAcrossWritesBoundedByTheLatestWindow) {
    LiveLowering lowering;
    EXPECT_EQ(observe_live_write(lowering, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(lowering, 500000, WINDOW), 500000);
    // Writes of 100 ms each pay a fifth
    record_cut(lowering, 100000);
    EXPECT_EQ(observe_live_write(lowering, 400000, WINDOW + 20000), 400000);
    record_cut(lowering, 100000);
    // A rebase or late writes left only 30 ms of excess: below the minimum, the debt falls to it
    EXPECT_EQ(observe_live_write(lowering, 30000, 2 * WINDOW + 20000), 30000);
    record_cut(lowering, 30000);
    EXPECT_EQ(observe_live_write(lowering, 0, 2 * WINDOW + 40000), 0);
    // Control: a late write measures none left, which ends a debt
    LiveLowering late;
    EXPECT_EQ(observe_live_write(late, 500000, 0), 0);
    EXPECT_EQ(observe_live_write(late, 500000, WINDOW), 500000);
    record_cut(late, 100000);
    EXPECT_EQ(observe_live_write(late, -20000, 2 * WINDOW), 0);
}

}  // namespace
