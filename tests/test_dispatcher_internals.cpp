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

/// @file test_dispatcher_internals.cpp
/// @brief Dispatcher::on_thread(): true on the dispatch thread alone, and on no thread once it
/// ended. Compiled with -fno-access-control (tests/CMakeLists.txt).

#include "dispatcher.h"
#include "thread_setup.h"
#include <gtest/gtest.h>

#include <future>

namespace {

using sendspin::server::Dispatcher;

/// Whether watched's on_thread() is true on the thread running dispatcher's tasks
bool on_thread_of(Dispatcher& dispatcher, const Dispatcher& watched) {
    std::promise<bool> seen;
    dispatcher.post([&seen, &watched] { seen.set_value(watched.on_thread()); });
    return seen.get_future().get();
}

// The dispatch thread alone is on its thread, and no thread is once it ended. A later thread given
// the ended thread's token would show a token left set, but a host need not hand one out again, so
// the test reads the token.
TEST(Dispatcher, KnowsItsThreadWhileItRunsAndNoneOnceItEnded) {
    Dispatcher first;
    EXPECT_TRUE(on_thread_of(first, first));
    EXPECT_FALSE(first.on_thread());
    {
        Dispatcher second;
        EXPECT_FALSE(on_thread_of(second, first));
        EXPECT_FALSE(on_thread_of(first, second));
    }
    first.finish();
    first.join();
    EXPECT_FALSE(first.on_thread());
    Dispatcher later;
    EXPECT_FALSE(on_thread_of(later, first));
    EXPECT_EQ(first.token_.load(), nullptr);
}

}  // namespace
