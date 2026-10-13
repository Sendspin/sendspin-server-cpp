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

/// @file test_sched_admission.cpp
/// @brief Write admission and readiness and the host view's run, latch and drain decisions

#include "sched/admission.h"
#include "sched/constants.h"
#include "sched/run_view.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using sendspin::server::sched::AdmissionInput;
using sendspin::server::sched::Anchor;
using sendspin::server::sched::DrainOutcome;
using sendspin::server::sched::DrainResolution;
using sendspin::server::sched::EpochId;
using sendspin::server::sched::InputKind;
using sendspin::server::sched::InputState;
using sendspin::server::sched::NEVER;
using sendspin::server::sched::RunId;
using sendspin::server::sched::RunState;
using sendspin::server::sched::RunView;
using sendspin::server::sched::WriteDecision;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr uint64_t RING = 100000000;
constexpr uint64_t R_MIN = 882;

/// A buffered input: 44.1 kHz, G 560 ms, W 1 s
AdmissionInput buffered(RunState run, uint64_t head) {
    AdmissionInput in;
    in.run = run;
    in.head = head;
    in.head_rate = 44100;
    in.group_lead_us = 560 * MS;
    in.write_ahead_us = 1000 * MS;
    in.ring_limit = RING;
    return in;
}

/// A live input: 48 kHz, G 140 ms, W_live 200 ms, L_p 30 ms, J_in 10 ms; k* is the cell after the
/// head's, 50 at 1140 ms, when the head is a second in
AdmissionInput live(RunState run, uint64_t head) {
    AdmissionInput in;
    in.kind = InputKind::LIVE;
    in.run = run;
    in.head = head;
    in.head_rate = 48000;
    in.group_lead_us = 140 * MS;
    in.write_ahead_us = 200 * MS;
    in.ring_limit = RING;
    in.shift_index = 50;
    in.rebase.kind = InputKind::LIVE;
    in.rebase.group_lead_us = 140 * MS;
    in.rebase.pipeline_latency_us = 30 * MS;
    in.rebase.live_jitter_us = 10 * MS;
    return in;
}

/// The live input's epoch 1: T0 140 ms from position 0
Anchor epoch_j() {
    return sched::make_anchor(140 * MS, 0, 48000, 2);
}

/// Epoch 2: T0 2610 ms from position 0
Anchor epoch2() {
    return sched::make_anchor(2610 * MS, 0, 44100, 2);
}

/// Epoch 3: resumed at 12000 from 281,799, T0 12560
Anchor epoch3() {
    return sched::make_anchor(12560 * MS, 281799, 44100, 2);
}

