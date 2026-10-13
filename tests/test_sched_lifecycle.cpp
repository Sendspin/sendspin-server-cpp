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

/// @file test_sched_lifecycle.cpp
/// @brief Desired-state markers and the drain predicates

#include "sched/constants.h"
#include "sched/lifecycle.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using sendspin::server::sched::Codec;
using sendspin::server::sched::ControlItemKind;
using sendspin::server::sched::ControlItemView;
using sendspin::server::sched::EpochId;
using sendspin::server::sched::FormatKey;
using sendspin::server::sched::GroupDrainView;
using sendspin::server::sched::Ledger;
using sendspin::server::sched::MarkerEmission;
using sendspin::server::sched::RoleSet;
using sendspin::server::sched::RunId;
using sendspin::server::sched::StartPreconditions;
using sendspin::server::sched::StreamAction;
using sendspin::server::sched::StreamRole;
using sendspin::server::sched::StreamSide;
using sendspin::server::sched::Want;
using sendspin::server::sched::WantInputs;
namespace sched = sendspin::server::sched;

constexpr int64_t MS = 1000;
constexpr FormatKey PCM44{{Codec::PCM, 44100, 2, 16}, 0};
constexpr FormatKey FLAC48{{Codec::FLAC, 48000, 2, 16}, 0};
constexpr RoleSet PLAYER = RoleSet::of(StreamRole::PLAYER);
constexpr RoleSet VISUALIZER = RoleSet::of(StreamRole::VISUALIZER);
constexpr RoleSet NO_ROLES{};

/// Every input of a wanted stream in run 1, epoch 2
WantInputs wanted() {
    WantInputs in;
    in.role_written = true;
    in.role_ahead = true;
    in.group_playing = true;
    in.run = RunId{1};
    in.epoch = EpochId{2};
    in.available = true;
    in.format = PCM44;
    return in;
}

constexpr StartPreconditions ALL_HOLD{true, true, true, true};

StreamSide active(uint64_t run, uint64_t epoch, FormatKey format = PCM44) {
    return {true, RunId{run}, EpochId{epoch}, format};
}

Want want(bool is_active, uint64_t run, uint64_t epoch, FormatKey format = PCM44) {
    return {is_active, RunId{run}, EpochId{epoch}, format};
}

