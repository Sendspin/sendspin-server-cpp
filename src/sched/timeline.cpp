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

#include "sched/timeline.h"

#include <algorithm>
#include <numeric>

namespace sendspin::server::sched {

namespace {

/// The segment holding position: the last one starting at or below it, else the first
const Segment& segment_for(const Anchor& anchor, uint64_t position) {
    auto it =
        std::upper_bound(anchor.segments.begin(), anchor.segments.end(), position,
                         [](uint64_t p, const Segment& segment) { return p < segment.position; });
    return it == anchor.segments.begin() ? anchor.segments.front() : *std::prev(it);
}

/// Picoseconds in a second, the unit of a segment's elapsed time
constexpr int64_t PS_PER_SECOND = 1000000000000;

/// `floor((p - p_j) * 10^6 / r_j)` for the given segment
int64_t offset_us(const Segment& segment, uint64_t position) {
    const int64_t samples = static_cast<int64_t>(position - segment.position);
    return floor_div(samples * US_PER_SECOND, segment.rate);
}

}  // namespace

Anchor make_anchor(int64_t t0_us, uint64_t p0, uint32_t rate, uint8_t channels) {
    Anchor anchor;
    anchor.t0_us = t0_us;
    anchor.segments.push_back({p0, t0_us, rate, channels});
    return anchor;
}

void add_segment(Anchor& anchor, uint64_t position, uint32_t rate, uint8_t channels) {
    const Segment& last = anchor.segments.back();
    const int64_t unshifted = last.unshifted_us + offset_us(last, position);
    // Whole seconds and the rest apart, so neither product overflows
    const auto samples = static_cast<int64_t>(position - last.position);
    const int64_t elapsed_ps = last.elapsed_ps + ((samples / last.rate) * PS_PER_SECOND) +
                               ((samples % last.rate) * PS_PER_SECOND / last.rate);
    anchor.segments.push_back({position, unshifted, rate, channels, elapsed_ps});
}

void add_shift(Anchor& anchor, int64_t index, int64_t delta_us) {
    anchor.shifts.push_back({index, delta_us});
}

int64_t unshifted_time(const Anchor& anchor, uint64_t position) {
    const Segment& segment = segment_for(anchor, position);
    return segment.unshifted_us + offset_us(segment, position);
}

int64_t cell_at(const Anchor& anchor, int64_t unshifted_us) {
    return floor_div(unshifted_us - anchor.t0_us, CELL_US);
}

int64_t shift_at(const Anchor& anchor, int64_t index) {
    int64_t total = anchor.shift_base_us;
    for (const Shift& shift : anchor.shifts) {
        if (shift.index > index) {
            break;
        }
        total += shift.delta_us;
    }
    return total;
}

int64_t frame_timestamp(const Anchor& anchor, int64_t index) {
    return anchor.t0_us + index * CELL_US + shift_at(anchor, index);
}

int64_t first_index_at_or_after(const Anchor& anchor, int64_t ts_us, uint32_t cells) {
    const int64_t m = cells;
    // ts(k) >= T0 + k * F + S_base, since every shift is forward, so this index qualifies and
    // bounds the search
    const int64_t bound =
        std::max<int64_t>(0, ceil_div(ts_us - anchor.t0_us - anchor.shift_base_us, CELL_US));
    int64_t low = 0;
    int64_t high = ceil_div(bound, m);
    while (low < high) {
        const int64_t mid = low + (high - low) / 2;
        if (frame_timestamp(anchor, mid * m) >= ts_us) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return low * m;
}

uint64_t cell_position(const Anchor& anchor, int64_t index) {
    return time_position(anchor, anchor.t0_us + (index * CELL_US));
}

uint64_t time_position(const Anchor& anchor, int64_t unshifted_us) {
    // The last segment starting at or before the time; unshifted times rise with the segments
    auto it = std::upper_bound(
        anchor.segments.begin(), anchor.segments.end(), unshifted_us,
        [](int64_t t, const Segment& segment) { return t < segment.unshifted_us; });
    const Segment& segment =
        it == anchor.segments.begin() ? anchor.segments.front() : *std::prev(it);
    // The smallest p with u_j + floor((p - p_j) * 10^6 / r_j) >= unshifted_us; a time before the
    // first segment extrapolates it
    const int64_t position =
        static_cast<int64_t>(segment.position) +
        ceil_div((unshifted_us - segment.unshifted_us) * int64_t{segment.rate}, US_PER_SECOND);
    return static_cast<uint64_t>(std::max<int64_t>(position, 0));
}

int64_t first_index_at_or_after_position(const Anchor& anchor, uint64_t position, uint32_t cells) {
    if (position == 0) {
        return 0;
    }
    // boundary(k) >= position exactly when the position before it lies in a cell below k
    const int64_t cell = cell_at(anchor, unshifted_time(anchor, position - 1)) + 1;
    return round_up(std::max<int64_t>(cell, 0), cells);
}

int64_t segment_output_index(const Anchor& anchor, size_t segment, uint32_t rate) {
    const int64_t elapsed = anchor.segments[segment].elapsed_ps;
    // Whole seconds and the rest apart, so neither product overflows; the rest rounds half up
    const int64_t rest = elapsed % PS_PER_SECOND;
    return ((elapsed / PS_PER_SECOND) * int64_t{rate}) +
           (((2 * rest * int64_t{rate}) + PS_PER_SECOND) / (2 * PS_PER_SECOND));
}

int64_t instance_start(const Anchor& anchor, int64_t requested, uint32_t cells) {
    int64_t start = requested;
    for (const Shift& shift : anchor.shifts) {
        if (shift.index % cells != 0) {
            start = std::max(start, shift.index);
        }
    }
    return round_up(start, cells);
}

int64_t last_frame_ts(const Anchor& anchor, int64_t last_cell, uint32_t cells) {
    return frame_timestamp(anchor, floor_div(last_cell, cells) * cells);
}

int64_t time_of(const Anchor& anchor, uint64_t position) {
    const int64_t u = unshifted_time(anchor, position);
    return u + shift_at(anchor, cell_at(anchor, u));
}

void fold_shifts(Anchor& anchor, int64_t lowest_live_index) {
    // Shifts are in index order: the folded ones are a prefix
    size_t folded = 0;
    while (folded < anchor.shifts.size() && anchor.shifts[folded].index < lowest_live_index) {
        anchor.shift_base_us += anchor.shifts[folded].delta_us;
        ++folded;
    }
    anchor.shifts.erase(anchor.shifts.begin(),
                        anchor.shifts.begin() + static_cast<std::ptrdiff_t>(folded));
}

int64_t first_kept_cell(const Anchor& anchor, uint64_t first_kept) {
    return cell_at(anchor, unshifted_time(anchor, first_kept));
}

int64_t last_begun_cell(const Anchor& anchor, int64_t now_us) {
    return first_index_at_or_after(anchor, now_us + 1, 1) - 1;
}

int64_t shift_index(std::span<const InstanceNext> frames,
                    std::span<const int64_t> visualizer_frontiers) {
    int64_t next = 0;
    int64_t multiple = 1;
    for (const InstanceNext& instance : frames) {
        next = std::max(next, instance.next_index);
        multiple = std::lcm(multiple, int64_t{instance.cells});
    }
    for (const int64_t frontier : visualizer_frontiers) {
        next = std::max(next, frontier);
    }
    return round_up(next, multiple);
}

bool rebase_needed(int64_t next_lead_us, const RebaseParams& params) {
    const int64_t threshold =
        params.kind == InputKind::BUFFERED
            ? params.output_delay_max_us + params.pump_margin_us
            : params.group_lead_us - params.pipeline_latency_us - params.pump_margin_us;
    return next_lead_us < threshold;
}

int64_t rebase_delta(int64_t next_lead_us, const RebaseParams& params) {
    const int64_t restored =
        params.kind == InputKind::BUFFERED
            ? params.group_lead_us
            : params.group_lead_us - params.pipeline_latency_us + params.live_jitter_us;
    return restored - next_lead_us;
}

uint64_t pause_position(const Anchor& anchor, uint64_t p0, uint64_t head, bool epoch_pending,
                        int64_t now_us) {
    if (epoch_pending || head <= p0 || time_of(anchor, p0) > now_us) {
        return p0;
    }
    // time_of never decreases with the position: the largest position in [p0, head] played by now
    uint64_t low = p0;
    uint64_t high = head;
    while (low < high) {
        const uint64_t mid = low + (high - low + 1) / 2;
        if (time_of(anchor, mid) <= now_us) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return low;
}

}  // namespace sendspin::server::sched