TEST(SchedAdmission, AdmissionLimit) {
    AdmissionInput paused = buffered(RunState::PAUSED, 350595);
    paused.p_pause = 281799;
    AdmissionInput live_paused = paused;
    live_paused.kind = InputKind::LIVE;
    live_paused.ring_limit = 400000;
    AdmissionInput draining = buffered(RunState::PLAYING, 100000);
    draining.draining = true;
    AdmissionInput latched = buffered(RunState::NONE, 0);
    latched.latched = true;
    AdmissionInput small_ring = buffered(RunState::PLAYING, 100000);
    small_ring.ring_limit = 150000;
    AdmissionInput live_draining_pause = live_paused;
    live_draining_pause.draining = true;
    // k* is the cell holding the head, 113, at 4870 ms; G 560 ms, od_max 0
    AdmissionInput stalled = buffered(RunState::PLAYING, 100000);
    stalled.shift_index = 113;
    stalled.rebase.group_lead_us = 560 * MS;
    AdmissionInput on_time = stalled;
    on_time.shift_index = 140;
    AdmissionInput buffered_j = live(RunState::PLAYING, 48000);
    buffered_j.kind = InputKind::BUFFERED;
    buffered_j.rebase.kind = InputKind::BUFFERED;
    const auto shifted_j = [] {
        Anchor anchor = epoch_j();
        sched::add_shift(anchor, 50, 80 * MS);
        return anchor;
    };

    struct Row {
        const char* name;
        AdmissionInput in;
        Anchor anchor;
        int64_t now;
        uint64_t limit;
    };
    const Row rows[] = {
        {"Control: frozen while paused", paused, epoch2(), 10000 * MS, 325899},
        // The first write at 0 anchors and admits W of 44.1 kHz
        {"a write into a new run admits W", buffered(RunState::NONE, 0), epoch2(), 0, 44100},
        {"a write into a pending epoch admits W", buffered(RunState::PENDING, 0), epoch2(),
         2050 * MS, 44100},
        // P_max(5000) = 174,195
        {"playing: up to now + G + W", buffered(RunState::PLAYING, 100000), epoch2(), 5000 * MS,
         174195},
        // At 12560 the limit reaches the head
        {"playing at the head", buffered(RunState::PLAYING, 350595), epoch3(), 12560 * MS, 350595},
        // Admission reaches P_end at 20070.300
        {"reaches P_end", buffered(RunState::PLAYING, 600000), epoch3(), 20070300, 681799},
        {"a head ahead of the horizon admits nothing", buffered(RunState::PLAYING, 350595),
         epoch3(), 12000 * MS, 350595},
        {"the ring caps it", small_ring, epoch2(), 5000 * MS, 150000},
        {"draining refuses", draining, epoch2(), 5000 * MS, 100000},
        {"latched refuses", latched, epoch2(), 0, 0},
        {"live paused takes up to the ring", live_paused, epoch2(), 10000 * MS, 400000},
        {"a live pause during a drain refuses", live_draining_pause, epoch2(), 10000 * MS, 350595},
        // At 6000 ms frame 113 is 1130 ms late, so the head's time counts the 1690 ms shift that
        // restores G: 100,000 + floor(992,427 us at 44.1 kHz)
        {"a stalled input admits as if rebased", stalled, epoch2(), 6000 * MS, 143766},
        {"a k* on time changes nothing", on_time, epoch2(), 5000 * MS, 174195},
        // Live: H = t + 140 + 200 ms; the head's time is 1140 ms
        {"live: up to now + G + W_live", live(RunState::PLAYING, 48000), epoch_j(), 1000 * MS,
         57600},
        // The live threshold is G - L_p - M_pump = 100 ms of lead for k*
        {"live: k* at the threshold admits the plain horizon", live(RunState::PLAYING, 48000),
         epoch_j(), 1040 * MS, 59520},
        // Once rebased the head is G - L_p + J_in = 120 ms ahead, so 220 ms fit past it
        {"live: k* under the threshold admits as if rebased", live(RunState::PLAYING, 48000),
         epoch_j(), 1040001, 58560},
        {"live: a stalled head admits what rebase_delta implies", live(RunState::PLAYING, 48000),
         epoch_j(), 1100 * MS, 58560},
        {"live: the same once the 80 ms shift is applied", live(RunState::PLAYING, 48000),
         shifted_j(), 1100 * MS, 58560},
        {"buffered at the same lead does not rebase", buffered_j, epoch_j(), 1100 * MS, 62400},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::admission_limit(row.in, row.anchor, row.now), row.limit);
    }
}

