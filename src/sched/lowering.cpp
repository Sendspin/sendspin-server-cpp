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

#include "sched/lowering.h"

#include <algorithm>

namespace sendspin::server::sched {

int64_t observe_live_write(LiveLowering& lowering, int64_t excess_us, int64_t now_us) {
    // As if no cut had been made, so the cuts during a window do not lower its least
    const int64_t counted = excess_us + lowering.cut_us;
    if (!lowering.window_start_us) {
        lowering.window_start_us = now_us;
        lowering.window_least_us = counted;
        return lowering.debt_us;
    }
    lowering.window_least_us = std::min(lowering.window_least_us, counted);
    if (now_us - *lowering.window_start_us >= LOWERING_WINDOW_US) {
        const int64_t remaining = lowering.window_least_us - lowering.cut_us;
        // A debt never outgrows what the window measured; one still owed below the minimum is
        // finished
        lowering.debt_us = remaining >= LOWERING_MIN_US
                               ? remaining
                               : std::min(lowering.debt_us, std::max<int64_t>(remaining, 0));
        lowering.window_start_us = now_us;
        lowering.window_least_us = counted;
    }
    return lowering.debt_us;
}

void record_cut(LiveLowering& lowering, int64_t cut_us) {
    lowering.cut_us += cut_us;
    lowering.debt_us = std::max<int64_t>(lowering.debt_us - cut_us, 0);
}

}  // namespace sendspin::server::sched
