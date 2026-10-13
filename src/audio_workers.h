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

/// @file audio_workers.h
/// @brief The group audio threads: a fixed pool of threads that runs each group's pipeline jobs off
/// the core thread. A group's jobs run one at a time, in order, on whichever thread takes them;
/// groups that have work share the threads round robin, one job per turn.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sendspin::server {

class AudioWorkers {
public:
    /// @brief The threads a run starts: one per core, at most MAX_THREADS
    static constexpr size_t MAX_THREADS = 4;

    static size_t default_threads();

    class Task;

    /// @param threads at least one
    explicit AudioWorkers(size_t threads);
    /// @brief stop(); never from a worker
    ~AudioWorkers();
    AudioWorkers(const AudioWorkers&) = delete;
    AudioWorkers& operator=(const AudioWorkers&) = delete;

    /// @brief A group's jobs; none runs until Task::wake(). Any thread.
    /// @param job one pipeline job, on a worker and never two of one task at once
    /// @return job's return: another job could produce more now, so the task runs again
    std::shared_ptr<Task> add(std::function<bool()> job);

    /// @brief Ends the threads once each has finished the job it runs, and waits for them; a job
    /// not started is never run, and a task woken from now on does nothing. Any thread but a
    /// worker.
    void stop();

private:
    /// The run queue: an intrusive list through Task::next_, so a wake never allocates
    struct Pool {
        std::mutex mutex;
        std::condition_variable work;
        std::shared_ptr<Task> head;
        Task* tail{nullptr};
        bool stopping{false};
    };

    void run();

    std::shared_ptr<Pool> pool_;
    std::vector<std::thread> threads_;
};

/// @brief One group's jobs on the pool
class AudioWorkers::Task : public std::enable_shared_from_this<Task> {
public:
    explicit Task(std::shared_ptr<Pool> pool, std::function<bool()> job);
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /// @brief Runs the job on a worker soon, or once more after the job running now. Any thread;
    /// never allocates and never waits for a job, so GroupInput::write() may call it.
    void wake();

    /// @brief No job starts from now on. The job is destroyed here, or on its worker once the
    /// job running there returns, so this never waits for it. Any thread but the task's job.
    void stop();

private:
    friend class AudioWorkers;

    /// Appends the task to the run queue and wakes a worker; under the pool's mutex
    void enqueue_locked();

    std::shared_ptr<Pool> pool_;
    /// Under the pool's mutex, but for job, which only the worker running it or stop() touches,
    /// each while running says it may
    std::function<bool()> job_;
    std::shared_ptr<Task> next_;
    bool queued_{false};
    bool running_{false};
    /// Woken while running: runs again once it returns
    bool again_{false};
    bool stopped_{false};
};

}  // namespace sendspin::server