TEST(SchedLifecycle, EvaluateWant) {
    struct Row {
        const char* name;
        void (*change)(WantInputs&);
        bool active;
    };
    const Row rows[] = {
        {"Control: everything holds", [](WantInputs&) {}, true},
        {"role not written", [](WantInputs& in) { in.role_written = false; }, false},
        {"the next activation removes the role", [](WantInputs& in) { in.role_ahead = false; },
         false},
        {"no playing run", [](WantInputs& in) { in.group_playing = false; }, false},
        {"unavailable", [](WantInputs& in) { in.available = false; }, false},
        {"no producible format", [](WantInputs& in) { in.format.reset(); }, false},
        {"draining, not done", [](WantInputs& in) { in.epoch_draining = true; }, true},
        {"draining and done",
         [](WantInputs& in) {
             in.epoch_draining = true;
             in.drain_done = true;
         },
         false},
        {"done but the epoch is not draining", [](WantInputs& in) { in.drain_done = true; }, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        WantInputs in = wanted();
        row.change(in);
        const Want result = sched::evaluate_want(in);
        EXPECT_EQ(result.active, row.active);
        EXPECT_EQ(result.run, RunId{1});
        EXPECT_EQ(result.epoch, EpochId{2});
    }
}

// The marker table, rows in order
TEST(SchedLifecycle, ReconcileStream) {
    struct Row {
        const char* name;
        StreamSide sent;
        Want want;
        StartPreconditions pre;
        StreamAction action;
    };
    const Row rows[] = {
        {"Control: a fresh start", {}, want(true, 1, 2), ALL_HOLD, StreamAction::START},
        {"active and not wanted: end", active(1, 2), want(false, 1, 2), ALL_HOLD,
         StreamAction::END},
        {"active in another run: end", active(1, 2), want(true, 2, 2), ALL_HOLD, StreamAction::END},
        // A pause then resume before the marker runs is an end, never a clear
        {"a run change with an epoch change is an end", active(1, 2), want(true, 2, 3), ALL_HOLD,
         StreamAction::END},
        {"active in another epoch: clear", active(1, 2), want(true, 1, 3), ALL_HOLD,
         StreamAction::CLEAR},
        // A format change with an epoch change: the clear; the in-place start is the cursor's
        {"epoch and format change: clear", active(1, 2, FLAC48), want(true, 1, 3, PCM44), ALL_HOLD,
         StreamAction::CLEAR},
        {"a format change alone is the cursor's", active(1, 2, FLAC48), want(true, 1, 2, PCM44),
         ALL_HOLD, StreamAction::NONE},
        {"active and current", active(1, 2), want(true, 1, 2), ALL_HOLD, StreamAction::NONE},
        {"start: unavailable", {}, want(true, 1, 2), {false, true, true, true}, StreamAction::NONE},
        {"start: role not written",
         {},
         want(true, 1, 2),
         {true, false, true, true},
         StreamAction::NONE},
        {"start: no client/state since the activation",
         {},
         want(true, 1, 2),
         {true, true, false, true},
         StreamAction::NONE},
        // Spec messaging.md "stream/start": a client/state never starts a stream by itself
        {"start: no trigger pending",
         {},
         want(true, 1, 2),
         {true, true, true, false},
         StreamAction::NONE},
        {"not active and not wanted", {}, want(false, 1, 2), ALL_HOLD, StreamAction::NONE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::reconcile_stream(row.sent, row.want, row.pre), row.action);
    }
}

// One message per dequeue, ends before clears before starts
TEST(SchedLifecycle, MarkerStepOrdersEndClearStart) {
    std::array<StreamSide, 2> sent = {active(1, 2), StreamSide{}};
    const std::array<Want, 2> wants = {want(true, 2, 3), want(true, 2, 3, {{}, 9})};
    const std::array<StartPreconditions, 2> pre = {ALL_HOLD, ALL_HOLD};

    // Pause then resume before the marker: the player ends, then both start
    MarkerEmission emission = sched::marker_step(sent, wants, pre);
    EXPECT_EQ(emission.action, StreamAction::END);
    EXPECT_EQ(emission.roles, PLAYER);
    sched::apply_action(sent[0], wants[0], emission.action);
    EXPECT_FALSE(sent[0].active);

    emission = sched::marker_step(sent, wants, pre);
    EXPECT_EQ(emission.action, StreamAction::START);
    EXPECT_EQ(emission.roles, PLAYER.with(StreamRole::VISUALIZER));
    sched::apply_action(sent[0], wants[0], emission.action);
    sched::apply_action(sent[1], wants[1], emission.action);
    EXPECT_TRUE(sent[0].active);
    EXPECT_EQ(sent[0].run, RunId{2});
    EXPECT_EQ(sent[1].format.visualizer_config, 9U);

    // Nothing left: the marker pops
    EXPECT_EQ(sched::marker_step(sent, wants, pre).action, StreamAction::NONE);
}

// Rows 1 and 2 are both ends: a player no longer wanted and a visualizer whose run changed go in
// one stream/end
TEST(SchedLifecycle, MarkerEndsListEveryEndingRole) {
    std::array<StreamSide, 2> sent = {active(1, 2), active(1, 2, {{}, 9})};
    const std::array<Want, 2> wants = {want(false, 2, 3), want(true, 2, 3, {{}, 9})};
    const std::array<StartPreconditions, 2> pre = {ALL_HOLD, ALL_HOLD};
    const MarkerEmission emission = sched::marker_step(sent, wants, pre);
    EXPECT_EQ(emission.action, StreamAction::END);
    EXPECT_EQ(emission.roles, PLAYER.with(StreamRole::VISUALIZER));
}

// A clear of the player and a start of the visualizer in one marker both go out, the start
// second, with the trigger still pending for it
TEST(SchedLifecycle, MarkerClearThenStart) {
    std::array<StreamSide, 2> sent = {active(1, 2), StreamSide{}};
    const std::array<Want, 2> wants = {want(true, 1, 3), want(true, 1, 3)};
    const std::array<StartPreconditions, 2> pre = {ALL_HOLD, ALL_HOLD};
    MarkerEmission emission = sched::marker_step(sent, wants, pre);
    EXPECT_EQ(emission.action, StreamAction::CLEAR);
    EXPECT_EQ(emission.roles, PLAYER);
    sched::apply_action(sent[0], wants[0], emission.action);
    EXPECT_EQ(sent[0].epoch, EpochId{3});
    emission = sched::marker_step(sent, wants, pre);
    EXPECT_EQ(emission.action, StreamAction::START);
    EXPECT_EQ(emission.roles, VISUALIZER);
}

TEST(SchedLifecycle, RolesAhead) {
    struct Row {
        const char* name;
        std::vector<ControlItemView> behind;
        RoleSet roles;
    };
    const Row rows[] = {
        {"Control: nothing queued gives roles_written", {}, PLAYER},
        {"messages only", {{ControlItemKind::MESSAGE, {}}, {ControlItemKind::MARKER, {}}}, PLAYER},
        {"the first activation's roles",
         {{ControlItemKind::MESSAGE, {}},
          {ControlItemKind::ACTIVATION, NO_ROLES},
          {ControlItemKind::MARKER, {}},
          {ControlItemKind::ACTIVATION, PLAYER}},
         NO_ROLES},
        // A rekey item's frozen activation without the player (re-pairing)
        {"a rekey item's frozen roles", {{ControlItemKind::REKEY, NO_ROLES}}, NO_ROLES},
        {"a rekey keeping the player", {{ControlItemKind::REKEY, PLAYER}}, PLAYER},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::roles_ahead(row.behind, PLAYER), row.roles);
    }
}

