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

/// @file dispatcher.h
/// @brief The dispatch thread: runs the host's listener, persistence provider, trace and log calls
/// one at a time, in the order they were posted, so the core thread never waits on the host

#pragma once

#include "task_queue.h"
#include "thread_setup.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace sendspin::server {

class Dispatcher {
public:
    /// @brief Starts the thread
    Dispatcher();
    /// @brief Calls finish() and join(); never from the dispatch thread
    ~Dispatcher();
    Dispatcher(const Dispatcher&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;

    /// @brief The bytes each of the thread's two task queues holds before a burst grows it: a few
    /// tasks with their headers
    static constexpr size_t QUEUE_BYTES = 256;

    /// @brief Queues task, a closure called with no arguments, behind every task posted before it.
    /// Any thread. A task posted after finish() is dropped, destroyed on the posting thread.
    template <class F>
    void post(F&& task) {
        std::decay_t<F> held(std::forward<F>(task));
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            if (this->finishing_) {
                return;
            }
            this->tasks_.push(TaskRef(held));
        }
        this->wake_.notify_one();
    }

    /// @brief The thread runs the tasks already posted, then exits. Any thread.
    void finish();

    /// @brief Waits for the thread to exit; never from the dispatch thread
    void join();

    /// @brief Whether the caller is the dispatch thread. Any thread or task.
    bool on_thread() const {
        return current_thread_token() == this->token_.load(std::memory_order_relaxed);
    }

private:
    void run();

    std::mutex mutex_;
    std::condition_variable wake_;
    /// The tasks posted and not yet taken, under mutex_; the thread swaps them with batch_, which
    /// it runs without the mutex. In PSRAM: only the thread's stack must be internal
    TaskQueue tasks_{QUEUE_BYTES, MemoryLocation::PREFER_EXTERNAL};
    TaskQueue batch_{QUEUE_BYTES, MemoryLocation::PREFER_EXTERNAL};
    bool finishing_{false};
    /// The thread's token while it runs tasks, null otherwise, which no caller's token equals. Only
    /// the thread writes it, so any other caller reads a token not its own. Declared before
    /// thread_, so it is set up before the thread can write it.
    std::atomic<ThreadToken> token_{nullptr};
    std::thread thread_;
};

}  // namespace sendspin::server
