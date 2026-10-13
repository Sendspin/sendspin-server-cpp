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

/// @file test_sched_timeline.cpp
/// @brief Anchor timestamps, positions and shifts, the stall rebase and the pause position

#include "sched/timeline.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using sendspin::server::sched::Anchor;
using sendspin::server::sched::InputKind;
using sendspin::server::sched::InstanceNext;
using sendspin::server::sched::RebaseParams;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;

/// Epoch 2: T0 2610 ms, 44.1 kHz stereo from position 0
Anchor epoch2() {
    return sched::make_anchor(2610 * MS, 0, 44100, 2);
}

/// Three segments from T0 0: 44.1 kHz to position 100,000, 48 kHz to 150,000, then 11,025 Hz.
/// The second starts at 2,267,573 us, inside cell 113, and the third at 3,309,239 us, inside
/// cell 165 (later segments' cell boundaries fall between samples)
Anchor three_segments() {
    Anchor anchor = sched::make_anchor(0, 0, 44100, 2);
    sched::add_segment(anchor, 100000, 48000, 2);
    sched::add_segment(anchor, 150000, 11025, 1);
    return anchor;
}

/// T0 140 ms, 48 kHz, shifted by 80 ms at k 250
Anchor live_shifted() {
    Anchor anchor = sched::make_anchor(140 * MS, 0, 48000, 2);
    sched::add_shift(anchor, 250, 80 * MS);
    return anchor;
}

TEST(SchedTimeline, FrameTimestampAppliesShiftsFromTheirIndex) {
    Anchor anchor = sched::make_anchor(560 * MS, 0, 44100, 2);
    sched::add_shift(anchor, 10, 5 * MS);
    struct Row {
        const char* name;
        int64_t index;
        int64_t ts;
    };
    const Row rows[] = {
        {"Control: no shift yet", 0, 560 * MS},
        {"before the shift", 9, 740 * MS},
        {"at the shift", 10, 765 * MS},
        {"after the shift", 11, 785 * MS},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::frame_timestamp(anchor, row.index), row.ts);
    }
    // Control: k 250 after the shift
    EXPECT_EQ(sched::frame_timestamp(live_shifted(), 250), 5220 * MS);
}

TEST(SchedTimeline, FirstIndexAtOrAfter) {
    struct Row {
        const char* name;
        Anchor anchor;
        int64_t ts;
        uint32_t cells;
        int64_t index;
    };
    const Row rows[] = {
        {"exactly on a boundary", epoch2(), 2650 * MS, 1, 2},
        {"one microsecond after a boundary", epoch2(), 2650 * MS + 1, 1, 3},
        {"before T0 gives the first frame", epoch2(), 1000 * MS, 1, 0},
        {"m = 2 on an odd cell's timestamp gives the next even index", epoch2(), 2670 * MS, 2, 4},
        {"m = 2 on an even cell's timestamp gives it", epoch2(), 2690 * MS, 2, 4},
        // A join stamped 5000.100 with lead 150
        {"Control: a join", epoch2(), 5150100, 1, 128},
        // B's clear delayed to 2400, min_ts 2650
        {"a delayed clear", epoch2(), 2650 * MS, 1, 2},
        // E's min_ts 5190.100 lies inside the shift's gap
        {"inside a shift's gap", live_shifted(), 5190100, 1, 250},
        {"the same without the shift", sched::make_anchor(140 * MS, 0, 48000, 2), 5190100, 1, 253},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::first_index_at_or_after(row.anchor, row.ts, row.cells), row.index);
    }
}

TEST(SchedTimeline, TimeOfAcrossSegmentsAndShifts) {
    // The pause at 9000 ms is position 281,799
    EXPECT_EQ(sched::time_of(epoch2(), 281799), 9000 * MS);

    Anchor segments = sched::make_anchor(0, 0, 44100, 2);
    sched::add_segment(segments, 44100, 48000, 2);
    EXPECT_EQ(sched::unshifted_time(segments, 44100), 1000 * MS);
    EXPECT_EQ(sched::time_of(segments, 44100 + 24), 1000 * MS + 500);
    EXPECT_EQ(sched::time_of(segments, 44100 + 48000), 2000 * MS);
    // Control: the first segment
    EXPECT_EQ(sched::time_of(segments, 22050), 500 * MS);
    // Each segment starts where the previous one ends, at the previous rate
    sched::add_segment(segments, 44100 + 48000, 96000, 2);
    sched::add_segment(segments, 44100 + 48000 + 96000, 11025, 1);
    EXPECT_EQ(sched::time_of(segments, 44100 + 48000 + 96000), 3000 * MS);
    EXPECT_EQ(sched::time_of(segments, 44100 + 48000 + 96000 + 11025), 4000 * MS);

    // A shift at cell 50 moves positions from 1 s on
    Anchor shifted = sched::make_anchor(0, 0, 44100, 2);
    sched::add_shift(shifted, 50, 30 * MS);
    EXPECT_EQ(sched::time_of(shifted, 44100), 1030 * MS);
    EXPECT_EQ(sched::time_of(shifted, 44099), 999977);
}

