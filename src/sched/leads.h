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

/// @file leads.h
/// @brief Per-player leads, pipeline latency, the group lead, the first chunk after a trigger and
/// the buffered write-ahead. Pure functions.

#pragma once

#include "sched/constants.h"
#include "sched/types.h"

#include <cstdint>
#include <span>

namespace sendspin::server::sched {

/// @brief A player's timing values from its latest client/state (spec player/v1.md "client/state
/// player object")
struct PlayerTiming {
    int64_t min_buffer_ms{0};
    int64_t required_lead_time_ms{0};
    int64_t output_delay_ms{0};
};

struct PlayerLead {
    /// `lead_i`: the send-ahead the player asks for, its output delay included
    int64_t lead_us{0};
    /// `floor_i = mb_i + od_i`, the part the spec makes a MUST (spec player/v1.md "Server Audio
    /// Send Constraints")
    int64_t floor_us{0};
};

/// @brief `lead_i` and `floor_i`, with `min_buffer_ms` and `required_lead_time_ms` clamped at
/// t_clamp_us; `required_lead_time_ms` counts only for a buffered input
PlayerLead player_lead_us(const PlayerTiming& timing, InputKind kind, int64_t t_clamp_us);

/// @brief A pipeline's frame length and its resampler, if any
struct PipelineShape {
    uint32_t frame_cells{1};
    /// `N_p`; 0 without a resampler
    uint32_t resampler_filter_length{0};
    uint32_t input_rate{0};
    /// The encoder's delay: the input it reads past a frame's end before it encodes the frame
    int64_t encoder_delay_us{0};
};

/// @brief `L_p`: 0 for a buffered input; one frame plus the resampler lookahead, the encoder's
/// delay and the write jitter for a live one
int64_t pipeline_latency_us(const PipelineShape& shape, InputKind kind, int64_t jitter_us);

/// @brief One eligible member's lead and its pipeline's latency
struct LeadInput {
    PlayerLead lead;
    int64_t pipeline_latency_us{0};
};

/// @brief `G = max over members of (max(J_min, lead_i) + L_p(i)) + M_pump`, or `G_default` with no
/// eligible member
int64_t group_lead_us(std::span<const LeadInput> members, const SchedTiming& timing);

/// @brief The startup lead of a trigger: `max(floor_i, J_min)` after a group trigger, `max(lead_i,
/// J_min)` after a join trigger
int64_t startup_lead_us(const PlayerLead& lead, TriggerKind kind, int64_t join_min_us);

/// @brief `min_ts`: the earliest timestamp of the first chunk after a trigger stamped stamp_us
int64_t startup_min_ts(int64_t stamp_us, const PlayerLead& lead, TriggerKind kind,
                       int64_t join_min_us);

/// @brief A member's `buffer_capacity` and its pipeline's byte rate, chunk headers included
struct CapacityRate {
    int64_t capacity_bytes{0};
    int64_t bytes_per_second{0};
};

/// @brief A buffered input's `W`: the largest member capacity in time, clamped to [w_min, w_max];
/// w_min with no member
int64_t write_ahead_us(std::span<const CapacityRate> members, int64_t w_min_us, int64_t w_max_us);

}  // namespace sendspin::server::sched
