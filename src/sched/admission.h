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

/// @file admission.h
/// @brief Write admission and readiness for a group input. Pure functions.

#pragma once

#include "sched/timeline.h"
#include "sched/types.h"

#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief The host view's state that admission reads
struct AdmissionInput {
    InputKind kind{InputKind::BUFFERED};
    RunState run{RunState::NONE};
    bool draining{false};
    bool latched{false};
    /// The next position to be written
    uint64_t head{0};
    /// `p0`: the current epoch's first position, a pending one's included
    uint64_t p0{0};
    /// The paused position; with run PAUSED
    uint64_t p_pause{0};
    /// The rate of the segment holding the head
    uint32_t head_rate{0};
    /// `G_eff` while playing; `G_next`, which a write into a pending epoch or a new run anchors
    /// with, otherwise
    int64_t group_lead_us{0};
    /// `W`
    int64_t write_ahead_us{0};
    /// The highest position the PCM ring has room for
    uint64_t ring_limit{0};
    /// `k*` as the group audio thread last published it, and the parameters of the rebase it
    /// decides, for admission as if rebased; nullopt before a publish, which admits from the plain
    /// horizon
    std::optional<int64_t> shift_index;
    RebaseParams rebase;
};

/// @brief `P_max`: the position up to which input is accepted. Playing: up to the horizon
/// `H = now + G_eff + W` in timeline time, the head's time counted as if the rebase frame `k*`
/// would trigger now had happened. A write into a pending epoch, or one that begins a run,
/// anchors at now first, so it admits `p0 + W`. A paused buffered input is frozen at `p_pause + W`;
/// a paused live input is bounded only by the ring. The head while draining or latched. Always
/// within the ring; below the head when the input is past it.
uint64_t admission_limit(const AdmissionInput& in, const Anchor& anchor, int64_t now_us);

/// @brief The samples admitted past the head: `P_max - head`, or 0 below it
uint64_t admitted_past_head(const AdmissionInput& in, const Anchor& anchor, int64_t now_us);

struct WriteDecision {
    uint64_t frames{0};
    InputState state{InputState::OPEN};
    /// `time_of(head)` while playing, as admission read it before the pending rebase it adds for
    /// `t_head'`; nullopt otherwise
    std::optional<int64_t> head_us;
};

/// @brief What write() takes of `offered` samples and why it took fewer
WriteDecision decide_write(const AdmissionInput& in, const Anchor& anchor, uint64_t offered,
                           int64_t now_us);

/// @brief `t_ready`: when at least r_min samples past the head are admitted. now_us when they
/// already are; while playing, the first instant the horizon covers them; NEVER while draining,
/// latched or paused buffered, or while the next frame would trigger a rebase, which only an event
/// changes.
int64_t readiness_time(const AdmissionInput& in, const Anchor& anchor, uint64_t r_min,
                       int64_t now_us);

}  // namespace sendspin::server::sched