// The queue [marker, activation removing the player, marker, activation re-adding it]: the first
// marker ends the stream before the removing activation
TEST(SchedLifecycle, RemoveThenReAddEndsAtTheFirstMarker) {
    const std::vector<ControlItemView> behind_first = {{ControlItemKind::ACTIVATION, NO_ROLES},
                                                       {ControlItemKind::MARKER, {}},
                                                       {ControlItemKind::ACTIVATION, PLAYER}};
    WantInputs in = wanted();
    in.role_ahead = sched::roles_ahead(behind_first, PLAYER).contains(StreamRole::PLAYER);
    const Want result = sched::evaluate_want(in);
    EXPECT_EQ(sched::reconcile_stream(active(1, 2), result, ALL_HOLD), StreamAction::END);
}

TEST(SchedLifecycle, DrainDone) {
    Ledger empty(100000, true);
    Ledger counting(100000, true);
    counting.add(1000 * MS, 5000);
    struct Row {
        const char* name;
        int64_t next;
        std::optional<int64_t> final_index;
        const Ledger& ledger;
        bool done;
    };
    const Row rows[] = {
        {"Control: done", 454, 453, empty, true},
        {"next equal to its final index", 453, 453, empty, false},
        {"one past it with a non-empty ledger", 454, 453, counting, false},
        {"no InstanceFinal yet", 454, std::nullopt, empty, false},
        {"a final index of -1", 0, -1, empty, true},
        {"a cursor that started past the final index", 500, 453, empty, true},
        {"the other instance's final, one apart", 453, 452, empty, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::drain_done(row.next, row.final_index, row.ledger, 0), row.done);
    }
}

