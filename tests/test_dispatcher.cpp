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

/// @file test_dispatcher.cpp
/// @brief Dispatcher's queue: a task a task posts runs behind those queued before it, and one
/// posted after finish() is dropped.

#include "dispatcher.h"
#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <vector>

namespace {

using sendspin::server::Dispatcher;

// A task posted from a task goes behind the tasks already queued
TEST(Dispatcher, RunsATasksPostBehindTheQueue) {
    Dispatcher dispatcher;
    std::vector<int> order;
    std::promise<void> release;
    const std::shared_future<void> released = release.get_future().share();
    std::promise<void> last;
    dispatcher.post([&dispatcher, &order, &last, released] {
        released.wait();
        order.push_back(0);
        dispatcher.post([&order, &last] {
            order.push_back(3);
            last.set_value();
        });
    });
    dispatcher.post([&order] { order.push_back(1); });
    dispatcher.post([&order] { order.push_back(2); });
    release.set_value();
    last.get_future().wait();
    dispatcher.finish();
    dispatcher.join();
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3}));
}

// A task posted after finish() is destroyed by the posting thread and never runs
TEST(Dispatcher, DropsATaskPostedAfterFinish) {
    Dispatcher dispatcher;
    dispatcher.finish();
    bool ran = false;
    const auto held = std::make_shared<int>(0);
    dispatcher.post([&ran, held] { ran = *held == 0; });
    EXPECT_EQ(held.use_count(), 1) << "the task posted after finish() is destroyed at once";
    dispatcher.join();
    EXPECT_FALSE(ran);
}

}  // namespace
