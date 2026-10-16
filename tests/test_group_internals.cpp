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

/// @file test_group_internals.cpp
/// @brief A group on core with its pipeline job on the group audio threads: a group erased on
/// core while its job runs, an input released while its epoch start is queued, a full ring
/// answered once the job frees it, a slew freed on core, a group's stall counts and reports, and
/// the rings of an instance whose payload slots are all taken.
/// Compiled with -fno-access-control (tests/CMakeLists.txt).

#include "artwork_cache.h"
#include "audio_workers.h"
#include "clock.h"
#include "dispatcher.h"
#include "gate.h"
#include "group.h"
#include "group_audio.h"
#include "group_job.h"
#include "host_view.h"
#include "sched/stalls.h"
#include "test_recorder.h"
#include "transport.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sendspin::server::AudioWorkers;
using sendspin::server::test::Gate;
using sendspin::server::test::wrap_job;

/// The groups' artwork cache, which these tests never use: it refuses every job
sendspin::server::ArtworkCache unused_artwork() {
    return sendspin::server::ArtworkCache([](sendspin::server::ArtworkJob) { return false; });
}

// ==============================================================================
// A group erased on core while its pipeline job runs
// ==============================================================================

// docs/conventions.md "Teardown discards": core erases a group without waiting for the job running
// on a group audio thread; what the job then posts reaches no group, and the job never runs again
TEST(GroupAudioThread, ErasesAGroupWhileItsJobRuns) {
    using sendspin::server::Group;
    sendspin::server::EventLoop loop;
    AudioWorkers workers(1);
    std::thread core([&] { loop.run(); });
    uint64_t next_instance = 1;
    sendspin::server::ArtworkCache artwork = unused_artwork();
    std::unique_ptr<Group> group;
    const auto view = std::make_shared<sendspin::server::HostView>(
        sendspin::server::GroupId{1}, sendspin::server::InputId{1},
        sendspin::server::InputOptions{
            sendspin::server::InputFormat{48000, 2, sendspin::server::SampleFormat::S16}},
        sendspin::server::GroupParams{300000, 0, 1000000},
        std::make_shared<sendspin::server::Dispatcher>(), nullptr);
    std::promise<std::shared_ptr<AudioWorkers::Task>> attached;
    loop.post([&] {
        group = std::make_unique<Group>(
            loop, workers, sendspin::server::GroupId{1}, "Erased", next_instance,
            sendspin::server::ResamplerQuality::MEDIUM, false,
            sendspin::server::GroupHooks{[] {}, [](const sendspin::server::ServerError&) {}, [] {},
                                         [](const std::string&) {}},
            artwork);
        EXPECT_TRUE(group->attach_input(view));
        attached.set_value(group->audio_);
    });
    const std::shared_ptr<AudioWorkers::Task> task = attached.get_future().get();

    // White-box: the job's next run posts what it posts, then waits at a gate, still running; put
    // in place once the run the attachment woke is over, so the write below wakes the next
    Gate gate;
    std::atomic<int> gated_runs{0};
    wrap_job(*task, [&gate, &gated_runs](std::function<bool()> inner) {
        return [inner = std::move(inner), &gate, &gated_runs]() {
            const bool more = inner();
            if (++gated_runs == 1) {
                gate.pass();
            }
            return more;
        };
    });
    // Core waits, then erases the group, so the FIFO's run the job signals comes after the erase
    std::promise<void> core_gate;
    std::promise<void> erased;
    loop.post([&] {
        core_gate.get_future().wait();
        group.reset();
        erased.set_value();
    });
    // A write anchors a run, so the job posts its start
    const std::vector<uint8_t> pcm(size_t{960} * 4, 1);
    ASSERT_EQ(view->write(pcm.data(), 960).frames, 960U);
    gate.wait_reached();
    core_gate.set_value();
    erased.get_future().wait();
    // The FIFO's run the job signalled has run once a handler posted now runs, while the job,
    // still at its gate, holds the FIFO's state
    std::promise<void> handled;
    loop.post([&] { handled.set_value(); });
    handled.get_future().wait();
    // The group closed the input on its way out
    EXPECT_EQ(view->write(pcm.data(), 960).state, sendspin::server::InputState::CLOSED);
    // The one thread runs tasks in the order woken: once a probe task has run behind the job, the
    // job has returned; a wake then queues the job ahead of the probe's next run, unless stopped
    std::promise<void> probed;
    std::promise<void> probed_again;
    std::atomic<int> probes{0};
    const std::shared_ptr<AudioWorkers::Task> probe = workers.add([&] {
        (++probes == 1 ? probed : probed_again).set_value();
        return false;
    });
    probe->wake();
    gate.open();
    probed.get_future().wait();
    task->wake();
    probe->wake();
    probed_again.get_future().wait();
    EXPECT_EQ(gated_runs.load(), 1);
    workers.stop();
    loop.post([&] { loop.shutdown(0); });
    core.join();
}

