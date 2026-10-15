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

/// @file test_sched_stalls.cpp
/// @brief A group's stall count: the rebases and late frames since the last report, reported at
/// once when no report was made and at most every STALL_REPORT_US after one, held back counts
/// due when the period ends

#include "sched/constants.h"
#include "sched/stalls.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::sched::STALL_REPORT_US;
using sendspin::server::sched::StallCount;
using sendspin::server::sched::StallReport;
using sendspin::server::sched::StallTake;

constexpr int64_t PERIOD = STALL_REPORT_US;
/// Any instant a test starts its count at
constexpr int64_t T0 = 5'000'000;

/// One step: a rebase of shifted_us, a late frame, or a take at at_us with what it should decide
struct Step {
    enum class Kind { REBASE, LATE, TAKE } kind;
    int64_t value{0};
    std::optional<StallReport> report{};
    std::optional<int64_t> due_us{};
};

Step rebase(int64_t shifted_us) {
    return {Step::Kind::REBASE, shifted_us};
}
Step late() {
    return {Step::Kind::LATE};
}
Step take(int64_t at_us, std::optional<StallReport> report, std::optional<int64_t> due_us) {
    return {Step::Kind::TAKE, at_us, report, due_us};
}

struct Row {
    std::string name;
    std::vector<Step> steps;
};

TEST(StallCount, ReportsAtMostEveryPeriodAndHoldsTheRestBack) {
    const Row rows[] = {
        {"Control: nothing counted reports nothing", {take(T0, std::nullopt, std::nullopt)}},
        {"A late frame with no report made is reported at once",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt)}},
        {"Rebases are counted with their shifts summed, beside late frames",
         {rebase(300'000), late(), rebase(200'000), late(),
          take(T0, StallReport{2, 500'000, 2}, std::nullopt)}},
        {"A report starts the count again",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt),
          take(T0 + PERIOD, std::nullopt, std::nullopt)}},
        {"Within the period after a report, counts are held back until it ends",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt), rebase(40'000),
          take(T0 + PERIOD - 1, std::nullopt, T0 + PERIOD)}},
        {"Held back counts are reported once the period ends, with those that followed",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt), rebase(40'000),
          take(T0 + 1, std::nullopt, T0 + PERIOD), late(),
          take(T0 + PERIOD, StallReport{1, 40'000, 1}, std::nullopt)}},
        {"The period runs from the last report",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt), late(),
          take(T0 + (2 * PERIOD), StallReport{0, 0, 1}, std::nullopt), late(),
          take(T0 + (2 * PERIOD) + 1, std::nullopt, T0 + (3 * PERIOD))}},
        {"Control: within the period with nothing counted, nothing is due",
         {late(), take(T0, StallReport{0, 0, 1}, std::nullopt),
          take(T0 + 1, std::nullopt, std::nullopt)}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        StallCount count;
        for (size_t i = 0; i < row.steps.size(); ++i) {
            SCOPED_TRACE(i);
            const Step& step = row.steps[i];
            switch (step.kind) {
                case Step::Kind::REBASE:
                    count.add_rebase(step.value);
                    break;
                case Step::Kind::LATE:
                    count.add_late_frame();
                    break;
                case Step::Kind::TAKE: {
                    const StallTake taken = count.take(step.value);
                    ASSERT_EQ(taken.report.has_value(), step.report.has_value());
                    if (step.report) {
                        EXPECT_EQ(taken.report->rebases, step.report->rebases);
                        EXPECT_EQ(taken.report->shifted_us, step.report->shifted_us);
                        EXPECT_EQ(taken.report->late_frames, step.report->late_frames);
                    }
                    EXPECT_EQ(taken.due_us, step.due_us);
                    break;
                }
            }
        }
    }
}

}  // namespace
