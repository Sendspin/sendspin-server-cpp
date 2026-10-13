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

/// @file test_sched_property.cpp
/// @brief Seeded property tests: a simulated clock drives a frame ring, prepare_cursor(),
/// item_ready(), select_next(), pack_chunk() and the Ledger with random frame sizes and lengths,
/// output delay changes, competing time replies and write durations, and checks each chunk's
/// shape, that no frame is late for the player and the capacity at every chunk's write start; the
/// capacity check counts against a brute-force replay of the spec's accounting rule, not against
/// the Ledger. Random anchors of several input segments and shifts check the frame grid's positions
/// and output indices against their definitions by search. Random live inputs, late, stalling,
/// from slow clocks and with rising group leads, check the live rebase, admission as if rebased
/// and the folding of shifts.

#include "sched/admission.h"
#include "sched/chunk.h"
#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/leads.h"
#include "sched/ledger.h"
#include "sched/lowering.h"
#include "sched/selection.h"
#include "sched/timeline.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

namespace {

using sendspin::server::sched::AdmissionInput;
using sendspin::server::sched::Choice;
using sendspin::server::sched::Codec;
using sendspin::server::sched::Cursor;
using sendspin::server::sched::CursorView;
using sendspin::server::sched::EpochId;
using sendspin::server::sched::Frame;
using sendspin::server::sched::FrameRing;
using sendspin::server::sched::FrameView;
using sendspin::server::sched::InputKind;
using sendspin::server::sched::InstanceId;
using sendspin::server::sched::ItemKind;
using sendspin::server::sched::ItemState;
using sendspin::server::sched::Ledger;
using sendspin::server::sched::PackLimits;
using sendspin::server::sched::PumpSnapshot;
using sendspin::server::sched::RunState;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr EpochId EPOCH{1};
constexpr int64_t T0 = 500 * MS;
/// Frames exist this far ahead of now, as for a buffered input with G + W of 1.56 s
constexpr int64_t PRODUCED_AHEAD = 1560 * MS;
constexpr int64_t MESSAGE_CAP = 16384;
constexpr int SEEDS = 40;
constexpr int STEPS = 3000;

struct SentChunk {
    int64_t ts;
    int64_t duration;
    int64_t bytes;
};

/// The spec's count: every chunk since the reset whose completion is in the future (spec
/// player/v1.md "Player Buffer Accounting")
int64_t replay_count(const std::vector<SentChunk>& sent, int64_t od, int64_t now) {
    int64_t count = 0;
    for (const SentChunk& chunk : sent) {
        if (chunk.ts + chunk.duration - od > now) {
            count += chunk.bytes;
        }
    }
    return count;
}

void run_seed(uint64_t seed) {
    std::mt19937_64 rng(seed);
    // mt19937_64's output is fixed by the standard; std::uniform_int_distribution's mapping is
    // not, so this mapping keeps a seed's run the same on every standard library
    auto uniform = [&rng](int64_t low, int64_t high) {
        return low + static_cast<int64_t>(rng() % static_cast<uint64_t>(high - low + 1));
    };
    // Frames of one cell, of two (11,025 Hz) and of five (8,010 Hz)
    const int64_t shape = uniform(0, 5);
    const uint32_t cells = shape == 0 ? 2 : shape == 1 ? 5 : 1;
    const int64_t capacity = uniform(8000, 200000);
    const int64_t max_frame = std::min<int64_t>(4000, capacity - sched::CHUNK_HEADER_BYTES);

    FrameRing ring(InstanceId{1}, cells);
    ring.restart(EPOCH, 0);
    Ledger ledger(capacity, true);
    std::vector<SentChunk> sent;
    Cursor cursor;
    int64_t od = uniform(0, 200) * MS;
    ledger.set_output_delay_us(od);
    int64_t now = 0;
    int64_t produced = 0;
    int chunks = 0;

    for (int step = 0; step < STEPS; ++step) {
        if (uniform(0, 49) == 0) {
            od = uniform(0, 300) * MS;
            ledger.set_output_delay_us(od);
        }
        while (T0 + produced * sched::CELL_US <= now + PRODUCED_AHEAD) {
            Frame frame;
            frame.epoch = EPOCH;
            frame.index = produced;
            frame.ts_us = T0 + produced * sched::CELL_US;
            frame.duration_us = cells * sched::CELL_US;
            frame.payload.assign(static_cast<size_t>(uniform(1, max_frame)), 0);
            ASSERT_TRUE(ring.append(std::move(frame), now));
            produced += cells;
        }
        ledger.prune(now);
        sched::prepare_cursor(cursor, ring, EPOCH, od, now);

        const Frame* next = ring.at(EPOCH, cursor.next);
        CursorView view;
        view.phase_normal = true;
        view.stream_active = true;
        view.available = true;
        view.next = cursor.next;
        if (next != nullptr) {
            view.next_payload_bytes = static_cast<int64_t>(next->payload.size());
        }
        const ItemState item = sched::item_ready(view, ledger, now);
        PumpSnapshot snapshot;
        snapshot.time_pending = uniform(0, 9) == 0;
        snapshot.player = sched::audio_candidate(item, next != nullptr ? next->ts_us - od : 0);

        switch (sched::select_next(snapshot)) {
            case Choice::TIME:
                now += uniform(50, 500);
                break;
            case Choice::PLAYER: {
                ASSERT_EQ(item.kind, ItemKind::CHUNK);
                std::vector<FrameView> frames;
                for (int64_t k = cursor.next;; k += cells) {
                    const Frame* frame = ring.at(EPOCH, k);
                    if (frame == nullptr || frames.size() == 10) {
                        break;
                    }
                    frames.push_back(sched::view_of(*frame));
                }
                PackLimits limits;
                limits.cells = cells;
                limits.now_us = now;
                limits.output_delay_us = od;
                limits.message_cap = MESSAGE_CAP;
                limits.room_bytes = capacity - ledger.count(now);
                const size_t n = sched::pack_chunk(frames, limits);

                // Whole consecutive frames, at least one, at most 150 ms and n_target
                ASSERT_GE(n, 1U);
                ASSERT_LE(n, std::max<size_t>(1, sched::TARGET_CHUNK_CELLS / cells));
                int64_t bytes = sched::CHUNK_HEADER_BYTES;
                int64_t duration = 0;
                for (size_t i = 0; i < n; ++i) {
                    ASSERT_EQ(frames[i].index, frames[0].index + static_cast<int64_t>(i) * cells);
                    // No frame late for this player
                    ASSERT_GT(frames[i].ts_us - od, now);
                    bytes += frames[i].payload_bytes;
                    duration += frames[i].duration_us;
                }
                ASSERT_LE(duration, sched::MAX_CHUNK_US);
                ASSERT_LE(bytes, MESSAGE_CAP);
                // The spec's count plus this chunk within the capacity
                ASSERT_EQ(ledger.count(now), replay_count(sent, od, now));
                ASSERT_LE(replay_count(sent, od, now) + bytes, capacity);

                ledger.add(frames[0].ts_us + duration, bytes);
                sent.push_back({frames[0].ts_us, duration, bytes});
                cursor.next += static_cast<int64_t>(n) * cells;
                ++chunks;
                // A slow write now and then, long enough for frames to go late
                now += uniform(0, 19) == 0 ? uniform(100, 600) * MS : uniform(50, 5000);
                break;
            }
            case Choice::NONE:
                ASSERT_NE(item.kind, ItemKind::CHUNK);
                if (item.kind == ItemKind::BLOCKED) {
                    // The room time is the first instant the next chunk's first frame fits
                    const int64_t first = sched::CHUNK_HEADER_BYTES + *view.next_payload_bytes;
                    ASSERT_GT(item.room_time_us, now);
                    ASSERT_TRUE(ledger.admits(first, item.room_time_us));
                    ASSERT_FALSE(ledger.admits(first, item.room_time_us - 1));
                    now = item.room_time_us + uniform(0, 2000);
                } else {
                    now += sched::CELL_US;
                }
                break;
            default:
                FAIL() << "unexpected choice";
        }
    }
    // The run sent audio, so the checks above ran; a longer frame fills the ledger sooner
    EXPECT_GT(chunks, STEPS / (4 * static_cast<int>(cells)));
}

/// One instance a switching cursor reads: its ring and the next index it produces
struct SwitchInstance {
    FrameRing ring;
    uint32_t cells{1};
    int64_t produced{0};
};

/// One player's stream switching in place between instances: now and then its wanted format changes
/// to a new instance, which starts at the first frame boundary a start trigger could read (`J_min`
/// past now) and catches up a few frames at a time, to another instance, or back to the stream's
/// own; the cursor packs up to the switch point plan_switch() gives and moves to the target there;
/// slow writes make frames late
void run_switch_seed(uint64_t seed) {
    std::mt19937_64 rng(seed);
    auto uniform = [&rng](int64_t low, int64_t high) {
        return low + static_cast<int64_t>(rng() % static_cast<uint64_t>(high - low + 1));
    };
    // Frames of one cell, of two (11,025 Hz) and of five (8,010 Hz)
    auto random_cells = [&]() -> uint32_t {
        const int64_t shape = uniform(0, 3);
        return shape == 0 ? 2 : shape == 1 ? 5 : 1;
    };
    const int64_t od = uniform(0, 200) * MS;
    std::vector<SwitchInstance> instances;
    auto add_instance = [&](uint32_t cells, int64_t start) {
        SwitchInstance instance{FrameRing(InstanceId{instances.size() + 1}, cells), cells, start};
        instance.ring.restart(EPOCH, start);
        instances.push_back(std::move(instance));
        return instances.size() - 1;
    };
    size_t current = add_instance(random_cells(), 0);
    std::optional<size_t> target;
    std::optional<int64_t> planned;
    // A small capacity keeps the cursor near now, below a new target's first frame
    Ledger ledger(uniform(8000, 200000), true);
    ledger.set_output_delay_us(od);
    Cursor cursor;
    int64_t now = 0;
    // The index after the last frame sent; a chunk starts there unless frames went late
    int64_t sent_end = 0;
    bool skipped = false;
    int switches = 0;

    for (int step = 0; step < STEPS; ++step) {
        for (size_t i = 0; i < instances.size(); ++i) {
            SwitchInstance& instance = instances[i];
            // A target catches up a few frames at a time; the others are at the production edge
            int64_t budget = target == i ? uniform(1, 3) : std::numeric_limits<int64_t>::max();
            while (budget-- > 0 &&
                   T0 + instance.produced * sched::CELL_US <= now + PRODUCED_AHEAD) {
                Frame frame;
                frame.epoch = EPOCH;
                frame.index = instance.produced;
                frame.ts_us = T0 + instance.produced * sched::CELL_US;
                frame.duration_us = instance.cells * sched::CELL_US;
                frame.payload.assign(static_cast<size_t>(uniform(1, 3000)), 0);
                // A frame already late is dropped at insertion
                instance.ring.append(std::move(frame), now);
                instance.produced += instance.cells;
            }
        }
        if (uniform(0, 59) == 0) {
            const int64_t pick = uniform(0, 2);
            if (pick == 0 || instances.size() == 1) {
                // A new format: core starts its instance where a start trigger could read it,
                // and the pipelines may move it later (past a shift, or to history still held)
                const uint32_t cells = random_cells();
                const int64_t at = std::max<int64_t>(
                    0, sched::ceil_div(now + sched::timing_defaults::JOIN_MIN_US - T0,
                                       sched::CELL_US));
                target = add_instance(cells, sched::round_up(at + uniform(0, 30), cells));
            } else if (pick == 1) {
                target =
                    static_cast<size_t>(uniform(0, static_cast<int64_t>(instances.size()) - 1));
            } else {
                target = current;
            }
            // Rule 3: planned again from the cursor; back to the stream's own cancels it
            planned.reset();
            if (target == current) {
                target.reset();
            }
        }
        ledger.prune(now);
        const FrameRing& ring = instances[current].ring;
        const uint32_t cells = instances[current].cells;
        const int64_t before = cursor.next;
        sched::prepare_cursor(cursor, ring, EPOCH, od, now);
        if (cursor.next != before) {
            // Only frames late for the player are passed over
            ASSERT_LE(T0 + (cursor.next - cells) * sched::CELL_US - od, now);
            skipped = true;
        }
        CursorView view;
        view.phase_normal = true;
        view.stream_active = true;
        view.available = true;
        view.next = cursor.next;
        if (const Frame* next = ring.at(EPOCH, cursor.next)) {
            view.next_payload_bytes = static_cast<int64_t>(next->payload.size());
        }
        if (target) {
            const SwitchInstance& to = instances[*target];
            planned = sched::plan_switch(cursor.next, planned, to.ring.base(), cells, to.cells);
            // A frame boundary of both, at or past the cursor and the target's first frame
            ASSERT_EQ(*planned % std::lcm(int64_t{cells}, int64_t{to.cells}), 0);
            ASSERT_GE(*planned, cursor.next);
            ASSERT_GE(*planned, to.ring.base());
            view.switch_point = planned;
            view.target_holds_switch_point = to.ring.at(EPOCH, *planned) != nullptr;
        }
        const ItemState item = sched::item_ready(view, ledger, now);
        PumpSnapshot snapshot;
        snapshot.time_pending = uniform(0, 9) == 0;
        snapshot.player = sched::audio_candidate(item, 0);
        switch (sched::select_next(snapshot)) {
            case Choice::TIME:
                now += uniform(50, 500);
                break;
            case Choice::PLAYER: {
                if (item.kind == ItemKind::SWITCH_START) {
                    // The in-place stream/start between chunks; the target holds the frame after
                    // the last one sent, or a late one past it
                    ASSERT_EQ(cursor.next, *planned);
                    ASSERT_NE(instances[*target].ring.at(EPOCH, cursor.next), nullptr);
                    ASSERT_TRUE(cursor.next == sent_end || skipped) << cursor.next;
                    current = *target;
                    target.reset();
                    planned.reset();
                    ++switches;
                    break;
                }
                ASSERT_EQ(item.kind, ItemKind::CHUNK);
                std::vector<FrameView> frames;
                for (int64_t k = cursor.next; frames.size() < 10; k += cells) {
                    const Frame* frame = ring.at(EPOCH, k);
                    if (frame == nullptr) {
                        break;
                    }
                    frames.push_back(sched::view_of(*frame));
                }
                PackLimits limits;
                limits.cells = cells;
                limits.now_us = now;
                limits.output_delay_us = od;
                limits.message_cap = MESSAGE_CAP;
                limits.room_bytes = ledger.capacity() - ledger.count(now);
                limits.switch_point = planned;
                const size_t n = sched::pack_chunk(frames, limits);
                ASSERT_GE(n, 1U);
                // Nothing resent, no gap but late frames, nothing at or past the switch point
                ASSERT_TRUE(frames[0].index == sent_end || skipped) << frames[0].index;
                ASSERT_GE(frames[0].index, sent_end);
                int64_t duration = 0;
                for (size_t i = 0; i < n; ++i) {
                    ASSERT_EQ(frames[i].index, frames[0].index + static_cast<int64_t>(i) * cells);
                    ASSERT_GT(frames[i].ts_us - od, now);
                    if (planned) {
                        ASSERT_LT(frames[i].index, *planned);
                    }
                    duration += frames[i].duration_us;
                }
                ASSERT_LE(duration, sched::MAX_CHUNK_US);
                int64_t bytes = sched::CHUNK_HEADER_BYTES;
                for (size_t i = 0; i < n; ++i) {
                    bytes += frames[i].payload_bytes;
                }
                ledger.add(frames[0].ts_us + duration, bytes);
                cursor.next += static_cast<int64_t>(n) * cells;
                sent_end = cursor.next;
                skipped = false;
                now += uniform(0, 29) == 0 ? uniform(100, 600) * MS : uniform(50, 5000);
                break;
            }
            case Choice::NONE:
                now = item.kind == ItemKind::BLOCKED ? item.room_time_us + uniform(0, 2000)
                                                     : now + uniform(1000, sched::CELL_US);
                break;
            default:
                FAIL() << "unexpected choice";
        }
    }
    // The run switched, so the checks above ran
    EXPECT_GT(switches, 5);
}

/// A rate from those a segment or a format takes, frames of one, two and five cells among them
uint32_t random_rate(const std::function<int64_t(int64_t, int64_t)>& uniform) {
    constexpr std::array<uint32_t, 9> RATES = {8010,  11025, 22050, 32000, 44100,
                                               48000, 88200, 96000, 192000};
    return RATES[static_cast<size_t>(uniform(0, RATES.size() - 1))];
}

void run_grid_seed(uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::function<int64_t(int64_t, int64_t)> uniform = [&rng](int64_t low, int64_t high) {
        return low + static_cast<int64_t>(rng() % static_cast<uint64_t>(high - low + 1));
    };
    // A resume's p0 is not 0
    const auto p0 = static_cast<uint64_t>(uniform(0, 1) == 0 ? 0 : uniform(1, 500000));
    sched::Anchor anchor = sched::make_anchor(uniform(0, 1000000000), p0, random_rate(uniform), 2);
    uint64_t position = p0;
    // Up to the staged cap, past which whole microseconds would drift a sample
    const int64_t segments = uniform(0, 31);
    for (int64_t i = 0; i < segments; ++i) {
        position += static_cast<uint64_t>(uniform(1, 300000));
        sched::add_segment(anchor, position, random_rate(uniform), 1);
    }
    const uint64_t end = position + static_cast<uint64_t>(uniform(1, 300000));
    int64_t shift_at = 0;
    for (int64_t i = uniform(0, 3); i > 0; --i) {
        shift_at += uniform(1, 40);
        sched::add_shift(anchor, shift_at, uniform(1, 500) * MS);
    }
    const int64_t last_cell = sched::cell_at(anchor, sched::unshifted_time(anchor, end));

    for (int check = 0; check < 200; ++check) {
        // cell_position(): the smallest position whose unshifted time reaches the cell
        const int64_t k = uniform(0, last_cell);
        const uint64_t at = sched::cell_position(anchor, k);
        const int64_t cell_start = anchor.t0_us + (k * sched::CELL_US);
        ASSERT_GE(sched::unshifted_time(anchor, at), cell_start) << k;
        if (at > p0) {
            ASSERT_LT(sched::unshifted_time(anchor, at - 1), cell_start) << k;
        }

        // first_index_at_or_after_position(): the first frame boundary at or past a position
        const auto p =
            static_cast<uint64_t>(uniform(static_cast<int64_t>(p0), static_cast<int64_t>(end)));
        const uint32_t cells = sched::frame_layout(random_rate(uniform), sched::Codec::PCM).cells;
        const int64_t first = sched::first_index_at_or_after_position(anchor, p, cells);
        ASSERT_EQ(first % cells, 0);
        ASSERT_GE(sched::cell_position(anchor, first), p) << p;
        if (first >= cells) {
            ASSERT_LT(sched::cell_position(anchor, first - cells), p) << p;
        }

        // instance_start(): on its frame boundary, past every shift its frames would straddle
        const int64_t requested = uniform(0, 200);
        const int64_t start = sched::instance_start(anchor, requested, cells);
        ASSERT_EQ(start % cells, 0);
        ASSERT_GE(start, requested);
        bool straddled_below = false;
        for (const sched::Shift& shift : anchor.shifts) {
            if (shift.index % cells != 0) {
                ASSERT_LE(shift.index, start);
                straddled_below = straddled_below || shift.index > start - cells;
            }
        }
        // The smallest: the frame boundary below it is before the request or straddles a shift
        ASSERT_TRUE(start - cells < requested || straddled_below) << start;
    }

    // segment_output_index(): within half a sample of the boundary's exact time at the rate,
    // however many segments come before it, and never falling
    for (int check = 0; check < 20; ++check) {
        const uint32_t rate = random_rate(uniform);
        int64_t previous = -1;
        long double exact = 0;
        for (size_t j = 0; j < anchor.segments.size(); ++j) {
            if (j > 0) {
                const sched::Segment& before = anchor.segments[j - 1];
                exact += static_cast<long double>(anchor.segments[j].position - before.position) *
                         rate / before.rate;
            }
            const int64_t index = sched::segment_output_index(anchor, j, rate);
            ASSERT_LE(std::abs(static_cast<long double>(index) - exact), 0.5L + 1e-6L) << j;
            ASSERT_GE(index, previous);
            previous = index;
        }
    }
}

/// The blocks a live run writes: a minute of 10 ms blocks
constexpr int64_t LIVE_BLOCKS = 6000;

/// The frames the live runs cut to lower the latency: in runs whose source's clock runs fast, and
/// in the others where a member left
struct LiveCuts {
    uint64_t fast_clock{0};
    uint64_t left{0};
};

/// One live run: a source delivers blocks in real time, or from a clock running slow or fast, each
/// written late by up to the declared jitter and now and then after a stall, the blocks it held
/// back dropped; now and then a joiner raises the group lead, or one leaves and lowers it. Each
/// write is lowered as the host view lowers it (sched::lower_live_write()), cutting what the epoch
/// owes from its end and lowering `G_eff` with it. Each frame is produced as soon as its input is
/// staged (no resampler, so no lookahead), its rebase decided first. One anchor is folded below the
/// cell a cell behind the position playing, as the host view folds its own, one below the last cell
/// begun, as core folds its own, and one never.
/// @param cuts adds the frames this run cut
void run_live_seed(uint64_t seed, LiveCuts& cuts) {
    std::mt19937_64 rng(seed);
    auto uniform = [&rng](int64_t low, int64_t high) {
        return low + static_cast<int64_t>(rng() % static_cast<uint64_t>(high - low + 1));
    };
    // Frames of one cell at 48 kHz written in 10 ms blocks, or of two at 11,025 Hz written a frame
    // at a time
    const bool two_cells = uniform(0, 2) == 0;
    const uint32_t rate = two_cells ? 11025 : 48000;
    const uint32_t cells = sched::frame_layout(rate, Codec::PCM).cells;
    const uint64_t block = two_cells ? 441 : 480;
    const auto block_us = static_cast<int64_t>(block) * sched::US_PER_SECOND / rate;
    const uint64_t cell_samples = rate / sched::CELLS_PER_SECOND;
    const int64_t jitter = uniform(5, 30) * MS;
    const sched::SchedTiming timing;
    const int64_t latency = sched::pipeline_latency_us({cells, 0, rate}, InputKind::LIVE, jitter);
    const int64_t lead = uniform(0, 300) * MS;
    const sched::LeadInput member{{lead, lead}, latency};
    sched::RebaseParams params;
    params.kind = InputKind::LIVE;
    params.group_lead_us = sched::group_lead_us({&member, 1}, timing);
    params.pipeline_latency_us = latency;
    params.live_jitter_us = jitter;
    params.pump_margin_us = timing.pump_margin_us;
    // A third of the sources' clocks run slow and a third fast, by up to 0.2 %
    const int64_t clock = uniform(0, 2);
    const int64_t ppm = clock == 0 ? 0 : clock == 1 ? uniform(50, 2000) : -uniform(50, 2000);
    // `G_next`, which the lowering measures against; params.group_lead_us is `G_eff`
    int64_t lead_next = params.group_lead_us;
    const int64_t lead_least = lead_next;
    sched::LiveLowering lowering;

    sched::Anchor full;
    sched::Anchor folded;
    sched::Anchor core;
    int64_t written_at = 0;
    uint64_t head = 0;
    int64_t next = 0;
    std::optional<int64_t> last_ts;
    int stalls = 0;
    int raises = 0;
    int leaves = 0;
    int shifts = 0;
    uint64_t cut = 0;
    for (int64_t i = 0; i < LIVE_BLOCKS; ++i) {
        const int64_t source_at = MS + (i * block_us) + (i * block_us * ppm / sched::US_PER_SECOND);
        int64_t late = uniform(0, jitter);
        if (uniform(0, 299) == 0) {
            late += uniform(1, 600) * MS;
            ++stalls;
        }
        // The blocks a stall held back: written at once they would run ahead of real time past
        // W_live, which admission cuts, so the host drops them
        if (source_at + jitter < written_at) {
            continue;
        }
        const int64_t now = std::max(written_at, source_at + late);
        written_at = now;
        if (i == 0) {
            // The write that begins the run anchors it
            full = sched::make_anchor(now + params.group_lead_us, 0, rate, 2);
            folded = full;
            core = full;
        } else if (uniform(0, 499) == 0) {
            // A joiner raises G_next, and with it G_eff within the epoch
            // (HostView::set_group_params())
            lead_next += uniform(10, 300) * MS;
            params.group_lead_us = std::max(params.group_lead_us, lead_next);
            ++raises;
        } else if (uniform(0, 499) == 0) {
            // A high-lead member leaves: G_next falls, G_eff stays until the lowering cuts
            lead_next = std::max(lead_least, lead_next - (uniform(10, 300) * MS));
            ++leaves;
        }
        AdmissionInput in;
        in.kind = InputKind::LIVE;
        in.run = i == 0 ? RunState::NONE : RunState::PLAYING;
        in.head = head;
        in.head_rate = rate;
        in.group_lead_us = params.group_lead_us;
        in.write_ahead_us = timing.write_ahead_live_us;
        in.ring_limit = std::numeric_limits<uint64_t>::max();
        in.shift_index = next;
        in.rebase = params;
        const uint64_t limit = sched::admission_limit(in, full, now);
        // A writer on time is never cut short
        ASSERT_GE(limit, head + block) << i;
        if (i > 0) {
            // The limit is the one the rebase that frame k* triggers now leaves, and what it admits
            // plays within the horizon
            sched::Anchor after = full;
            const int64_t next_lead = sched::frame_timestamp(full, next) - now;
            if (sched::rebase_needed(next_lead, params)) {
                sched::add_shift(after, next, sched::rebase_delta(next_lead, params));
            }
            ASSERT_EQ(sched::admission_limit(in, after, now), limit) << i;
            ASSERT_LE(sched::time_of(after, limit),
                      now + params.group_lead_us + timing.write_ahead_live_us)
                << i;
            // The folded anchor admits the same
            ASSERT_EQ(sched::admission_limit(in, folded, now), limit) << i;
        }
        uint64_t taken = block;
        if (i > 0) {
            const sched::LoweredWrite lowered =
                sched::lower_live_write(lowering, block, rate, sched::time_of(full, head), now,
                                        lead_next, params.group_lead_us);
            taken = lowered.taken;
            cut += block - taken;
            params.group_lead_us = lowered.group_lead_eff_us;
            // Every player keeps the lead it needs
            ASSERT_GE(params.group_lead_us, lead_next) << i;
        }
        head += taken;

        while (sched::cell_position(full, next + cells) <= head) {
            const int64_t frame_lead = sched::frame_timestamp(full, next) - now;
            if (sched::rebase_needed(frame_lead, params)) {
                const int64_t delta = sched::rebase_delta(frame_lead, params);
                ASSERT_GT(delta, 0);
                sched::add_shift(full, next, delta);
                sched::add_shift(folded, next, delta);
                sched::add_shift(core, next, delta);
                ++shifts;
                // The lead of a frame whose last input arrived on time
                ASSERT_EQ(sched::frame_timestamp(full, next) - now,
                          params.group_lead_us - latency + jitter);
            }
            const int64_t ts = sched::frame_timestamp(full, next);
            // player/v1.md min_buffer_ms: every frame leaves at least G_eff - L_p - M_pump ahead
            ASSERT_GE(ts - now, params.group_lead_us - latency - params.pump_margin_us) << next;
            // The timeline only moves forward: no frame overlaps the one before
            if (last_ts) {
                ASSERT_GE(ts, *last_ts + (int64_t{cells} * sched::CELL_US)) << next;
            }
            last_ts = ts;
            next += cells;
        }

        // Shifts below every live index fold into S_base, leaving every live timestamp as it was
        // and the list within the cells from the ring's first to the next
        const uint64_t playing = sched::pause_position(full, 0, head, false, now);
        const uint64_t keep = playing - std::min(playing, cell_samples);
        const int64_t lowest = std::max<int64_t>(0, sched::first_kept_cell(folded, keep));
        sched::fold_shifts(folded, lowest);
        ASSERT_LE(lowest, next);
        ASSERT_LE(static_cast<int64_t>(folded.shifts.size()), ((next - lowest) / cells) + 1);
        for (int64_t k = lowest; k <= next + (10 * int64_t{cells}); ++k) {
            ASSERT_EQ(sched::frame_timestamp(folded, k), sched::frame_timestamp(full, k)) << k;
        }
        ASSERT_EQ(sched::time_of(folded, head), sched::time_of(full, head));
        // Core folds below the last cell begun: every time it maps, at or after now, finds the
        // index it would have, and every live frame keeps its timestamp
        sched::fold_shifts(core, sched::last_begun_cell(core, now));
        ASSERT_EQ(sched::first_index_at_or_after(core, now, cells),
                  sched::first_index_at_or_after(full, now, cells));
        ASSERT_EQ(sched::frame_timestamp(core, next), sched::frame_timestamp(full, next));
        ASSERT_LE(static_cast<int64_t>(core.shifts.size()), ((next - lowest) / cells) + 1);
    }
    // A write late by no more than its declared jitter never shifts, nor does a cut, nor a clock
    // running fast; only a stall or a raised lead does, once each
    if (ppm <= 0) {
        EXPECT_LE(shifts, stalls + raises);
    }
    // Coarse: a run whose clock keeps time or runs slow may also cut after a stall's shift
    if (ppm < 0) {
        cuts.fast_clock += cut;
    } else if (leaves > 0) {
        cuts.left += cut;
    }
    // The checks above saw frames produced through the run
    EXPECT_GT(next, LIVE_BLOCKS * block_us / sched::CELL_US / 2);
}

TEST(SchedProperty, CapacityLateFramesAndChunkShapeHoldAtEveryWrite) {
    for (int seed = 1; seed <= SEEDS; ++seed) {
        SCOPED_TRACE(seed);
        run_seed(static_cast<uint64_t>(seed));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// A cursor switching in place between instances of frames of one, two and five cells, to targets
// that catch up, back to its own and to new ones again, always switches on a boundary of both frame
// lengths at or past the target's first frame, packs no frame at or past the switch point, and
// sends every frame once, in order, passing over only frames late for the player
TEST(SchedProperty, SwitchesInPlaceOnABoundaryOfBothFormatsWithNoGapOrRepeat) {
    for (int seed = 1; seed <= SEEDS; ++seed) {
        SCOPED_TRACE(seed);
        run_switch_seed(static_cast<uint64_t>(seed));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// Across random segments of rates whose frames span one, two or five cells, and random shifts, a
// cell's position, the first frame past a position, an instance's start and each segment's first
// output index are what their definitions give
TEST(SchedProperty, FrameGridHoldsAcrossSegments) {
    for (int seed = 1; seed <= SEEDS * 10; ++seed) {
        SCOPED_TRACE(seed);
        run_grid_seed(static_cast<uint64_t>(seed));
        if (HasFatalFailure()) {
            return;
        }
    }
}

// A live input keeps every frame at least G_eff - L_p - M_pump ahead and G_eff at least G_next, a
// rebase restores the on-time lead exactly and never runs the timeline back, only a stall or a
// raised lead shifts a source whose clock keeps time or runs fast (a cut that lowers the latency
// never does), admission as if rebased is what the rebase leaves and stays within the horizon, and
// folding keeps every live timestamp. The seeds cut audio both under a fast clock and after a
// member leaves. About 5 s under TSan: forty minute-long runs, each write and frame checked.
TEST(SchedProperty, LiveRebaseAdmissionAndFoldingHoldAtEveryWrite) {
    LiveCuts cuts;
    for (int seed = 1; seed <= SEEDS; ++seed) {
        SCOPED_TRACE(seed);
        run_live_seed(static_cast<uint64_t>(seed), cuts);
        if (HasFatalFailure()) {
            return;
        }
    }
    EXPECT_GT(cuts.fast_clock, 0U);
    EXPECT_GT(cuts.left, 0U);
}

}  // namespace