/// Runs a handler on core and waits for it
template <typename F>
auto on_core(sendspin::server::EventLoop& loop, F fn) {
    std::promise<decltype(fn())> done;
    loop.post([&] {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            done.set_value();
        } else {
            done.set_value(fn());
        }
    });
    return done.get_future().get();
}

// When the host releases its input, core ends a playing run itself and detaches the input: an
// EpochStarted the stopped job posted before the release, still in the FIFO when core sees the
// release, never starts a run with no input. Control: an EpochStarted delivered before the release
// starts the run, which the release ends.
TEST(GroupAudioThread, ReleasesAnInputWhoseEpochStartIsStillQueued) {
    using sendspin::server::Group;
    for (const bool queued : {true, false}) {
        SCOPED_TRACE(queued ? "start queued" : "control: start delivered");
        sendspin::server::EventLoop loop;
        AudioWorkers workers(1);
        std::thread core([&] { loop.run(); });
        uint64_t next_instance = 1;
        sendspin::server::ArtworkCache artwork = unused_artwork();
        std::unique_ptr<Group> group;
        const auto view = std::make_shared<sendspin::server::HostView>(
            sendspin::server::GroupId{1}, sendspin::server::InputId{1},
            sendspin::server::InputOptions{
                sendspin::server::InputFormat{48000, 2, sendspin::server::SampleFormat::S16}},
            sendspin::server::GroupParams{300000, 0, 1000000},
            std::make_shared<sendspin::server::Dispatcher>(), nullptr);
        const std::shared_ptr<AudioWorkers::Task> task = on_core(loop, [&] {
            group = std::make_unique<Group>(
                loop, workers, sendspin::server::GroupId{1}, "Released", next_instance,
                sendspin::server::ResamplerQuality::MEDIUM, false,
                sendspin::server::GroupHooks{[] {}, [](const sendspin::server::ServerError&) {},
                                             [] {}, [](const std::string&) {}},
                artwork);
            EXPECT_TRUE(group->attach_input(view));
            return group->audio_;
        });
        // White-box: the test learns when the job's next run, which the write below wakes, is over
        std::promise<void> ran;
        std::atomic<int> runs{0};
        wrap_job(*task, [&ran, &runs](std::function<bool()> inner) {
            return [inner = std::move(inner), &ran, &runs]() {
                const bool more = inner();
                if (++runs == 1) {
                    ran.set_value();
                }
                return more;
            };
        });
        on_core(loop, [] {});
        // Core waits until the release, so the write's wakeup and the FIFO's run the job signals
        // come after it, in that order
        std::promise<void> core_gate;
        if (queued) {
            loop.post([gate = core_gate.get_future().share()] { gate.wait(); });
        }
        // A write anchors a run, so the job posts its start
        const std::vector<uint8_t> pcm(size_t{960} * 4, 1);
        ASSERT_EQ(view->write(pcm.data(), 960).frames, 960U);
        ran.get_future().wait();
        // The FIFO's run, signalled before the job returned, has run once a later handler runs
        if (!queued) {
            EXPECT_TRUE(on_core(loop, [&] { return group->playing_; }));
        }
        // The one thread is busy, so the job the release wakes runs only after core saw it
        std::promise<void> busy;
        std::promise<void> go;
        const std::shared_ptr<AudioWorkers::Task> blocker =
            workers.add([&busy, gate = go.get_future().share()] {
                busy.set_value();
                gate.wait();
                return false;
            });
        blocker->wake();
        busy.get_future().wait();
        view->release();
        core_gate.set_value();
        // Core handles the release's wakeup, then the FIFO's run, then this
        on_core(loop, [] {});
        const auto [playing, attached] =
            on_core(loop, [&] { return std::pair{group->playing_, group->input_ != nullptr}; });
        EXPECT_FALSE(playing);
        EXPECT_FALSE(attached);
        go.set_value();
        on_core(loop, [&] { group.reset(); });
        workers.stop();
        loop.post([&] { loop.shutdown(0); });
        core.join();
    }
}

