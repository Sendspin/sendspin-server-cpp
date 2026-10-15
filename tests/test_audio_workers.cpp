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

/// @file test_audio_workers.cpp
/// @brief The group audio threads: a task's jobs one at a time and in order, a wake during a job,
/// groups taking turns, a task stopped while its job runs, and a group erased on core while its
/// pipeline job runs. White-box: the last wraps the group's job (see tests/CMakeLists.txt).

#include "artwork_cache.h"
#include "audio_workers.h"
#include "clock.h"
#include "dispatcher.h"
#include "group.h"
#include "group_audio.h"
#include "host_view.h"
#include "test_recorder.h"
#include "transport.h"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sendspin::server::AudioWorkers;

/// The groups' artwork cache, which these tests never use: it refuses every job
sendspin::server::ArtworkCache unused_artwork() {
    return sendspin::server::ArtworkCache([](sendspin::server::ArtworkJob) { return false; });
}

/// A gate a job waits at until the test opens it, and which tells the test the job reached it
class Gate {
public:
    /// On the job's thread
    void pass() {
        this->reached_.set_value();
        this->open_.get_future().wait();
    }

    void wait_reached() {
        this->reached_.get_future().wait();
    }

    void open() {
        this->open_.set_value();
    }

private:
    std::promise<void> reached_;
    std::promise<void> open_;
};

// A wake while the job runs runs it once more after it returns, never alongside it; two wakes then
// are one more run, not two
TEST(AudioWorkers, RunsAWakeDuringAJobOnceAfterIt) {
    AudioWorkers workers(2);
    Gate gate;
    std::promise<void> second;
    std::atomic<int> runs{0};
    std::atomic<int> inside{0};
    std::atomic<int> most_inside{0};
    const std::shared_ptr<AudioWorkers::Task> task = workers.add([&] {
        most_inside = std::max(most_inside.load(), ++inside);
        const int run = ++runs;
        if (run == 1) {
            gate.pass();
        } else if (run == 2) {
            second.set_value();
        }
        --inside;
        return false;
    });
    task->wake();
    gate.wait_reached();
    task->wake();
    task->wake();
    gate.open();
    second.get_future().wait();
    workers.stop();
    EXPECT_EQ(runs.load(), 2);
    EXPECT_EQ(most_inside.load(), 1);
    // Control: a task woken once runs once
    AudioWorkers more(1);
    std::promise<void> ran;
    std::atomic<int> once{0};
    const std::shared_ptr<AudioWorkers::Task> single = more.add([&] {
        if (++once == 1) {
            ran.set_value();
        }
        return false;
    });
    single->wake();
    ran.get_future().wait();
    more.stop();
    EXPECT_EQ(once.load(), 1);
}

// A job that reports more work runs again with no wake; one that reports none waits for a wake
TEST(AudioWorkers, RunsAJobAgainWhileItReportsMore) {
    AudioWorkers workers(1);
    std::atomic<int> runs{0};
    std::promise<void> done;
    std::promise<void> woken;
    const std::shared_ptr<AudioWorkers::Task> task = workers.add([&] {
        const int run = ++runs;
        if (run == 5) {
            done.set_value();
        } else if (run == 6) {
            woken.set_value();
        }
        return run < 5;
    });
    task->wake();
    done.get_future().wait();
    task->wake();
    woken.get_future().wait();
    workers.stop();
    EXPECT_EQ(runs.load(), 6);
}

// A group whose job always reports more work goes to the back of the queue, so another group's job
// runs on the one thread in between
TEST(AudioWorkers, LetsGroupsTakeTurns) {
    AudioWorkers workers(1);
    std::atomic<bool> other_ran{false};
    std::atomic<int> busy_runs{0};
    const std::shared_ptr<AudioWorkers::Task> busy = workers.add([&] {
        ++busy_runs;
        return !other_ran.load();
    });
    std::promise<void> ran;
    const std::shared_ptr<AudioWorkers::Task> other = workers.add([&] {
        other_ran = true;
        ran.set_value();
        return false;
    });
    busy->wake();
    other->wake();
    // Never ends if the busy group keeps the thread
    ran.get_future().wait();
    workers.stop();
    EXPECT_GE(busy_runs.load(), 1);
}

// A task stopped while its job runs does not wait for it; the job finishes, is destroyed on its
// thread, and never runs again, even when woken
TEST(AudioWorkers, StopsATaskWhileItsJobRuns) {
    AudioWorkers workers(1);
    Gate gate;
    std::atomic<int> runs{0};
    auto held = std::make_shared<int>(0);
    const std::weak_ptr<int> job_state = held;
    const std::shared_ptr<AudioWorkers::Task> task = workers.add([&, held = std::move(held)] {
        ++runs;
        gate.pass();
        return true;
    });
    task->wake();
    gate.wait_reached();
    // Returns while the job still waits at the gate: a stop that waited would never return
    task->stop();
    task->wake();
    EXPECT_FALSE(job_state.expired());
    gate.open();
    workers.stop();
    EXPECT_EQ(runs.load(), 1);
    EXPECT_TRUE(job_state.expired());
}

