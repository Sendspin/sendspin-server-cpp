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

#include "sched/stalls.h"

#include "sched/constants.h"

#include <utility>

namespace sendspin::server::sched {

void StallCount::add_rebase(int64_t shifted_us) {
    ++this->counted_.rebases;
    this->counted_.shifted_us += shifted_us;
}

void StallCount::add_late_frame() {
    ++this->counted_.late_frames;
}

StallTake StallCount::take(int64_t now_us) {
    if (this->counted_.rebases == 0 && this->counted_.late_frames == 0) {
        return {};
    }
    if (this->reported_us_ && now_us - *this->reported_us_ < STALL_REPORT_US) {
        return {std::nullopt, *this->reported_us_ + STALL_REPORT_US};
    }
    this->reported_us_ = now_us;
    return {std::exchange(this->counted_, {}), std::nullopt};
}

}  // namespace sendspin::server::sched