// A paused live input takes every write the staging ring has room for, and its pipelines' job frees
// the ring; core polls for room once the job has run, so on_writable answers a write the full ring
// cut short even when core polled before the job freed it
TEST(GroupAudioThread, AnswersAFullRingOnceTheJobFreesIt) {
    using sendspin::server::Group;
    constexpr uint32_t RATE = 48000;
    sendspin::server::test::Recorder recorder;
    const auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
    sendspin::server::EventLoop loop;
    AudioWorkers workers(1);
    std::thread core([&] { loop.run(); });
    uint64_t next_instance = 1;
    sendspin::server::ArtworkCache artwork = unused_artwork();
    std::unique_ptr<Group> group;
    sendspin::server::InputOptions options{{RATE, 2, sendspin::server::SampleFormat::S16}};
    options.kind = sendspin::server::InputKind::LIVE;
    const auto view = std::make_shared<sendspin::server::HostView>(
        sendspin::server::GroupId{1}, sendspin::server::InputId{1}, options,
        sendspin::server::GroupParams{300000, 0, 200000}, dispatcher, &recorder);
    const std::shared_ptr<AudioWorkers::Task> task = on_core(loop, [&] {
        group = std::make_unique<Group>(
            loop, workers, sendspin::server::GroupId{1}, "Paused", next_instance,
            sendspin::server::ResamplerQuality::MEDIUM, false,
            sendspin::server::GroupHooks{[] {}, [](const sendspin::server::ServerError&) {}, [] {},
                                         [](const std::string&) {}},
            artwork);
        EXPECT_TRUE(group->attach_input(view));
        return group->audio_;
    });
    // More than the ring holds, paused
    const std::vector<uint8_t> pcm(size_t{13} * RATE * 4, 1);
    ASSERT_GT(view->write(pcm.data(), RATE / 10).frames, 0U);
    view->pause();
    // The one thread is busy, so the job the full write wakes runs after core polled
    std::promise<void> busy;
    std::promise<void> go;
    const std::shared_ptr<AudioWorkers::Task> blocker =
        workers.add([&busy, gate = go.get_future().share()] {
            busy.set_value();
            gate.wait();
            return false;
        });
    blocker->wake();
    busy.get_future().wait();
    const sendspin::server::WriteResult full = view->write(pcm.data(), size_t{13} * RATE);
    ASSERT_EQ(full.state, sendspin::server::InputState::FULL);
    ASSERT_GT(full.frames, 0U);
    // Core handled the write's wakeup: the ring has no room yet, and no time gives it any
    EXPECT_EQ(on_core(loop, [&] { return view->poll_writable(sendspin::server::now_us()); }),
              sendspin::server::sched::NEVER);
    go.set_value();
    // Never ends unless core polls once the job freed the ring
    recorder.wait_for("writable 1 1");
    EXPECT_EQ(view->state(), sendspin::server::InputState::OPEN);
    on_core(loop, [&] { group.reset(); });
    workers.stop();
    loop.post([&] { loop.shutdown(0); });
    core.join();
    dispatcher->finish();
    dispatcher->join();
}

// The write that takes up set_format()'s slew leaves the slew it replaced in the view, and core
// frees it as it handles the group's wakeup. The pipeline jobs signal that wakeup too, so a write's
// signal can fold into one whose handler is not posted yet: the test runs the handler itself.
// White-box: Group::on_wake(), HostView::slew_, next_slew_, mutex_.
TEST(GroupAudioThread, FreesTheSlewAWriteReplacedOnCore) {
    using sendspin::server::Group;
    constexpr uint32_t RATE = 48000;
    sendspin::server::test::Recorder recorder;
    const auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
    sendspin::server::EventLoop loop;
    AudioWorkers workers(1);
    std::thread core([&] { loop.run(); });
    uint64_t next_instance = 1;
    sendspin::server::ArtworkCache artwork = unused_artwork();
    std::unique_ptr<Group> group;
    sendspin::server::InputOptions options{{RATE, 2, sendspin::server::SampleFormat::S16}};
    options.kind = sendspin::server::InputKind::LIVE;
    const auto view = std::make_shared<sendspin::server::HostView>(
        sendspin::server::GroupId{1}, sendspin::server::InputId{1}, options,
        sendspin::server::GroupParams{300000, 0, 200000}, dispatcher, &recorder);
    on_core(loop, [&] {
        group = std::make_unique<Group>(
            loop, workers, sendspin::server::GroupId{1}, "Live", next_instance,
            sendspin::server::ResamplerQuality::MEDIUM, false,
            sendspin::server::GroupHooks{[] {}, [](const sendspin::server::ServerError&) {}, [] {},
                                         [](const std::string&) {}},
            artwork);
        EXPECT_TRUE(group->attach_input(view));
    });
    // A cell in either format
    const std::vector<uint8_t> pcm(size_t{RATE} / 50 * 8, 0);
    ASSERT_GT(view->write(pcm.data(), RATE / 50).frames, 0U);
    ASSERT_TRUE(view->set_format({RATE, 2, sendspin::server::SampleFormat::F32}));
    const uint8_t* made = [&] {
        const std::lock_guard<std::mutex> lock(view->mutex_);
        return view->next_slew_.output();
    }();
    ASSERT_GT(view->write(pcm.data(), RATE / 50).frames, 0U);
    const auto [adopted, retired] = on_core(loop, [&] {
        group->on_wake();
        const std::lock_guard<std::mutex> lock(view->mutex_);
        return std::pair{view->slew_.output() == made, view->next_slew_.ready()};
    });
    EXPECT_TRUE(adopted);
    EXPECT_FALSE(retired);
    on_core(loop, [&] { group.reset(); });
    workers.stop();
    loop.post([&] { loop.shutdown(0); });
    core.join();
    dispatcher->finish();
    dispatcher->join();
}