// Cell k begins at the first position whose unshifted time is at or after T0 + k * F, in whichever
// segment holds that time
TEST(SchedTimeline, CellPositionAcrossSegments) {
    struct Row {
        const char* name;
        int64_t index;
        uint64_t position;
    };
    const Row rows[] = {
        {"Control: cell 0 is p0", 0, 0},
        {"one 44.1 kHz cell", 1, 882},
        {"a second", 50, 44100},
        {"the last cell starting in the first segment", 113, 99666},
        // ceil((2,280,000 - 2,267,573) * 48,000 / 10^6) = ceil(596.496)
        {"the first in the second, between two of its samples", 114, 100597},
        // ceil((3,320,000 - 3,309,239) * 11,025 / 10^6) = ceil(118.64)
        {"the first in the third", 166, 150119},
    };
    const Anchor anchor = three_segments();
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const uint64_t position = sched::cell_position(anchor, row.index);
        EXPECT_EQ(position, row.position);
        // The smallest such position
        const int64_t start = row.index * sched::CELL_US;
        EXPECT_GE(sched::unshifted_time(anchor, position), start);
        if (position > 0) {
            EXPECT_LT(sched::unshifted_time(anchor, position - 1), start);
        }
    }
}

// The first frame of length m (cells) holding nothing before a position, as an instance starting at
// a segment or at what the staging ring still holds needs
TEST(SchedTimeline, FirstIndexAtOrAfterPosition) {
    struct Row {
        const char* name;
        uint64_t position;
        uint32_t cells;
        int64_t index;
    };
    const Row rows[] = {
        {"Control: p0", 0, 1, 0},
        {"exactly a cell's position", 882, 1, 1},
        {"one sample after it", 883, 1, 2},
        {"a segment starting inside cell 113", 100000, 1, 114},
        {"the same, two-cell frames", 100000, 2, 114},
        {"the same, five-cell frames", 100000, 5, 115},
        {"exactly cell 114's position", 100597, 1, 114},
        {"one sample after it", 100598, 1, 115},
    };
    const Anchor anchor = three_segments();
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::first_index_at_or_after_position(anchor, row.position, row.cells),
                  row.index);
    }
    // Before a resume's p0, the first segment extrapolated back: cell 0
    EXPECT_EQ(
        sched::first_index_at_or_after_position(sched::make_anchor(0, 5000, 48000, 2), 100, 2), 0);
}

// n_j = round((u_j - T0) * r / 10^6), from the boundary's exact time, so rounding never accumulates
TEST(SchedTimeline, SegmentOutputIndexFromTheBoundaryTime) {
    struct Row {
        const char* name;
        size_t segment;
        uint32_t rate;
        int64_t index;
    };
    const Row rows[] = {
        {"Control: the first segment starts the output", 0, 48000, 0},
        // 100,000 / 44,100 s at 48 kHz is 108,843.537
        {"the second at 48 kHz rounds up", 1, 48000, 108844},
        // ...and at 44.1 kHz its own input's count, where u_1's whole microseconds give 99,999.97
        {"the second at the first's rate", 1, 44100, 100000},
        {"the second at 11,025 Hz", 1, 11025, 25000},
        // 50,000 / 48,000 s more: 25,000 + 11,484.375 at 11,025 Hz
        {"the third at 11,025 Hz rounds down", 2, 11025, 36484},
        {"the third at 48 kHz", 2, 48000, 158844},
    };
    const Anchor anchor = three_segments();
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::segment_output_index(anchor, row.segment, row.rate), row.index);
    }
    // Four segments of 1,001 samples alternating 44.1 and 48 kHz: at 48 kHz the fifth starts at
    // round(2 * 1,089.524 + 2 * 1,001) = round(4,181.048) = 4,181, where adding each segment's own
    // rounded count, 1,090 + 1,001 + 1,090 + 1,001, gives 4,182
    Anchor alternating = sched::make_anchor(0, 0, 44100, 2);
    for (uint64_t i = 1; i <= 4; ++i) {
        sched::add_segment(alternating, i * 1001, i % 2 == 1 ? 48000 : 44100, 2);
    }
    EXPECT_EQ(sched::segment_output_index(alternating, 4, 48000), 4181);
}

