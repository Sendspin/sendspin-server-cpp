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

/// @file timeline.h
/// @brief An epoch's anchor and the timestamps, positions and shifts derived from it, the stall
/// rebase decision and the pause position. Pure functions.

#pragma once

#include "sched/constants.h"
#include "sched/types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sendspin::server::sched {

/// @brief Integer division rounding toward negative infinity; d positive
constexpr int64_t floor_div(int64_t n, int64_t d) {
    return n / d - ((n % d != 0 && n < 0) ? 1 : 0);
}

/// @brief Integer division rounding toward positive infinity; d positive
constexpr int64_t ceil_div(int64_t n, int64_t d) {
    return -floor_div(-n, d);
}

/// @brief The smallest multiple of `multiple` (positive) at or above n
constexpr int64_t round_up(int64_t n, int64_t multiple) {
    return ceil_div(n, multiple) * multiple;
}

/// @brief A stretch of an epoch's input with one rate and channel count, from `position` on
struct Segment {
    uint64_t position{0};
    /// `u_j`: the unshifted time of `position`
    int64_t unshifted_us{0};
    uint32_t rate{0};
    uint8_t channels{0};
    /// The time from `T0` to `position` in picoseconds, floored at each segment: the boundary
    /// time output indices derive from, which `u_j`'s whole microseconds would drift by up to
    /// one per segment
    int64_t elapsed_ps{0};
};

/// @brief A forward move of the timeline by delta_us (positive) for every index at or above index
struct Shift {
    int64_t index{0};
    int64_t delta_us{0};
};

/// @brief One epoch's mapping from input positions and cells to server time
struct Anchor {
    /// `T0`: the server time of cell 0 before shifts
    int64_t t0_us{0};
    /// In position order; the first starts at the epoch's first position `p0`
    std::vector<Segment> segments;
    /// `S_base`: the shifts folded below every live index
    int64_t shift_base_us{0};
    /// In index order
    std::vector<Shift> shifts;
};

/// @brief An anchor with one segment: `p0` at rate and channels, its unshifted time t0_us
Anchor make_anchor(int64_t t0_us, uint64_t p0, uint32_t rate, uint8_t channels);

/// @brief make_anchor() into anchor, reusing its storage
void reset_anchor(Anchor& anchor, int64_t t0_us, uint64_t p0, uint32_t rate, uint8_t channels);

/// @brief Starts a new segment at position (at or after the last segment's start)
void add_segment(Anchor& anchor, uint64_t position, uint32_t rate, uint8_t channels);

/// @brief Records a shift at index, at or above every earlier shift's index
void add_shift(Anchor& anchor, int64_t index, int64_t delta_us);

/// @brief `u(p)`: a position's time before shifts; a position below `p0` extrapolates the first
/// segment
int64_t unshifted_time(const Anchor& anchor, uint64_t position);

/// @brief `cell(u)`: the cell holding an unshifted time
int64_t cell_at(const Anchor& anchor, int64_t unshifted_us);

/// @brief `S(k)`: the total shift applied to index k
int64_t shift_at(const Anchor& anchor, int64_t index);

/// @brief `ts(k) = T0 + k * F + S(k)`: the timestamp of the frame starting at cell k
int64_t frame_timestamp(const Anchor& anchor, int64_t index);

/// @brief The smallest index k at or above 0, a multiple of cells, with `ts(k) >= ts_us`
int64_t first_index_at_or_after(const Anchor& anchor, int64_t ts_us, uint32_t cells);

/// @brief `boundary(k)`: the first position whose unshifted time is at or after cell k's start, k
/// at or above 0
uint64_t cell_position(const Anchor& anchor, int64_t index);

/// @brief The first position whose unshifted time is at or after unshifted_us: `boundary` of a
/// time, which a visualizer instant's window ends at
uint64_t time_position(const Anchor& anchor, int64_t unshifted_us);

/// @brief The smallest index at or above 0, a multiple of cells, whose cell begins at or after
/// position: the first frame holding nothing before it
int64_t first_index_at_or_after_position(const Anchor& anchor, uint64_t position, uint32_t cells);

/// @brief `n_j = round((u_j - T0) * rate / 10^6)`: the output samples at rate since the epoch's
/// start at segment j's first position, from the boundary time to the picosecond, so rounding never
/// accumulates over segments
int64_t segment_output_index(const Anchor& anchor, size_t segment, uint32_t rate);

/// @brief The first index at or above requested, a multiple of cells, at or past every shift index
/// that cells does not divide, so no frame of an instance started there straddles a shift
int64_t instance_start(const Anchor& anchor, int64_t requested, uint32_t cells);

/// @brief `last_ts(m) = ts(m * floor(last_cell / m))`: where a format with frame length `m` has its
/// last frame in a drained epoch whose last input sample is in last_cell
int64_t last_frame_ts(const Anchor& anchor, int64_t last_cell, uint32_t cells);

/// @brief `time_of(p) = u(p) + S(cell(u(p)))`: when a position is played
int64_t time_of(const Anchor& anchor, uint64_t position);

/// @brief Folds the shifts below lowest_live_index into `S_base`, in place and with no allocation;
/// every timestamp at or above lowest_live_index is unchanged, and every one below it stays below
/// the timestamp of lowest_live_index
void fold_shifts(Anchor& anchor, int64_t lowest_live_index);

/// @brief Where a host view folds its shifts: the cell holding the first position its staging ring
/// keeps, below which no frame is still to be produced and no position is staged
int64_t first_kept_cell(const Anchor& anchor, uint64_t first_kept);

/// @brief Where core folds its shifts: the last cell begun by now_us, or -1 when none has; every
/// time core maps to an index is at or after now
int64_t last_begun_cell(const Anchor& anchor, int64_t now_us);

/// @brief A frame instance's next unproduced index and its frame length
struct InstanceNext {
    int64_t next_index{0};
    uint32_t cells{1};
};

/// @brief `k*`: the largest next unproduced index over the frame instances and the visualizer
/// instances, rounded up to a multiple of every frame instance's length, so a shift never lands
/// inside a frame or on anything already produced or dispatched
/// @param visualizer_frontiers per visualizer instance, the cell after its last dispatched one
int64_t shift_index(std::span<const InstanceNext> frames,
                    std::span<const int64_t> visualizer_frontiers);

/// @brief What the stall rebase decides from
struct RebaseParams {
    InputKind kind{InputKind::BUFFERED};
    /// `od_max`: the largest output delay over the group's eligible members
    int64_t output_delay_max_us{0};
    /// `G_eff`
    int64_t group_lead_us{0};
    /// `L_p`, the largest over the group's frame instances; live inputs only
    int64_t pipeline_latency_us{0};
    /// `J_in`; live inputs only
    int64_t live_jitter_us{0};
    int64_t pump_margin_us{timing_defaults::PUMP_MARGIN_US};
};

/// @brief Whether frame `k*`, whose lead is `ts(k*) - now`, needs a shift: below `od_max + M_pump`
/// for a buffered input, below `G_eff - L_p - M_pump` for a live one
bool rebase_needed(int64_t next_lead_us, const RebaseParams& params);

/// @brief The shift that restores the lead: to `G_eff` for a buffered input, to `G_eff - L_p +
/// J_in` for a live one
int64_t rebase_delta(int64_t next_lead_us, const RebaseParams& params);

/// @brief `p_pause = clamp(pos(now), p0, head)`, `pos(t)` being the largest position played by t;
/// `p0` while the epoch is pending
uint64_t pause_position(const Anchor& anchor, uint64_t p0, uint64_t head, bool epoch_pending,
                        int64_t now_us);

}  // namespace sendspin::server::sched
