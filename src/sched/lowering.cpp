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
    lowering.debt_us = std::min(lowering.debt_us, std::max<int64_t>(excess_us, 0));
    return lowering.debt_us;
}

LoweredWrite lower_live_write(LiveLowering& lowering, uint64_t frames, uint32_t rate,
                              int64_t head_us, int64_t now_us, int64_t group_lead_next_us,
                              int64_t group_lead_eff_us) {
    const int64_t excess = head_us - now_us - group_lead_next_us;
    const int64_t debt = observe_live_write(lowering, excess, now_us);
    const auto cut = std::min<uint64_t>(
        frames, static_cast<uint64_t>(std::max<int64_t>(debt, 0) * rate / US_PER_SECOND));
    if (cut == 0) {
        return {frames, group_lead_eff_us};
    }
    const auto cut_us = static_cast<int64_t>(cut) * US_PER_SECOND / rate;
    record_cut(lowering, cut_us);
    // The debt never passes the excess, so neither does the cut: never below `G_next`
    return {frames - cut, std::min(group_lead_eff_us, group_lead_next_us + excess - cut_us)};
}

void record_cut(LiveLowering& lowering, int64_t cut_us) {
    lowering.cut_us += cut_us;
    lowering.debt_us = std::max<int64_t>(lowering.debt_us - cut_us, 0);
}

}  // namespace sendspin::server::sched
