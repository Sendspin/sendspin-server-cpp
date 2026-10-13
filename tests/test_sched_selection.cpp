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

/// @file test_sched_selection.cpp
/// @brief The pump's selection order, item_ready, the timer deadline and the drain watch

#include "sched/constants.h"
#include "sched/selection.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using sendspin::server::sched::AudioCandidate;
using sendspin::server::sched::Choice;
using sendspin::server::sched::CursorView;
using sendspin::server::sched::ItemKind;
using sendspin::server::sched::ItemState;
using sendspin::server::sched::Ledger;
using sendspin::server::sched::NEVER;
using sendspin::server::sched::Phase;
using sendspin::server::sched::PumpSnapshot;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr AudioCandidate READY_AT_1120{true, 1120 * MS, NEVER};

TEST(SchedSelection, SelectNext) {
    struct Row {
        const char* name;
        void (*change)(PumpSnapshot&);
        Choice choice;
    };
    const Row rows[] = {
        {"Control: one ready chunk", [](PumpSnapshot& s) { s.player = READY_AT_1120; },
         Choice::PLAYER},
        {"nothing", [](PumpSnapshot&) {}, Choice::NONE},
        {"fragment in progress first",
         [](PumpSnapshot& s) {
             s.fragment_in_progress = true;
             s.time_pending = true;
             s.phase = Phase::AWAIT_MSG2;
         },
         Choice::FRAGMENT},
        {"AwaitMsg2 selects nothing, time replies included",
         [](PumpSnapshot& s) {
             s.phase = Phase::AWAIT_MSG2;
             s.time_pending = true;
             s.control_pending = true;
             s.player = READY_AT_1120;
         },
         Choice::NONE},
        {"AwaitActivate selects only the frozen activation",
         [](PumpSnapshot& s) {
             s.phase = Phase::AWAIT_ACTIVATE;
             s.time_pending = true;
         },
         Choice::REKEY_ACTIVATE},
        // The time reply goes first
        {"time before control",
         [](PumpSnapshot& s) {
             s.time_pending = true;
             s.control_pending = true;
             s.player = READY_AT_1120;
         },
         Choice::TIME},
        {"control before audio",
         [](PumpSnapshot& s) {
             s.control_pending = true;
             s.player = READY_AT_1120;
         },
         Choice::CONTROL},
        {"aged artwork before audio",
         [](PumpSnapshot& s) {
             s.artwork_ready = true;
             s.artwork_aged = true;
             s.player = READY_AT_1120;
         },
         Choice::ARTWORK},
        {"audio before artwork that has not aged",
         [](PumpSnapshot& s) {
             s.artwork_ready = true;
             s.player = READY_AT_1120;
         },
         Choice::PLAYER},
        {"artwork when nothing else is ready", [](PumpSnapshot& s) { s.artwork_ready = true; },
         Choice::ARTWORK},
        // Visualizer messages at 640 go before a chunk at 1120
        {"earliest deadline",
         [](PumpSnapshot& s) {
             s.player = READY_AT_1120;
             s.visualizer = {true, 640 * MS, NEVER};
         },
         Choice::VISUALIZER},
        // The player's deadline is ts - od: 1120 - 500 = 620, before 640
        {"earliest deadline with od",
         [](PumpSnapshot& s) {
             s.player = {true, 620 * MS, NEVER};
             s.visualizer = {true, 640 * MS, NEVER};
         },
         Choice::PLAYER},
        {"a tie goes to the player",
         [](PumpSnapshot& s) {
             s.player = {true, 640 * MS, NEVER};
             s.visualizer = {true, 640 * MS, NEVER};
         },
         Choice::PLAYER},
        {"a blocked player lets the visualizer go",
         [](PumpSnapshot& s) {
             s.player = {false, 0, 1120 * MS};
             s.visualizer = {true, 640 * MS, NEVER};
         },
         Choice::VISUALIZER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PumpSnapshot snapshot;
        row.change(snapshot);
        EXPECT_EQ(sched::select_next(snapshot), row.choice);
    }
}

TEST(SchedSelection, ArtworkAgedOncePerAge) {
    const int64_t age = sched::timing_defaults::ARTWORK_AGE_US;
    struct Row {
        const char* name;
        int64_t queued;
        std::optional<int64_t> last_aged;
        int64_t now;
        bool aged;
    };
    const Row rows[] = {
        {"Control: waited A_age, none aged before", 0, std::nullopt, age, true},
        {"waited less", 0, std::nullopt, age - 1, false},
        {"an aged write within A_age", 0, 100 * MS, age + 100 * MS - 1, false},
        {"an aged write A_age ago", 0, 100 * MS, age + 100 * MS, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::artwork_aged(row.queued, row.last_aged, age, row.now), row.aged);
    }
}

TEST(SchedSelection, NextWake) {
    struct Row {
        const char* name;
        void (*change)(PumpSnapshot&);
        int64_t wake;
    };
    const Row rows[] = {
        {"nothing pending", [](PumpSnapshot&) {}, NEVER},
        {"Control: the minimum of every deadline",
         [](PumpSnapshot& s) {
             s.player.room_time_us = 640 * MS;
             s.visualizer.room_time_us = 610 * MS;
             s.artwork_wake_us = 700 * MS;
             s.drain_watch_us = 900 * MS;
         },
         610 * MS},
        {"player room", [](PumpSnapshot& s) { s.player.room_time_us = 640 * MS; }, 640 * MS},
        {"artwork", [](PumpSnapshot& s) { s.artwork_wake_us = 700 * MS; }, 700 * MS},
        {"drain watch", [](PumpSnapshot& s) { s.drain_watch_us = 900 * MS; }, 900 * MS},
        {"AwaitMsg2 arms nothing",
         [](PumpSnapshot& s) {
             s.phase = Phase::AWAIT_MSG2;
             s.player.room_time_us = 640 * MS;
         },
         NEVER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        PumpSnapshot snapshot;
        row.change(snapshot);
        EXPECT_EQ(sched::next_wake(snapshot), row.wake);
    }
}

// item_ready for player A
TEST(SchedSelection, ItemReady) {
    Ledger empty(100000, true);
    Ledger full(100000, true);
    for (int64_t j = 0; j < 7; ++j) {
        full.add((640 + 80 * j) * MS, 14125);
    }
    // At its capacity: no message of any size fits
    Ledger brim(100000, true);
    brim.add(5000 * MS, 100000);
    CursorView a0;
    a0.phase_normal = true;
    a0.stream_active = true;
    a0.available = true;
    a0.next_payload_bytes = 4 * 3528;

    struct Row {
        const char* name;
        void (*change)(CursorView&);
        const Ledger& ledger;
        ItemState state;
    };
    const Row rows[] = {
        {"Control: A0", [](CursorView&) {}, empty, {ItemKind::CHUNK, NEVER}},
        {"room blocked gives the room time",
         [](CursorView&) {},
         full,
         {ItemKind::BLOCKED, 640 * MS}},
        {"a due switch with the target frame needs no room",
         [](CursorView& c) {
             c.switch_point = 0;
             c.target_holds_switch_point = true;
         },
         brim,
         {ItemKind::SWITCH_START, NEVER}},
        {"a due switch without the target frame",
         [](CursorView& c) { c.switch_point = 0; },
         empty,
         {ItemKind::STARVED, NEVER}},
        {"a due switch while unavailable",
         [](CursorView& c) {
             c.switch_point = 0;
             c.target_holds_switch_point = true;
             c.available = false;
         },
         empty,
         {ItemKind::STARVED, NEVER}},
        {"a switch not yet due",
         [](CursorView& c) {
             c.switch_point = 4;
             c.target_holds_switch_point = true;
         },
         empty,
         {ItemKind::CHUNK, NEVER}},
        {"past its own final index",
         [](CursorView& c) {
             c.next = 454;
             c.final_index = 453;
         },
         empty,
         {ItemKind::FINISHED, NEVER}},
        {"at its final index",
         [](CursorView& c) {
             c.next = 453;
             c.final_index = 453;
         },
         empty,
         {ItemKind::CHUNK, NEVER}},
        {"the frame is not produced",
         [](CursorView& c) { c.next_payload_bytes.reset(); },
         empty,
         {ItemKind::STARVED, NEVER}},
        {"no active stream",
         [](CursorView& c) { c.stream_active = false; },
         empty,
         {ItemKind::IDLE, NEVER}},
        {"the phase allows no audio",
         [](CursorView& c) { c.phase_normal = false; },
         empty,
         {ItemKind::IDLE, NEVER}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        CursorView cursor = a0;
        row.change(cursor);
        const ItemState state = sched::item_ready(cursor, row.ledger, 1600);
        EXPECT_EQ(state.kind, row.state.kind);
        EXPECT_EQ(state.room_time_us, row.state.room_time_us);
    }

    // The snapshot entry: ready items carry their deadline, blocked and held ones their room time
    const AudioCandidate ready = sched::audio_candidate({ItemKind::CHUNK, NEVER}, 560 * MS);
    EXPECT_TRUE(ready.ready);
    EXPECT_EQ(ready.deadline_us, 560 * MS);
    const AudioCandidate blocked = sched::audio_candidate({ItemKind::BLOCKED, 640 * MS}, 1120 * MS);
    EXPECT_FALSE(blocked.ready);
    EXPECT_EQ(blocked.room_time_us, 640 * MS);
    EXPECT_EQ(sched::audio_candidate({ItemKind::STARVED, NEVER}, 0).room_time_us, NEVER);
    // A held chunk is not ready and wakes the pump when it goes anyway
    const AudioCandidate held = sched::audio_candidate({ItemKind::HELD, 840 * MS}, 1000 * MS);
    EXPECT_FALSE(held.ready);
    EXPECT_EQ(held.room_time_us, 840 * MS);
}

// Armed at the last completion while no marker is queued, and due once the ledger is empty
TEST(SchedSelection, DrainWatch) {
    Ledger ledger(100000, true);
    ledger.add(21630295, 5357);
    // Control: active, past its final index, no marker queued
    EXPECT_EQ(sched::drain_watch_time(true, true, 0, ledger), 21630295);
    EXPECT_EQ(sched::drain_watch_time(true, true, 1, ledger), NEVER);
    EXPECT_EQ(sched::drain_watch_time(true, false, 0, ledger), NEVER);
    EXPECT_EQ(sched::drain_watch_time(false, true, 0, ledger), NEVER);

    EXPECT_FALSE(sched::drain_watch_due(true, true, 0, ledger, 21630294));
    EXPECT_TRUE(sched::drain_watch_due(true, true, 0, ledger, 21630295));
    EXPECT_FALSE(sched::drain_watch_due(true, true, 1, ledger, 21630295));

    // An od decrease during a freeze re-counts the entry: the watch moves to the new completion
    ledger.set_output_delay_us(50 * MS);
    EXPECT_TRUE(sched::drain_watch_due(true, true, 0, ledger, 21580295));
    ledger.set_output_delay_us(0);
    EXPECT_FALSE(sched::drain_watch_due(true, true, 0, ledger, 21580295));
    EXPECT_EQ(sched::drain_watch_time(true, true, 0, ledger), 21630295);
}

}  // namespace