TEST(SchedAdmission, DecideWrite) {
    AdmissionInput paused = buffered(RunState::PAUSED, 350595);
    paused.p_pause = 281799;
    AdmissionInput draining = buffered(RunState::PLAYING, 100000);
    draining.draining = true;
    AdmissionInput latched = buffered(RunState::NONE, 0);
    latched.latched = true;
    AdmissionInput both = draining;
    both.latched = true;

    struct Taken {
        uint64_t frames;
        InputState state;
    };
    struct Row {
        const char* name;
        AdmissionInput in;
        uint64_t offered;
        Taken decision;
    };
    const Row rows[] = {
        {"Control: room for everything", buffered(RunState::NONE, 0), 882, {882, InputState::OPEN}},
        {"cut short by admission", buffered(RunState::NONE, 0), 441000, {44100, InputState::FULL}},
        // Writes during the pause take nothing
        {"frozen while paused", paused, 882, {0, InputState::FULL}},
        {"draining", draining, 882, {0, InputState::DRAINING}},
        {"latched", latched, 882, {0, InputState::STOP_LATCHED}},
        {"the latch reports first", both, 882, {0, InputState::STOP_LATCHED}},
        // A live input's first write admits W_live
        {"a live run's first write admits W_live",
         live(RunState::NONE, 0),
         48000,
         {9600, InputState::FULL}},
        // The head's time, which a live write's lowering reads, comes with a playing epoch's
        {"playing", buffered(RunState::PLAYING, 0), 0, {0, InputState::OPEN}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const WriteDecision decision = sched::decide_write(row.in, epoch2(), row.offered, 0);
        EXPECT_EQ(decision.frames, row.decision.frames);
        EXPECT_EQ(decision.state, row.decision.state);
        EXPECT_EQ(decision.head_us, row.in.run == RunState::PLAYING
                                        ? std::optional(sched::time_of(epoch2(), row.in.head))
                                        : std::nullopt);
    }
}

TEST(SchedAdmission, ReadinessTime) {
    // 44,100 admitted at 0 with T0 560; one cell more is admitted at 20 ms
    const Anchor epoch1 = sched::make_anchor(560 * MS, 0, 44100, 2);
    AdmissionInput paused = buffered(RunState::PAUSED, 350595);
    paused.p_pause = 281799;
    AdmissionInput draining = buffered(RunState::PLAYING, 44100);
    draining.draining = true;
    AdmissionInput stalled = buffered(RunState::PLAYING, 44100);
    stalled.shift_index = 0;
    stalled.rebase.group_lead_us = 560 * MS;
    AdmissionInput live_full = live(RunState::PLAYING, 57600);
    live_full.shift_index = 60;
    const AdmissionInput live_stalled = live(RunState::PLAYING, 58560);

    struct Row {
        const char* name;
        AdmissionInput in;
        Anchor anchor;
        int64_t now;
        int64_t ready;
    };
    const Row rows[] = {
        {"Control: the next cell", buffered(RunState::PLAYING, 44100), epoch1, 0, 20 * MS},
        {"already admitted", buffered(RunState::PLAYING, 44100), epoch1, 20 * MS, 20 * MS},
        // After the resume the host can write again from 12580
        {"after the resume", buffered(RunState::PLAYING, 350595), epoch3(), 12000100, 12580 * MS},
        {"a new run is ready at once", buffered(RunState::NONE, 0), epoch1, 5 * MS, 5 * MS},
        {"paused buffered waits for an event", paused, epoch2(), 9000 * MS, NEVER},
        {"draining", draining, epoch1, 0, NEVER},
        // Frame 0 is 1440 ms late at 2000 ms, and the shift that restores G puts the head at the
        // horizon: only the next publish or shift moves admission
        {"a pending rebase waits for an event", stalled, epoch1, 2000 * MS, NEVER},
        // The head at 1340 ms is at the horizon; 882 samples more at 48 kHz take 18,375 us
        {"live: the inverse of P_max", live_full, epoch_j(), 1000 * MS, 1018375},
        {"live: a stalled head waits for an event", live_stalled, epoch_j(), 1100 * MS, NEVER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const int64_t ready = sched::readiness_time(row.in, row.anchor, R_MIN, row.now);
        EXPECT_EQ(ready, row.ready);
        if (ready != NEVER && ready > row.now) {
            // The limit admits r_min exactly from that instant, not a microsecond earlier
            EXPECT_GE(sched::admitted_past_head(row.in, row.anchor, ready), R_MIN);
            EXPECT_LT(sched::admitted_past_head(row.in, row.anchor, ready - 1), R_MIN);
        }
    }
}

std::optional<DrainOutcome> outcome(const std::optional<DrainResolution>& resolution) {
    return resolution.has_value() ? std::optional<DrainOutcome>(resolution->outcome) : std::nullopt;
}

RunView playing(uint64_t epoch) {
    RunView view;
    view.run = RunState::PLAYING;
    view.epoch = EpochId{epoch};
    return view;
}

// Whoever resolves a staged drain first posts its one outcome
TEST(SchedRunView, StagedDrainResolvesOnce) {
    struct Row {
        const char* name;
        std::optional<DrainOutcome> (*first)(RunView&);
        DrainOutcome outcome;
    };
    const Row rows[] = {
        {"Control: completion",
         [](RunView& v) { return outcome(sched::view_drain_complete(v, EpochId{1})); },
         DrainOutcome::PLAYED},
        {"flush", [](RunView& v) { return outcome(sched::view_flush(v, EpochId{2})); },
         DrainOutcome::FLUSHED},
        {"host stop", [](RunView& v) { return outcome(sched::view_stop(v)); },
         DrainOutcome::STOPPED},
        {"server stop", [](RunView& v) { return outcome(sched::view_server_stop(v).drain); },
         DrainOutcome::STOPPED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RunView view = playing(1);
        EXPECT_FALSE(sched::view_drain(view).has_value());
        EXPECT_EQ(row.first(view), row.outcome);
        // The loser of any race resolves nothing
        EXPECT_FALSE(sched::view_drain_complete(view, EpochId{1}).has_value());
        EXPECT_FALSE(sched::view_flush(view, EpochId{3}).has_value());
        EXPECT_FALSE(sched::view_stop(view).has_value());
    }
}

TEST(SchedRunView, DrainCalls) {
    // No run: played at once, nothing staged
    RunView none;
    EXPECT_EQ(outcome(sched::view_drain(none)), DrainOutcome::PLAYED);
    EXPECT_FALSE(none.drain.has_value());

    // A second drain() joins: two callbacks, one outcome
    RunView twice = playing(1);
    EXPECT_FALSE(sched::view_drain(twice).has_value());
    EXPECT_FALSE(sched::view_drain(twice).has_value());
    const std::optional<DrainResolution> both = sched::view_drain_complete(twice, EpochId{1});
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(both->outcome, DrainOutcome::PLAYED);
    EXPECT_EQ(both->callbacks, 2U);
    EXPECT_EQ(twice.run, RunState::NONE);

    // A pause keeps it pending: completion is not delivered while paused; the resumed epoch
    // drains instead, and the old epoch's completion is stale
    RunView paused = playing(1);
    sched::view_drain(paused);
    EXPECT_TRUE(sched::view_pause(paused));
    EXPECT_FALSE(sched::view_drain_complete(paused, EpochId{1}).has_value());
    EXPECT_TRUE(sched::view_resume(paused, RunId{2}, EpochId{2}, true));
    EXPECT_FALSE(sched::view_drain_complete(paused, EpochId{1}).has_value());
    EXPECT_EQ(outcome(sched::view_drain_complete(paused, EpochId{2})), DrainOutcome::PLAYED);

    // flush() then drain() with no write: the pending epoch drains and completes
    RunView flushed = playing(1);
    sched::view_flush(flushed, EpochId{2});
    EXPECT_EQ(flushed.run, RunState::PENDING);
    sched::view_drain(flushed);
    EXPECT_EQ(outcome(sched::view_drain_complete(flushed, EpochId{2})), DrainOutcome::PLAYED);
}

TEST(SchedRunView, StopsAndTheLatch) {
    // The host's stop discards a staged flush and a staged drain; the next write begins a run
    RunView host = playing(1);
    sched::view_flush(host, EpochId{2});
    sched::view_drain(host);
    EXPECT_EQ(outcome(sched::view_stop(host)), DrainOutcome::STOPPED);
    EXPECT_EQ(host.run, RunState::NONE);
    sched::view_anchor_write(host, RunId{2}, EpochId{3});
    EXPECT_EQ(host.run, RunState::PLAYING);
    EXPECT_EQ(host.epoch, EpochId{3});

    // A server-initiated stop latches; flush, pause and resume do nothing; drain resolves at once
    // as stopped and keeps the latch; the host's stop clears it
    RunView server = playing(4);
    const sched::ServerStopResult stop = sched::view_server_stop(server);
    EXPECT_TRUE(stop.applied);
    EXPECT_EQ(stop.stopped_epoch, EpochId{4});
    EXPECT_TRUE(server.latched);
    EXPECT_FALSE(sched::view_flush(server, EpochId{5}).has_value());
    EXPECT_FALSE(sched::view_pause(server));
    EXPECT_FALSE(sched::view_resume(server, RunId{3}, EpochId{5}, true));
    EXPECT_EQ(server.run, RunState::NONE);
    EXPECT_EQ(outcome(sched::view_drain(server)), DrainOutcome::STOPPED);
    EXPECT_TRUE(server.latched);
    EXPECT_FALSE(sched::view_stop(server).has_value());
    EXPECT_FALSE(server.latched);
    sched::view_anchor_write(server, RunId{4}, EpochId{6});
    EXPECT_EQ(server.run, RunState::PLAYING);

    // A server-initiated stop on a paused view does nothing, and resume() still works
    RunView paused = playing(7);
    sched::view_pause(paused);
    EXPECT_FALSE(sched::view_server_stop(paused).applied);
    EXPECT_FALSE(paused.latched);
    EXPECT_TRUE(sched::view_resume(paused, RunId{5}, EpochId{8}, true));
    EXPECT_EQ(paused.run, RunState::PLAYING);

    // A stop with no run changes nothing
    RunView none;
    EXPECT_FALSE(sched::view_server_stop(none).applied);
    EXPECT_FALSE(none.latched);
    EXPECT_FALSE(sched::view_stop(none).has_value());
    EXPECT_EQ(none.run, RunState::NONE);

    // A pending epoch is stopped like a playing run
    RunView pending = playing(9);
    sched::view_flush(pending, EpochId{10});
    const sched::ServerStopResult pending_stop = sched::view_server_stop(pending);
    EXPECT_TRUE(pending_stop.applied);
    EXPECT_EQ(pending_stop.stopped_epoch, EpochId{10});
}

// With a flush and a pause in either order, resume() anchors at p0 when input exists at or after
// it, and otherwise leaves the epoch pending for the first write
TEST(SchedRunView, FlushAndPauseInEitherOrder) {
    struct Row {
        const char* name;
        bool flush_first;
        bool written_while_paused;
        RunState after_resume;
    };
    const Row rows[] = {
        {"Control: flush, pause, writes, resume", true, true, RunState::PLAYING},
        {"flush, pause, no writes, resume", true, false, RunState::PENDING},
        {"pause, flush, writes, resume", false, true, RunState::PLAYING},
        {"pause, flush, no writes, resume", false, false, RunState::PENDING},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RunView view = playing(1);
        view.run_id = RunId{1};
        if (row.flush_first) {
            sched::view_flush(view, EpochId{2});
            EXPECT_TRUE(sched::view_pause(view));
        } else {
            EXPECT_TRUE(sched::view_pause(view));
            sched::view_flush(view, EpochId{2});
        }
        EXPECT_EQ(view.run, RunState::PAUSED);
        EXPECT_TRUE(sched::view_resume(view, RunId{2}, EpochId{3}, row.written_while_paused));
        EXPECT_EQ(view.run, row.after_resume);
        EXPECT_EQ(view.run_id, RunId{2});
        EXPECT_EQ(view.epoch, EpochId{3});
        // The first write anchors a pending epoch in place, keeping the resume's ids
        sched::view_anchor_write(view, RunId{9}, EpochId{9});
        EXPECT_EQ(view.run, RunState::PLAYING);
        EXPECT_EQ(view.run_id, RunId{2});
        EXPECT_EQ(view.epoch, EpochId{3});
    }
}

// Admission across a flush and a pause: positions restart at p0 = 0, a pause after the flush
// freezes at p0 + W, and a pending epoch admits W from p0 however much was written before
TEST(SchedAdmission, FlushAndPauseAdmitWFromP0) {
    constexpr uint64_t w_samples = 44100;
    AdmissionInput paused = buffered(RunState::PAUSED, 0);
    paused.p_pause = 0;
    AdmissionInput paused_written = paused;
    paused_written.head = 30000;
    AdmissionInput pending_written = buffered(RunState::PENDING, 30000);
    struct Row {
        const char* name;
        AdmissionInput in;
        uint64_t limit;
    };
    const Row rows[] = {
        {"Control: pending, nothing written", buffered(RunState::PENDING, 0), w_samples},
        {"paused after a flush, nothing written", paused, w_samples},
        {"paused after a flush, some written", paused_written, w_samples},
        {"pending with input staged admits W from p0, not from the head", pending_written,
         w_samples},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::admission_limit(row.in, epoch2(), 50000 * MS), row.limit);
    }
}

}  // namespace