// An instance starts on its own frame boundary, and past a shift whose index its frame length does
// not divide, so none of its frames straddles one
TEST(SchedTimeline, InstanceStartPastAStraddledShift) {
    Anchor odd = sched::make_anchor(0, 0, 11025, 2);
    sched::add_shift(odd, 7, 30 * MS);
    Anchor even = sched::make_anchor(0, 0, 11025, 2);
    sched::add_shift(even, 10, 30 * MS);
    struct Row {
        const char* name;
        const Anchor& anchor;
        int64_t requested;
        uint32_t cells;
        int64_t start;
    };
    const Row rows[] = {
        {"Control: one-cell frames start where requested", odd, 4, 1, 4},
        {"an odd request rounds up to two-cell frames", even, 5, 2, 6},
        {"an even request stays", even, 4, 2, 4},
        {"a shift at an odd index lifts an m = 2 start past it", odd, 4, 2, 8},
        {"a request past that shift only rounds up", odd, 9, 2, 10},
        {"a shift at an even index divides into m = 2", even, 4, 2, 4},
        {"a shift at 10 divides into m = 5", even, 3, 5, 5},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::instance_start(row.anchor, row.requested, row.cells), row.start);
    }
}

TEST(SchedTimeline, FoldShiftsKeepsLiveTimestamps) {
    Anchor anchor = sched::make_anchor(0, 0, 48000, 2);
    sched::add_shift(anchor, 10, 5 * MS);
    sched::add_shift(anchor, 20, 7 * MS);
    const auto folded = [](Anchor copy, int64_t lowest) {
        sched::fold_shifts(copy, lowest);
        return copy;
    };

    const Anchor below = folded(anchor, 20);
    EXPECT_EQ(below.shift_base_us, 5 * MS);
    ASSERT_EQ(below.shifts.size(), 1U);
    EXPECT_EQ(below.shifts[0].index, 20);
    for (int64_t k = 20; k <= 25; ++k) {
        EXPECT_EQ(sched::frame_timestamp(below, k), sched::frame_timestamp(anchor, k)) << k;
    }
    // An index below the lowest live one keeps a timestamp below the lowest's, so a search for a
    // time at or past it never lands there
    for (int64_t k = 0; k < 20; ++k) {
        EXPECT_LT(sched::frame_timestamp(below, k), sched::frame_timestamp(anchor, 20)) << k;
    }
    // A shift at the lowest live index stays in the list
    const Anchor at = folded(anchor, 10);
    EXPECT_EQ(at.shift_base_us, 0);
    EXPECT_EQ(at.shifts.size(), 2U);
    // Both below it fold
    const Anchor all = folded(anchor, 21);
    EXPECT_EQ(all.shift_base_us, 12 * MS);
    EXPECT_TRUE(all.shifts.empty());
    // Control: no shifts
    const Anchor none = folded(sched::make_anchor(0, 0, 48000, 2), 100);
    EXPECT_EQ(none.shift_base_us, 0);
    EXPECT_TRUE(none.shifts.empty());
}

// Where the host view and core fold: the cell holding the first position the staging ring keeps,
// and the last cell begun by now, which a shift moves
TEST(SchedTimeline, FoldPoints) {
    Anchor anchor = sched::make_anchor(1000 * MS, 0, 48000, 2);
    sched::add_shift(anchor, 10, 5 * MS);
    EXPECT_EQ(sched::first_kept_cell(anchor, 0), 0);
    EXPECT_EQ(sched::first_kept_cell(anchor, 959), 0);
    EXPECT_EQ(sched::first_kept_cell(anchor, 960), 1);
    EXPECT_EQ(sched::last_begun_cell(anchor, 1000 * MS - 1), -1);
    EXPECT_EQ(sched::last_begun_cell(anchor, 1000 * MS), 0);
    EXPECT_EQ(sched::last_begun_cell(anchor, 1020 * MS - 1), 0);
    // Cell 10 begins at 1205 ms, not 1200
    EXPECT_EQ(sched::last_begun_cell(anchor, 1205 * MS - 1), 9);
    // Control: on a cell's start
    EXPECT_EQ(sched::last_begun_cell(anchor, 1205 * MS), 10);
}

