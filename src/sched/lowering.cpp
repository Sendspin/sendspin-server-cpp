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
    // As if nothing had been slewed, so the slew during a window does not lower its least
    const int64_t counted = excess_us + lowering.slewed_us;
    if (!lowering.window_start_us) {
        lowering.window_start_us = now_us;
        lowering.window_least_us = counted;
    }
    lowering.window_least_us = std::min(lowering.window_least_us, counted);
    if (now_us - *lowering.window_start_us >= LOWERING_WINDOW_US) {
        const int64_t remaining = lowering.window_least_us - lowering.slewed_us;
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

int32_t live_slew_ppm(LiveLowering& lowering, uint64_t frames, uint32_t rate, int64_t head_us,
                      int64_t now_us, int64_t group_lead_next_us) {
    const int64_t debt =
        observe_live_write(lowering, head_us - now_us - group_lead_next_us, now_us);
    const auto write_us = static_cast<int64_t>(frames) * US_PER_SECOND / rate;
    if (debt <= 0 || write_us == 0) {
        return 0;
    }
    // The slew never takes out more than is owed, so the lead never falls below `G_next`
    return static_cast<int32_t>(std::min(LOWERING_SLEW_PPM, debt * US_PER_SECOND / write_us));
}

int64_t record_slew(LiveLowering& lowering, int64_t slewed_us, int64_t group_lead_next_us,
                    int64_t group_lead_eff_us) {
    lowering.slewed_us += slewed_us;
    lowering.debt_us = std::max<int64_t>(lowering.debt_us - slewed_us, 0);
    return std::max(group_lead_next_us, group_lead_eff_us - slewed_us);
}

}  // namespace sendspin::server::sched
