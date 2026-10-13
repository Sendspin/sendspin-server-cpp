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

#include "sched/leads.h"

#include "sched/timeline.h"

#include <algorithm>

namespace sendspin::server::sched {

PlayerLead player_lead_us(const PlayerTiming& timing, InputKind kind, int64_t t_clamp_us) {
    const int64_t mb = std::min(timing.min_buffer_ms * US_PER_MS, t_clamp_us);
    const int64_t rl = std::min(timing.required_lead_time_ms * US_PER_MS, t_clamp_us);
    const int64_t od = timing.output_delay_ms * US_PER_MS;
    const int64_t floor = mb + od;
    // required_lead_time_ms is a hint honored only where it adds no latency (spec player/v1.md
    // "Server Audio Send Constraints")
    const int64_t lead = kind == InputKind::LIVE ? floor : std::max(mb, rl) + od;
    return {lead, floor};
}

int64_t pipeline_latency_us(const PipelineShape& shape, InputKind kind, int64_t jitter_us) {
    if (kind == InputKind::BUFFERED) {
        return 0;
    }
    int64_t lookahead = 0;
    if (shape.resampler_filter_length != 0) {
        lookahead = ceil_div((int64_t{shape.resampler_filter_length} / 2 + 1) * US_PER_SECOND,
                             shape.input_rate);
    }
    return int64_t{shape.frame_cells} * CELL_US + lookahead + shape.encoder_delay_us + jitter_us;
}

int64_t group_lead_us(std::span<const LeadInput> members, const SchedTiming& timing) {
    if (members.empty()) {
        return timing.group_default_us;
    }
    int64_t largest = 0;
    for (const LeadInput& member : members) {
        largest = std::max(largest, std::max(timing.join_min_us, member.lead.lead_us) +
                                        member.pipeline_latency_us);
    }
    return largest + timing.pump_margin_us;
}

int64_t startup_lead_us(const PlayerLead& lead, TriggerKind kind, int64_t join_min_us) {
    const int64_t wanted = kind == TriggerKind::GROUP ? lead.floor_us : lead.lead_us;
    return std::max(wanted, join_min_us);
}

int64_t startup_min_ts(int64_t stamp_us, const PlayerLead& lead, TriggerKind kind,
                       int64_t join_min_us) {
    return stamp_us + startup_lead_us(lead, kind, join_min_us);
}

int64_t write_ahead_us(std::span<const CapacityRate> members, int64_t w_min_us, int64_t w_max_us) {
    int64_t largest = 0;
    for (const CapacityRate& member : members) {
        if (member.bytes_per_second > 0) {
            largest =
                std::max(largest, member.capacity_bytes * US_PER_SECOND / member.bytes_per_second);
        }
    }
    return std::clamp(largest, w_min_us, w_max_us);
}

}  // namespace sendspin::server::sched
