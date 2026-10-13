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

#include "audio_workers.h"

#include "profile.h"
#include "thread_setup.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

// ==============================================================================
// AudioWorkers
// ==============================================================================

size_t AudioWorkers::default_threads() {
    if (profile::AUDIO_WORKERS > 0) {
        return profile::AUDIO_WORKERS;
    }
    return std::clamp<size_t>(std::thread::hardware_concurrency(), 1, MAX_THREADS);
}

AudioWorkers::AudioWorkers(size_t threads) : pool_(std::make_shared<Pool>()) {
    this->threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        this->threads_.push_back(start_thread(ThreadRole::AUDIO_WORKER, [this] { this->run(); }));
    }
}

AudioWorkers::~AudioWorkers() {
    this->stop();
}

std::shared_ptr<AudioWorkers::Task> AudioWorkers::add(std::function<bool()> job) {
    return std::make_shared<Task>(this->pool_, std::move(job));
}

void AudioWorkers::stop() {
    std::shared_ptr<Task> queued;
    {
        const std::lock_guard<std::mutex> lock(this->pool_->mutex);
        this->pool_->stopping = true;
        queued = std::move(this->pool_->head);
        this->pool_->tail = nullptr;
        // Off the queue: a later Task::stop() has nothing to unlink, and no other thread touches
        // their links from now on
        for (Task* task = queued.get(); task != nullptr; task = task->next_.get()) {
            task->queued_ = false;
        }
    }
    this->pool_->work.notify_all();
    for (std::thread& thread : this->threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    // Unlinked one by one, so a long queue does not destroy its tasks recursively
    while (queued) {
        queued = std::exchange(queued->next_, nullptr);
    }
}

void AudioWorkers::run() {
    Pool& pool = *this->pool_;
    for (;;) {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(pool.mutex);
            pool.work.wait(lock, [&pool] { return pool.stopping || pool.head != nullptr; });
            if (pool.stopping) {
                return;
            }
            task = std::move(pool.head);
            pool.head = std::move(task->next_);
            if (!pool.head) {
                pool.tail = nullptr;
            }
            task->queued_ = false;
            task->running_ = true;
            task->again_ = false;
        }
        const bool more = task->job_();
        std::function<bool()> dropped;
        bool notify = false;
        {
            const std::lock_guard<std::mutex> lock(pool.mutex);
            task->running_ = false;
            if (task->stopped_) {
                dropped = std::move(task->job_);
            } else if ((more || task->again_) && !pool.stopping) {
                // At the back: the other groups' jobs run first
                task->enqueue_locked();
                notify = true;
            }
        }
        if (notify) {
            pool.work.notify_one();
        }
    }
}

// ==============================================================================
// Task
// ==============================================================================

AudioWorkers::Task::Task(std::shared_ptr<Pool> pool, std::function<bool()> job)
    : pool_(std::move(pool)), job_(std::move(job)) {}

void AudioWorkers::Task::wake() {
    {
        const std::lock_guard<std::mutex> lock(this->pool_->mutex);
        if (this->stopped_ || this->pool_->stopping || this->queued_) {
            return;
        }
        if (this->running_) {
            this->again_ = true;
            return;
        }
        this->enqueue_locked();
    }
    this->pool_->work.notify_one();
}

void AudioWorkers::Task::stop() {
    std::function<bool()> dropped;
    {
        const std::lock_guard<std::mutex> lock(this->pool_->mutex);
        this->stopped_ = true;
        if (this->queued_) {
            // Unlinked, so no worker takes it
            Pool& pool = *this->pool_;
            std::shared_ptr<Task>* link = &pool.head;
            Task* previous = nullptr;
            while (link->get() != this) {
                previous = link->get();
                link = &(*link)->next_;
            }
            *link = std::move(this->next_);
            if (pool.tail == this) {
                pool.tail = previous;
            }
            this->queued_ = false;
        }
        if (!this->running_) {
            dropped = std::move(this->job_);
        }
    }
}

void AudioWorkers::Task::enqueue_locked() {
    Pool& pool = *this->pool_;
    this->queued_ = true;
    if (pool.tail != nullptr) {
        pool.tail->next_ = this->shared_from_this();
    } else {
        pool.head = this->shared_from_this();
    }
    pool.tail = this;
}

}  // namespace sendspin::server
