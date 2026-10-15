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

/// @file test_scheduled_updates.cpp
/// @brief ScheduledUpdates, a current state and at most one scheduled update, through the color
/// schedule.

#include "colors.h"
#include "scheduled_updates.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using sendspin::server::Colors;
using sendspin::server::ColorState;
using sendspin::server::RgbColor;

// spec roles/color/v1.md "Scheduled color updates", through the schedule metadata shares
TEST(ColorSchedule, SendsCurrentAndScheduledStates) {
    constexpr int64_t S = 1000000;
    sendspin::server::ScheduledUpdates<ColorState> colors;
    Colors red;
    red.primary = RgbColor{255, 0, 0};
    EXPECT_EQ(colors.current(5).timestamp_us, 5);
    EXPECT_EQ(colors.current(5).colors, Colors{});
    std::vector<ColorState> sends = colors.set({S, red}, 2 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0], (ColorState{S, red}));
    Colors blue;
    blue.primary = RgbColor{0, 0, 255};
    EXPECT_TRUE(colors.set({30 * S, blue}, 3 * S).empty());
    EXPECT_EQ(colors.next_due_us(), 10 * S);
    sends = colors.advance(10 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0].colors, blue);
    EXPECT_EQ(colors.sent_pending(), (ColorState{30 * S, blue}));
    // Cancelled once sent: the current state again
    sends = colors.cancel(11 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0], (ColorState{S, red}));
}

}  // namespace
