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

/// @file test_clock.cpp
/// @brief now_us() counts microseconds on the system's raw monotonic clock.

#include "clock.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <ctime>

namespace {

using sendspin::server::now_us;

// The clock now_us() reads: on macOS CLOCK_MONOTONIC_RAW is mach_continuous_time, while
// CLOCK_MONOTONIC is adjusted and drifts from it, on Linux by NTP's slew.
int64_t monotonic_us() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * 1000000) + (ts.tv_nsec / 1000);
}

// Each read lies between reference reads taken just before and just after it, so scheduling
// cannot fail this, and reads never go back. Both clocks count from boot, so a timebase, unit or
// rate error shows as an offset of the uptime's order rather than needing a sleep to measure.
TEST(Clock, ReadsTheMonotonicClockInMicroseconds) {
    // Both convert the same counter, each rounding down, so they differ by a microsecond at most
    constexpr int64_t TOLERANCE_US = 2;
    constexpr int READS = 10000;
    int64_t previous = now_us();
    for (int i = 0; i < READS; ++i) {
        const int64_t before = monotonic_us();
        const int64_t now = now_us();
        const int64_t after = monotonic_us();
        ASSERT_GE(now, before - TOLERANCE_US) << "read " << i;
        ASSERT_LE(now, after + TOLERANCE_US) << "read " << i;
        ASSERT_GE(now, previous) << "read " << i;
        previous = now;
    }
}

}  // namespace
