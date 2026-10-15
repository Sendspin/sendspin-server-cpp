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

/// @file test_sched_run_view.cpp
/// @brief The host view's run, latch and drain decisions

#include "sched/run_view.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>

namespace {

using sendspin::server::sched::DrainOutcome;
using sendspin::server::sched::DrainResolution;
using sendspin::server::sched::EpochId;
using sendspin::server::sched::RunId;
using sendspin::server::sched::RunState;
using sendspin::server::sched::RunView;
namespace sched = sendspin::server::sched;

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

}  // namespace