TEST(SchedTimeline, ShiftIndex) {
    struct Row {
        const char* name;
        std::vector<InstanceNext> frames;
        std::vector<int64_t> frontiers;
        int64_t index;
    };
    const Row rows[] = {
        {"Control: one instance gives its next index", {{7, 1}}, {}, 7},
        {"two instances at different next indices", {{12, 1}, {15, 1}}, {}, 15},
        {"a visualizer frontier above every frame instance", {{12, 1}}, {20}, 20},
        {"an odd k* with an m = 2 instance rounds up", {{13, 1}, {12, 2}}, {}, 14},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::shift_index(row.frames, row.frontiers), row.index);
    }
}

// Each threshold at minus one, at and plus one
TEST(SchedTimeline, RebaseThresholdsAndDelta) {
    RebaseParams buffered;
    buffered.kind = InputKind::BUFFERED;
    buffered.output_delay_max_us = 50 * MS;
    buffered.group_lead_us = 560 * MS;
    buffered.pump_margin_us = 10 * MS;

    // A live group after E joined
    RebaseParams live;
    live.kind = InputKind::LIVE;
    live.group_lead_us = 230 * MS;
    live.pipeline_latency_us = 30 * MS;
    live.live_jitter_us = 10 * MS;
    live.pump_margin_us = 10 * MS;

    struct Row {
        const char* name;
        const RebaseParams& params;
        int64_t lead;
        bool needed;
    };
    const Row rows[] = {
        {"buffered below od_max + M_pump", buffered, 60 * MS - 1, true},
        {"buffered at the threshold", buffered, 60 * MS, false},
        {"buffered above", buffered, 60 * MS + 1, false},
        {"live below G_eff - L_p - M_pump", live, 190 * MS - 1, true},
        {"live at the threshold", live, 190 * MS, false},
        {"live above", live, 190 * MS + 1, false},
        {"live on-time frame late by J_in after a shift", live, 200 * MS, false},
        {"Control: no stall", buffered, 500 * MS, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::rebase_needed(row.lead, row.params), row.needed);
    }
    // Buffered restores the full group lead
    EXPECT_EQ(sched::rebase_delta(30 * MS, buffered), 530 * MS);
    // A lead of 130 shifts by 80 to the on-time lead G_eff - L_p + J_in = 210
    EXPECT_EQ(sched::rebase_delta(130 * MS, live), 80 * MS);
}

TEST(SchedTimeline, PausePosition) {
    Anchor shifted = epoch2();
    sched::add_shift(shifted, 100, 100 * MS);
    Anchor segments = sched::make_anchor(0, 0, 44100, 2);
    sched::add_segment(segments, 44100, 48000, 2);
    struct Row {
        const char* name;
        Anchor anchor;
        uint64_t head;
        bool pending;
        int64_t now;
        uint64_t position;
    };
    const Row rows[] = {
        {"Control: a pause at 9000 ms", epoch2(), 350595, false, 9000 * MS, 281799},
        {"before T0 clamps to p0", epoch2(), 350595, false, 2000 * MS, 0},
        {"past the head clamps to the head", epoch2(), 100000, false, 9000 * MS, 100000},
        {"a pending epoch returns p0", epoch2(), 350595, true, 9000 * MS, 0},
        {"with a shift", shifted, 350595, false, 9000 * MS, 277389},
        {"in a second segment", segments, 200000, false, 1500 * MS, 68100},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::pause_position(row.anchor, 0, row.head, row.pending, row.now),
                  row.position);
    }
}

// A format with frame length m has its last frame at ts(m * floor(c / m))
TEST(SchedTimeline, LastFrameTimestamp) {
    // Epoch 3, T0 12560, last input sample in cell 453
    const Anchor epoch3 = sched::make_anchor(12560 * MS, 281799, 44100, 2);
    EXPECT_EQ(sched::last_frame_ts(epoch3, 453, 1), 21620 * MS);
    EXPECT_EQ(sched::last_frame_ts(epoch3, 453, 2), 21600 * MS);
    EXPECT_EQ(sched::last_frame_ts(epoch3, 452, 2), 21600 * MS);
}

}  // namespace
