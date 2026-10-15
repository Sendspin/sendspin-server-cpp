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

/// @file test_audio_workers_internals.cpp
/// @brief The group audio threads: a stop drops the jobs not started and ignores later wakes.
/// Compiled with -fno-access-control (tests/CMakeLists.txt).

#include "audio_workers.h"
#include "gate.h"
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using sendspin::server::AudioWorkers;
using sendspin::server::test::Gate;

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
    for (;;) {
        {
            const std::lock_guard<std::mutex> lock(workers->pool_->mutex);
            if (workers->pool_->stopping) {
                break;
            }
        }
        std::this_thread::yield();
    }
    gate.open();
    stopper.join();
    waiting->wake();
    EXPECT_EQ(waiting_runs.load(), 0);
    // A task the stop took off the queue can still be stopped
    waiting->stop();
    blocker->stop();
}

}  // namespace
