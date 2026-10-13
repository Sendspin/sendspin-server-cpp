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

/// @file constants.h
/// @brief The send path's constants and its tunable timing defaults. Every time is in microseconds.

#pragma once

#include "profile.h"

#include <cstdint>
#include <limits>

namespace sendspin::server::sched {

constexpr int64_t US_PER_SECOND = 1000000;
constexpr int64_t US_PER_MS = 1000;

/// @brief No deadline
constexpr int64_t NEVER = std::numeric_limits<int64_t>::max();

/// @brief `F`: the cell, the unit every frame length is a whole number of
constexpr int64_t CELL_US = 20000;
constexpr uint32_t CELLS_PER_SECOND = 50;

/// @brief The longest frame, in cells, that one chunk can carry; a format whose frame is longer is
/// not producible
constexpr uint32_t MAX_FRAME_CELLS = 7;

/// @brief `n_target`: the chunk length packing aims for, in cells
constexpr uint32_t TARGET_CHUNK_CELLS = 5;
/// @brief `S_solo`: a first frame with less slack than this goes in a chunk alone
constexpr int64_t SOLO_SLACK_US = 50000;
/// @brief The longest chunk (spec player/v1.md "Server Audio Send Constraints")
constexpr int64_t MAX_CHUNK_US = 150000;

/// @brief A chunk's header: type byte, timestamp and send_ahead (spec player/v1.md "Audio
/// Chunks")
constexpr int64_t CHUNK_HEADER_BYTES = 13;
constexpr uint8_t MESSAGE_ID_AUDIO_CHUNK = 4;

/// @brief The deepest FLAC sample the encoder takes
constexpr uint8_t MAX_FLAC_BIT_DEPTH = 24;

/// @brief `P_prep`: cells between a cursor and a new pipeline's first frame at a format switch
constexpr int64_t SWITCH_PREP_CELLS = 2;

/// @brief A live input's latency lowering (lowering.h): how long every write's excess must stay at
/// or above LOWERING_MIN_US before a cut. A hundred 20 ms writes, so a transient excess is not cut
constexpr int64_t LOWERING_WINDOW_US = 2000000;
/// @brief The least excess cut, two cells; a smaller one stays in the timeline
constexpr int64_t LOWERING_MIN_US = 2 * CELL_US;

namespace timing_defaults {
constexpr int64_t PUMP_MARGIN_US = 10000;
constexpr int64_t JOIN_MIN_US = 100000;
constexpr int64_t GROUP_DEFAULT_US = 250000;
constexpr int64_t WRITE_AHEAD_MIN_US = 1000000;
constexpr int64_t WRITE_AHEAD_MAX_US = profile::WRITE_AHEAD_MAX_US;
constexpr int64_t WRITE_AHEAD_LIVE_US = 200000;
constexpr int64_t TIMING_CLAMP_US = 10000000;
constexpr int64_t ARTWORK_AGE_US = 250000;
}  // namespace timing_defaults

/// @brief `S_buf`: the least `S_pack` of a buffered input's player, a robustness margin. A buffered
/// stream's slack runs to seconds in steady state, so it still packs to `n_target`, while one
/// caught up to production keeps a queue deep enough for a loaded host or Wi-Fi
constexpr int64_t BUFFERED_HOLD_SLACK_US = 500000;

/// @brief `S_pack` at the default `M_pump` and a `min_buffer_ms` of at most `S_solo`
/// (pack_hold_slack_us())
constexpr int64_t PACK_HOLD_SLACK_US =
    (TARGET_CHUNK_CELLS * CELL_US) + SOLO_SLACK_US + timing_defaults::PUMP_MARGIN_US;

/// @brief The send path's tunable timing values
struct SchedTiming {
    /// `M_pump`: margin added to the group lead
    int64_t pump_margin_us{timing_defaults::PUMP_MARGIN_US};
    /// `J_min`: the least startup lead, and a floor inside the group lead
    int64_t join_min_us{timing_defaults::JOIN_MIN_US};
    /// `G_default`: the group lead with no eligible player
    int64_t group_default_us{timing_defaults::GROUP_DEFAULT_US};
    /// `W_min` and `W_max`: the clamp on a buffered input's write-ahead
    int64_t write_ahead_min_us{timing_defaults::WRITE_AHEAD_MIN_US};
    int64_t write_ahead_max_us{timing_defaults::WRITE_AHEAD_MAX_US};
    /// `W_live`: a live input's write-ahead
    int64_t write_ahead_live_us{timing_defaults::WRITE_AHEAD_LIVE_US};
    /// `T_clamp`: the clamp on `min_buffer_ms` and `required_lead_time_ms`
    int64_t timing_clamp_us{timing_defaults::TIMING_CLAMP_US};
    /// `A_age`: the wait after which one artwork message goes ahead of audio
    int64_t artwork_age_us{timing_defaults::ARTWORK_AGE_US};
};

}  // namespace sendspin::server::sched
