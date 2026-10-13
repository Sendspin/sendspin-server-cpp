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

#include "sched/admission.h"

#include "sched/constants.h"

#include <algorithm>

namespace sendspin::server::sched {

namespace {

/// The whole samples in a span of time at rate
uint64_t samples_in(int64_t span_us, uint32_t rate) {
    return span_us <= 0 ? 0 : static_cast<uint64_t>(floor_div(span_us * rate, US_PER_SECOND));
}

/// The rebase frame `k*` would trigger at now, or 0
int64_t pending_rebase_us(const AdmissionInput& in, const Anchor& anchor, int64_t now_us) {
    if (in.run != RunState::PLAYING || !in.shift_index) {
        return 0;
    }
    const int64_t lead = frame_timestamp(anchor, *in.shift_index) - now_us;
    return rebase_needed(lead, in.rebase) ? rebase_delta(lead, in.rebase) : 0;
}

/// The head's time while playing, which uncapped_limit() takes
std::optional<int64_t> playing_head_us(const AdmissionInput& in, const Anchor& anchor) {
    if (in.run != RunState::PLAYING) {
        return std::nullopt;
    }
    return time_of(anchor, in.head);
}

/// P_max before the PCM ring's cap; head_us is playing_head_us()
uint64_t uncapped_limit(const AdmissionInput& in, const Anchor& anchor, int64_t now_us,
                        std::optional<int64_t> head_us) {
    switch (in.run) {
        case RunState::PAUSED:
            return in.kind == InputKind::LIVE
                       ? in.ring_limit
                       : in.p_pause + samples_in(in.write_ahead_us, in.head_rate);
        case RunState::PLAYING: {
            const int64_t horizon = now_us + in.group_lead_us + in.write_ahead_us;
            // `t_head'`, as if rebased
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): set while playing
            const int64_t rebased_us = *head_us + pending_rebase_us(in, anchor, now_us);
            return in.head + samples_in(horizon - rebased_us, in.head_rate);
        }
        case RunState::NONE:
        case RunState::PENDING:
            break;
    }
    // A write that begins a run or anchors a pending epoch anchors p0 at now, so it admits W
    // from p0 whatever is already staged
    return in.p0 + samples_in(in.write_ahead_us, in.head_rate);
}

/// admission_limit() with the head's time read once
uint64_t limit_at(const AdmissionInput& in, const Anchor& anchor, int64_t now_us,
                  std::optional<int64_t> head_us) {
    if (in.draining || in.latched) {
        return in.head;
    }
    return std::min(uncapped_limit(in, anchor, now_us, head_us), in.ring_limit);
}

}  // namespace

uint64_t admission_limit(const AdmissionInput& in, const Anchor& anchor, int64_t now_us) {
    return limit_at(in, anchor, now_us, playing_head_us(in, anchor));
}

uint64_t admitted_past_head(const AdmissionInput& in, const Anchor& anchor, int64_t now_us) {
    const uint64_t limit = admission_limit(in, anchor, now_us);
    return limit > in.head ? limit - in.head : 0;
}

WriteDecision decide_write(const AdmissionInput& in, const Anchor& anchor, uint64_t offered,
                           int64_t now_us) {
    const std::optional<int64_t> head_us = playing_head_us(in, anchor);
    if (in.latched) {
        return {0, InputState::STOP_LATCHED, head_us};
    }
    if (in.draining) {
        return {0, InputState::DRAINING, head_us};
    }
    const uint64_t limit = limit_at(in, anchor, now_us, head_us);
    const uint64_t room = limit > in.head ? limit - in.head : 0;
    if (offered > room) {
        return {room, InputState::FULL, head_us};
    }
    return {offered, InputState::OPEN, head_us};
}

int64_t readiness_time(const AdmissionInput& in, const Anchor& anchor, uint64_t r_min,
                       int64_t now_us) {
    if (in.draining || in.latched) {
        return NEVER;
    }
    if (admitted_past_head(in, anchor, now_us) >= r_min) {
        return now_us;
    }
    if (in.run != RunState::PLAYING || in.head + r_min > in.ring_limit ||
        pending_rebase_us(in, anchor, now_us) != 0) {
        return NEVER;
    }
    // The inverse of the playing limit: the first instant it admits r_min past the head
    const int64_t needed_us = ceil_div(static_cast<int64_t>(r_min) * US_PER_SECOND, in.head_rate);
    return time_of(anchor, in.head) + needed_us - in.group_lead_us - in.write_ahead_us;
}

}  // namespace sendspin::server::sched