/// A group with no input, playing epoch 1 on its own anchor, with a frame ring for instance 7 and
/// an analysis on instance 8, and its INFO lines in lines. White-box: it sets the state an
/// epoch's start sets (playing_, epoch_, anchor_, rings_, pipelines_.analyses).
struct StallGroup {
    sendspin::server::EventLoop loop;
    AudioWorkers workers{1};
    uint64_t next_instance = 1;
    sendspin::server::ArtworkCache artwork = unused_artwork();
    std::mutex mutex;
    std::vector<std::string> lines;
    std::unique_ptr<sendspin::server::Group> group;

    static constexpr sendspin::server::sched::EpochId EPOCH{1};
    static constexpr sendspin::server::sched::InstanceId PLAYER{7};
    static constexpr sendspin::server::sched::InstanceId ANALYSIS{8};

    StallGroup() {
        sendspin::server::GroupHooks hooks{[] {}, [](const sendspin::server::ServerError&) {},
                                           [] {}, [](const std::string&) {}};
        hooks.info = [this](std::string line) {
            const std::lock_guard<std::mutex> lock(this->mutex);
            this->lines.push_back(std::move(line));
        };
        this->group = std::make_unique<sendspin::server::Group>(
            this->loop, this->workers, sendspin::server::GroupId{1}, "Kitchen", this->next_instance,
            sendspin::server::ResamplerQuality::MEDIUM, false, std::move(hooks), this->artwork);
        sendspin::server::Group& g = *this->group;
        g.playing_ = true;
        g.epoch_ = EPOCH;
        g.anchor_ = sendspin::server::sched::make_anchor(0, 0, 48000, 2);
        g.rings_.emplace(PLAYER, sendspin::server::sched::FrameRing(PLAYER, 1));
        g.rings_.emplace(ANALYSIS, sendspin::server::sched::FrameRing(ANALYSIS, 1));
        g.pipelines_.analyses.push_back(sendspin::server::AnalysisRequest{ANALYSIS, EPOCH, {}, 0});
    }

    void frame(sendspin::server::sched::InstanceId instance, sendspin::server::sched::EpochId epoch,
               int64_t ts_us) {
        sendspin::server::sched::Frame frame;
        frame.epoch = epoch;
        frame.ts_us = ts_us;
        sendspin::server::FrameReady ready{instance, std::move(frame)};
        this->group->on_frame(ready);
    }

    std::vector<std::string> taken() {
        const std::lock_guard<std::mutex> lock(this->mutex);
        return std::exchange(this->lines, {});
    }
};

