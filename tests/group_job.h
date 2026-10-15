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

/// @file group_job.h
/// @brief White-box: a group's pipeline job wrapped between two runs, and the pipelines it runs.
/// Only a test file compiled with -fno-access-control (tests/CMakeLists.txt) includes it.

#pragma once

#include "audio_workers.h"
#include "group.h"
#include "group_audio.h"

#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace sendspin::server::test {

/// @brief Once task is between runs, replaces its job with wrap(job), so the next wake runs the
/// wrapped job. Between runs means neither running nor queued (AudioWorkers::Task::running_ and
/// queued_, read under AudioWorkers::Pool::mutex), so a run already woken runs the job unwrapped.
/// Yields between tries.
/// @param wrap takes the job (Task::job_) and returns the job that replaces it
template <typename Wrap>
void wrap_job(AudioWorkers::Task& task, Wrap wrap) {
    for (;;) {
        {
            const std::lock_guard<std::mutex> lock(task.pool_->mutex);
            if (!task.running_ && !task.queued_) {
                task.job_ = wrap(std::move(task.job_));
                return;
            }
        }
        std::this_thread::yield();
    }
}

/// @brief The pipelines a group's job runs (Group::PipelineJob::audio), from wrap_job()'s wrap
inline std::shared_ptr<GroupAudio> pipelines_of(const std::function<bool()>& job) {
    return job.target<Group::PipelineJob>()->audio;
}

}  // namespace sendspin::server::test
