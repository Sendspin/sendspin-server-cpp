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

/// @file lowering.h
/// @brief Bringing a live input's latency back down within an epoch: once every write of a window
/// landed later than a fresh anchor would place it, the input cuts that excess from the writes
/// that follow. Pure functions.

#pragma once

#include "sched/constants.h"

#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief One live epoch's lowering state, from its anchor
struct LiveLowering {
    /// When the current window began; nullopt before the epoch's first observation
    std::optional<int64_t> window_start_us;
    /// The least excess of the window, each counted as it would be without the cuts made since
    /// the epoch's anchor
    int64_t window_least_us{0};
    /// The time cut since the epoch's anchor
    int64_t cut_us{0};
    /// What the last window decided to cut and later writes have not yet cut
    int64_t debt_us{0};
};

/// @brief Observes a live write of a playing epoch. Its excess is `time_of(head) - now - G_next`:
/// how much later the write's first sample lands than an anchor made at now would place it, the
/// fresh anchor's lead being the least that serves every player. Windows of LOWERING_WINDOW_US
/// follow one another from the epoch's first observation; one that ends with every excess in it at
/// or above LOWERING_MIN_US sets the debt to its least excess, net of the cuts made since, so the
/// latest write in it would have landed as if anchored then. Every observation caps the debt at its
/// own excess, so a lead that rose or a write that came late since ends what is no longer owed.
/// @return the debt: the time to cut from this write's end, then report with record_cut()
int64_t observe_live_write(LiveLowering& lowering, int64_t excess_us, int64_t now_us);

/// @brief A cut of cut_us was made: the debt falls by it
void record_cut(LiveLowering& lowering, int64_t cut_us);

}  // namespace sendspin::server::sched