// Stopping the threads drops a job not started and ignores later wakes, and its task can still
// be stopped. White-box: the running job is let go only once the stop has begun, which nothing
// else shows.
TEST(AudioWorkers, DropsQueuedJobsWhenStopped) {
    auto workers = std::make_unique<AudioWorkers>(1);
    Gate gate;
    std::atomic<int> waiting_runs{0};
    const std::shared_ptr<AudioWorkers::Task> blocker = workers->add([&] {
        gate.pass();
        return false;
    });
    const std::shared_ptr<AudioWorkers::Task> waiting = workers->add([&] {
        ++waiting_runs;
        return false;
    });
    blocker->wake();
    gate.wait_reached();
    waiting->wake();
    std::thread stopper([&] { workers->stop(); });
    for (bool stopping = false; !stopping;) {
        const std::lock_guard<std::mutex> lock(workers->pool_->mutex);
        stopping = workers->pool_->stopping;
    }
    gate.open();
    stopper.join();
    waiting->wake();
    EXPECT_EQ(waiting_runs.load(), 0);
    // A task the stop took off the queue can still be stopped
    waiting->stop();
    blocker->stop();
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
    for (bool wrapped = false; !wrapped;) {
        const std::lock_guard<std::mutex> lock(task->pool_->mutex);
        if (task->running_ || task->queued_) {
            continue;
        }
        task->job_ = [inner = std::move(task->job_), &gate, &gated_runs]() {
            const bool more = inner();
            if (++gated_runs == 1) {
                gate.pass();
            }
            return more;
        };
        wrapped = true;
    }
    // Core waits, then erases the group, so the FIFO handler the job posts runs after the erase
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
    // The FIFO handler the job posted has run once a handler posted now runs, while the job,
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
        for (bool wrapped = false; !wrapped;) {
            const std::lock_guard<std::mutex> lock(task->pool_->mutex);
            if (task->running_ || task->queued_) {
                continue;
            }
            task->job_ = [inner = std::move(task->job_), &ran, &runs]() {
                const bool more = inner();
                if (++runs == 1) {
                    ran.set_value();
                }
                return more;
            };
            wrapped = true;
        }
        on_core(loop, [] {});
        // Core waits until the release, so the write's wakeup and the FIFO handler the job posts
        // run after it, in that order
        std::promise<void> core_gate;
        if (queued) {
            loop.post([gate = core_gate.get_future().share()] { gate.wait(); });
        }
        // A write anchors a run, so the job posts its start
        const std::vector<uint8_t> pcm(size_t{960} * 4, 1);
        ASSERT_EQ(view->write(pcm.data(), 960).frames, 960U);
        ran.get_future().wait();
        // The FIFO handler, posted before the job returned, has run once a later handler runs
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
        // Core handles the release's wakeup, then the FIFO handler, then this
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

// A group counts the rebases its pipelines post in its epoch, with their sum, and each player frame
// that reaches its ring at or past its play time, once per instance; not a frame still ahead of its
// time, one of another epoch, or an analysis's cell (which skips its ring instead). One report
// carries them all and starts the count again.
TEST(GroupStalls, CountsRebasesAndPlayerFramesPastTheirPlayTime) {
    using sendspin::server::AnchorShift;
    StallGroup s;
    sendspin::server::Group& group = *s.group;
    const int64_t now = sendspin::server::now_us();
    // A report just made holds the counts back
    group.stalls_.reported_us = now;

    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1000);
    s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now + 10'000'000);
    s.frame(StallGroup::PLAYER, sendspin::server::sched::EpochId{2}, now - 1);
    s.frame(StallGroup::ANALYSIS, StallGroup::EPOCH, now - 1);
    group.on_anchor_shift(AnchorShift{StallGroup::EPOCH, 10, 300'000});
    group.on_anchor_shift(AnchorShift{StallGroup::EPOCH, 20, 200'000});
    group.on_anchor_shift(AnchorShift{sendspin::server::sched::EpochId{2}, 30, 900'000});
    EXPECT_TRUE(s.taken().empty());

    group.report_stalls(now + sendspin::server::Group::Stalls::STALL_REPORT_US);
    const std::vector<std::string> lines = s.taken();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("group Kitchen: 2 rebases (500 ms in all) and 2 frames ready after "
                            "their play time"),
              std::string::npos)
        << lines[0];
    EXPECT_EQ(group.stalls_.rebases, 0U);
    EXPECT_EQ(group.stalls_.late_frames, 0U);
}

// A stall with no report in the last STALL_REPORT_US is reported at once; within it, it is reported
// once the period ends, by the group's timer when no stall follows, and a report with nothing
// counted logs nothing
TEST(GroupStalls, ReportsAtMostEveryPeriodAndHoldsNothingBack) {
    StallGroup s;
    sendspin::server::Group& group = *s.group;
    std::thread core([&] { s.loop.run(); });
    const int64_t now = sendspin::server::now_us();
    std::promise<void> counted;
    s.loop.post([&] {
        s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
        // White-box: the last report, so the next is due 100 ms from now
        group.stalls_.reported_us =
            sendspin::server::now_us() + 100'000 - sendspin::server::Group::Stalls::STALL_REPORT_US;
        s.frame(StallGroup::PLAYER, StallGroup::EPOCH, now - 1);
        counted.set_value();
    });
    counted.get_future().get();
    std::vector<std::string> lines = s.taken();
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("0 rebases (0 ms in all) and 1 frames ready"), std::string::npos)
        << lines[0];

    // The held-back frame, by the timer; a regression hangs into the suite watchdog
    lines.clear();
    while (lines.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        lines = s.taken();
    }
    ASSERT_EQ(lines.size(), 1U);
    EXPECT_NE(lines[0].find("0 rebases (0 ms in all) and 1 frames ready"), std::string::npos)
        << lines[0];

    std::promise<void> reported;
    s.loop.post([&] {
        group.report_stalls(sendspin::server::now_us() +
                            (2 * sendspin::server::Group::Stalls::STALL_REPORT_US));
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

}  // namespace
