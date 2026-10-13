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

#include "dispatcher.h"

#include "thread_setup.h"

#include <utility>

namespace sendspin::server {

Dispatcher::Dispatcher() : thread_(start_thread(ThreadRole::DISPATCH, [this] { this->run(); })) {}

Dispatcher::~Dispatcher() {
    this->finish();
    this->join();
}

void Dispatcher::finish() {
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->finishing_ = true;
    }
    this->wake_.notify_one();
}

void Dispatcher::join() {
    if (this->thread_.joinable()) {
        this->thread_.join();
    }
}

void Dispatcher::run() {
    this->token_.store(current_thread_token(), std::memory_order_relaxed);
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(this->mutex_);
            this->wake_.wait(lock, [this] { return this->finishing_ || !this->tasks_.empty(); });
            if (this->tasks_.empty()) {
                // A later thread may be given the same token
                this->token_.store(nullptr, std::memory_order_relaxed);
                return;
            }
            this->batch_.swap(this->tasks_);
        }
        // A task may post: it goes into tasks_, behind this batch
        while (!this->batch_.empty()) {
            this->batch_.run_front();
        }
    }
}

}  // namespace sendspin::server
