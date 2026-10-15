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
/// groups taking turns and a task stopped while its job runs.

#include "audio_workers.h"
#include "gate.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <memory>

namespace {

using sendspin::server::AudioWorkers;
using sendspin::server::test::Gate;

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

}  // namespace
