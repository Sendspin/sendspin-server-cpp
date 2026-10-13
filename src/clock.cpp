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

#include "clock.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <ctime>
#endif

namespace sendspin::server {

namespace {

constexpr int64_t NS_PER_US = 1000;

}  // namespace

#if defined(__APPLE__)

int64_t now_us() {
    static const mach_timebase_info_data_t TIMEBASE = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const uint64_t ticks = mach_continuous_time();
    // Split so that ticks * numer cannot overflow
    const uint64_t whole = ticks / TIMEBASE.denom;
    const uint64_t rest = ticks % TIMEBASE.denom;
    const uint64_t ns = (whole * TIMEBASE.numer) + (rest * TIMEBASE.numer / TIMEBASE.denom);
    return static_cast<int64_t>(ns) / NS_PER_US;
}

#else

int64_t now_us() {
    constexpr int64_t US_PER_S = 1000000;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * US_PER_S) + (ts.tv_nsec / NS_PER_US);
}

#endif

}  // namespace sendspin::server