TEST(SchedLifecycle, StartFindsAudio) {
    const int64_t last_ts = 21620 * MS;
    struct Row {
        const char* name;
        int64_t now;
        int64_t lead;
        std::optional<int64_t> last;
        bool finds;
    };
    const Row rows[] = {
        {"Control: well before", 20070500, 100 * MS, last_ts, true},
        {"now + lead at last_ts", 21520 * MS, 100 * MS, last_ts, true},
        {"one microsecond past", 21520 * MS + 1, 100 * MS, last_ts, false},
        {"a visualizer at last_ts", last_ts, 0, last_ts, true},
        {"no input", 0, 0, std::nullopt, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::start_finds_audio(row.now, row.lead, row.last), row.finds);
    }
    // An inactive role's drain is done once no start finds audio; an active one's by its cursor
    const Ledger ledger(100000, true);
    EXPECT_FALSE(
        sched::role_drain_done(false, 0, std::nullopt, ledger, 100 * MS, last_ts, 21520 * MS));
    EXPECT_TRUE(
        sched::role_drain_done(false, 0, std::nullopt, ledger, 100 * MS, last_ts, 21520 * MS + 1));
    EXPECT_FALSE(sched::role_drain_done(true, 453, 453, ledger, 100 * MS, std::nullopt, 0));
    EXPECT_TRUE(sched::role_drain_done(true, 454, 453, ledger, 100 * MS, std::nullopt, 0));
}

// A start during a drain: an inactive role with a pending trigger starts while a start finds
// audio, and gets nothing a microsecond later
TEST(SchedLifecycle, StartInADrainingEpoch) {
    const int64_t last_ts = 21620 * MS;
    const Ledger ledger(100000, true);
    for (const int64_t now : {21520 * MS, 21520 * MS + 1}) {
        SCOPED_TRACE(now);
        WantInputs in = wanted();
        in.epoch_draining = true;
        in.drain_done =
            sched::role_drain_done(false, 0, std::nullopt, ledger, 100 * MS, last_ts, now);
        const StreamAction action = sched::reconcile_stream({}, sched::evaluate_want(in), ALL_HOLD);
        EXPECT_EQ(action, now == 21520 * MS ? StreamAction::START : StreamAction::NONE);
    }
    // An active stream stays wanted until its drain is done, then ends
    WantInputs in = wanted();
    in.epoch_draining = true;
    in.drain_done = sched::role_drain_done(true, 453, 453, ledger, 0, last_ts, 0);
    EXPECT_EQ(sched::reconcile_stream(active(1, 2), sched::evaluate_want(in), ALL_HOLD),
              StreamAction::NONE);
    in.drain_done = sched::role_drain_done(true, 454, 453, ledger, 0, last_ts, 0);
    EXPECT_EQ(sched::reconcile_stream(active(1, 2), sched::evaluate_want(in), ALL_HOLD),
              StreamAction::END);
}

// The group's drain timer at 21520 finds streams active; the last end at 21630.295 completes the
// drain
TEST(SchedLifecycle, DrainComplete) {
    const int64_t j_min = sched::timing_defaults::JOIN_MIN_US;
    const GroupDrainView done{true, true, false, 21620 * MS};
    GroupDrainView streaming = done;
    streaming.any_stream_active = true;
    GroupDrainView paused = done;
    paused.run_playing = false;
    GroupDrainView not_draining = done;
    not_draining.epoch_draining = false;
    const GroupDrainView no_input{true, true, false, std::nullopt};
    struct Row {
        const char* name;
        GroupDrainView group;
        int64_t now;
        bool complete;
    };
    const Row rows[] = {
        {"Control: done", done, 21630295, true},
        {"a session still active", streaming, 21520 * MS, false},
        {"none active before last_ts - J_min", done, 21520 * MS - 1, false},
        {"none active at last_ts - J_min", done, 21520 * MS, true},
        {"a run ended by a pause", paused, 21630295, false},
        {"not draining", not_draining, 21630295, false},
        {"an epoch with no input completes at once", no_input, 0, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sched::drain_complete(row.group, j_min, row.now), row.complete);
    }
}

// Each player's last completion, from its ledger
TEST(SchedLifecycle, LastCompletions) {
    Ledger a(100000, true);
    a.add(21620 * MS + 10295, 5357);
    EXPECT_EQ(a.last_completion(), 21630295);
    Ledger b(32000, true);
    b.set_output_delay_us(50 * MS);
    b.add(21620 * MS + 10292, 5000);
    EXPECT_EQ(b.last_completion(), 21580292);
}

}  // namespace