// A group counts the rebases its pipelines post in its epoch, with their shifts, and each player
// frame that reaches its ring at or past its play time, once per instance; not a frame still ahead
// of its time, one of another epoch, or an analysis's cell (which skips its ring instead). The
// counts are read from Group::stalls_, as the log line is the only other place they show.
TEST(GroupStalls, CountsRebasesAndPlayerFramesPastTheirPlayTime) {
    using sendspin::server::AnchorShift;
    using sendspin::server::sched::STALL_REPORT_US;
    StallGroup s;
    sendspin::server::Group& group = *s.group;
    const int64_t now = sendspin::server::now_us();
    // A first stall is reported at once, which holds back the counts that follow
    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
    ASSERT_EQ(s.taken().size(), 1U);

    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1000);
    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now + 10'000'000);
    s.frame(StallGroup::PLAYER, sendspin::server::sched::EpochId{2}, now - 1);
    s.frame(StallGroup::ANALYSIS, StallGroup::EPOCH, now - 1);
    group.on_anchor_shift(AnchorShift{StallGroup::EPOCH, 10, 300'000});
    group.on_anchor_shift(AnchorShift{StallGroup::EPOCH, 20, 200'000});
    group.on_anchor_shift(AnchorShift{sendspin::server::sched::EpochId{2}, 30, 900'000});
    EXPECT_TRUE(s.taken().empty());

    const std::optional<sendspin::server::sched::StallReport> report =
        group.stalls_.take(sendspin::server::now_us() + STALL_REPORT_US).report;
    ASSERT_TRUE(report);
    EXPECT_EQ(report->rebases, 2U);
    EXPECT_EQ(report->shifted_us, 500'000);
    EXPECT_EQ(report->late_frames, 2U);
}

// A stall with no report made (here a rebase) is logged at once; within STALL_REPORT_US of one it
// is logged once the period ends, by the group's timer when no stall follows, and a report with
// nothing counted logs nothing
TEST(GroupStalls, ReportsAtMostEveryPeriodAndHoldsNothingBack) {
    using sendspin::server::sched::STALL_REPORT_US;
    StallGroup s;
    sendspin::server::Group& group = *s.group;
    std::thread core([&] { s.loop.run(); });
    const int64_t now = sendspin::server::now_us();
    std::promise<void> counted;
    s.loop.post([&] {
        group.on_anchor_shift(sendspin::server::AnchorShift{StallGroup::EPOCH, 10, 300'000});
        // White-box: the last report replaced by one made so that the next is due 100 ms from now
        group.stalls_ = sendspin::server::sched::StallCount{};
        group.stalls_.add_late_frame();
        (void)group.stalls_.take(sendspin::server::now_us() + 100'000 - STALL_REPORT_US);
        s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
        counted.set_value();
    });
    counted.get_future().get();
    EXPECT_EQ(s.taken().size(), 1U);

    // The held-back frame, by the timer; a regression hangs into the suite watchdog
    std::vector<std::string> lines;
    while (lines.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lines = s.taken();
    }
    EXPECT_EQ(lines.size(), 1U);

    std::promise<void> reported;
    s.loop.post([&] {
        group.report_stalls(sendspin::server::now_us() + (2 * STALL_REPORT_US));
        reported.set_value();
    });
    reported.get_future().get();
    EXPECT_TRUE(s.taken().empty());
    s.loop.post([&] {
        s.group.reset();
        s.loop.shutdown(0);
    });
    core.join();
}

// ==============================================================================
// An instance whose payload slots are all taken
// ==============================================================================

// Core holds a full instance until its ring holds fewer frames than its slots, then wakes the
// pipelines: at once for a ring empty or pruned below them, and by its timer at the earliest
// frame's time when every frame is still ahead; an instance with no ring is let go without a wake.
// White-box: the group's pipeline task (Group::audio_) replaced with one that counts its runs, its
// rings_ filled directly, on_payloads_full() called as the group-to-core FIFO does, and full_ read
// on core.
TEST(GroupPayloads, WakesAFullInstanceOnceItsRingHoldsFewerFramesThanItsSlots) {
    using sendspin::server::PayloadsFull;
    namespace sched = sendspin::server::sched;
    constexpr size_t SLOTS = 2;
    StallGroup s;
    sendspin::server::Group& group = *s.group;
    std::atomic<int> runs{0};
    group.audio_ = s.workers.add([&runs] {
        ++runs;
        return false;
    });
    std::thread core([&] { s.loop.run(); });
    const auto wait_for_runs = [&](int count) {
        // A regression hangs into the suite watchdog
        while (runs.load() < count) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    sched::FrameRing& ring = group.rings_.at(StallGroup::PLAYER);
    // Two frames, the first at first_us; on core
    const auto hold = [&](int64_t first_us, int64_t second_us) {
        ring.restart(StallGroup::EPOCH, 0);
        const int64_t now = sendspin::server::now_us();
        for (int64_t index = 0; index < 2; ++index) {
            sched::Frame frame;
            frame.epoch = StallGroup::EPOCH;
            frame.index = index;
            frame.ts_us = index == 0 ? first_us : second_us;
            EXPECT_TRUE(ring.append(std::move(frame), now));
        }
    };

    // No ring: let go. The one worker runs tasks in the order woken, so once a probe woken after
    // has run, a wake would have run too.
    on_core(s.loop, [&] {
        group.on_payloads_full(PayloadsFull{sched::InstanceId{99}, SLOTS});
        EXPECT_TRUE(group.full_.empty());
    });
    std::promise<void> probed;
    const std::shared_ptr<AudioWorkers::Task> probe = s.workers.add([&probed] {
        probed.set_value();
        return false;
    });
    probe->wake();
    probed.get_future().wait();
    EXPECT_EQ(runs.load(), 0);

    // An empty ring holds fewer frames than its slots
    on_core(s.loop, [&] {
        group.on_payloads_full(PayloadsFull{StallGroup::ANALYSIS, SLOTS});
        EXPECT_TRUE(group.full_.empty());
    });
    wait_for_runs(1);

    // One frame due and one ahead: the due one is pruned
    const int64_t now = sendspin::server::now_us();
    on_core(s.loop, [&] { hold(now + 1, now + 10'000'000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    on_core(s.loop, [&] {
        group.on_payloads_full(PayloadsFull{StallGroup::PLAYER, SLOTS});
        EXPECT_TRUE(group.full_.empty());
        EXPECT_EQ(ring.size(), 1U);
    });
    wait_for_runs(2);

    // Every frame ahead: held, nothing pruned, until the timer at the earliest one
    on_core(s.loop, [&] {
        const int64_t at = sendspin::server::now_us();
        hold(at + 50'000, at + 10'000'000);
        group.on_payloads_full(PayloadsFull{StallGroup::PLAYER, SLOTS});
        ASSERT_EQ(group.full_.size(), 1U);
        EXPECT_EQ(ring.size(), 2U);
        EXPECT_EQ(runs.load(), 2);
    });
    wait_for_runs(3);
    on_core(s.loop, [&] {
        EXPECT_TRUE(group.full_.empty());
        EXPECT_EQ(ring.size(), 1U);
    });

    s.loop.post([&] {
        s.group.reset();
        s.loop.shutdown(0);
    });
    core.join();
}

// A full instance whose frames are all far ahead is let go as soon as its ring restarts and so
// returns every slot: at a flush's cut, an epoch's start or the run's end. White-box: rings_ filled
// directly, on_payloads_full() and the restarts called as the FIFO does, and full_ read; the run's
// end with no pipelines (pipelines_), so its recompute keeps the rings.
TEST(GroupPayloads, LetsAFullInstanceGoWhenItsRingRestarts) {
    using sendspin::server::PayloadsFull;
    namespace sched = sendspin::server::sched;
    struct Row {
        const char* name;
        void (*restart)(sendspin::server::Group&);
        bool held;
    };
    const Row rows[] = {
        {"Control: no restart", [](sendspin::server::Group&) {}, true},
        {"a flush's cut",
         [](sendspin::server::Group& group) {
             group.on_epoch_cut(sendspin::server::EpochCut{sched::EpochId{2}});
         },
         false},
        {"an epoch's start",
         [](sendspin::server::Group& group) {
             sendspin::server::EpochStarted started{sched::RunId{1}, sched::EpochId{2},
                                                    sched::make_anchor(0, 0, 48000, 2),
                                                    std::nullopt};
             group.on_epoch_started(started);
         },
         false},
        {"the run's end",
         [](sendspin::server::Group& group) {
             group.pipelines_ = {};
             group.end_run(StallGroup::EPOCH);
         },
         false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        StallGroup s;
        sendspin::server::Group& group = *s.group;
        sched::FrameRing& ring = group.rings_.at(StallGroup::PLAYER);
        ring.restart(StallGroup::EPOCH, 0);
        const int64_t now = sendspin::server::now_us();
        for (int64_t index = 0; index < 2; ++index) {
            sched::Frame frame;
            frame.epoch = StallGroup::EPOCH;
            frame.index = index;
            frame.ts_us = now + ((index + 1) * 10'000'000);
            ASSERT_TRUE(ring.append(std::move(frame), now));
        }
        group.on_payloads_full(PayloadsFull{StallGroup::PLAYER, 2});
        ASSERT_EQ(group.full_.size(), 1U);
        row.restart(group);
        EXPECT_EQ(group.rings_.size(), 2U);
        EXPECT_EQ(group.full_.size(), row.held ? 1U : 0U);
    }
}

}  // namespace
