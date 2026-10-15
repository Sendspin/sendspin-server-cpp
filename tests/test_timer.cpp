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

/// @file test_timer.cpp
/// @brief Timer and Wakeup on an EventLoop run from the test thread. Every deadline is already
/// past; a test that expects a timer's call runs the loop until it is made.

#include "clock.h"
#include "transport.h"
#include "transport_asio.h"
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::EventLoop;
using sendspin::server::now_us;
using sendspin::server::Timer;
using sendspin::server::Wakeup;

class TimerTest : public testing::Test {
protected:
    EventLoop loop;
    boost::asio::io_context& io{loop.impl().io()};
    std::vector<char> calls;

    /// Runs handlers until `count` calls were made, then every handler already ready, so a
    /// stray call after them shows too. A due timer's completion is ready only once the
    /// reactor has collected its expiry, which a single poll() may not do; a missing call
    /// hangs into the suite watchdog
    void run_until_called(std::size_t count) {
        while (this->calls.size() < count) {
            this->io.run_one();
        }
        this->io.poll();
    }
};

TEST_F(TimerTest, ASecondStartReplacesTheFirst) {
    Timer timer(this->loop);
    timer.start(now_us(), [this] { this->calls.push_back('a'); });
    timer.start(now_us(), [this] { this->calls.push_back('b'); });
    this->run_until_called(1);
    EXPECT_EQ(this->calls, std::vector<char>{'b'});
}

TEST_F(TimerTest, CancelDiscardsThePendingCall) {
    Timer timer(this->loop);
    timer.start(now_us(), [this] { this->calls.push_back('a'); });
    timer.cancel();
    this->io.poll();
    EXPECT_TRUE(this->calls.empty());
}

// The first timer's call restarts the second after the second's own expiry was already
// collected, so the second's stale completion runs and must not call anything
TEST_F(TimerTest, ACompletionAlreadyDueIsDiscardedWhenReplaced) {
    Timer first(this->loop);
    Timer second(this->loop);
    first.start(now_us(), [&] {
        this->calls.push_back('1');
        second.start(now_us(), [this] { this->calls.push_back('b'); });
    });
    // A later expiry than the first's, so the first runs first
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    second.start(now_us(), [this] { this->calls.push_back('a'); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    this->run_until_called(2);
    EXPECT_EQ(this->calls, (std::vector<char>{'1', 'b'}));
}

TEST_F(TimerTest, DroppingTheTimerDiscardsThePendingCall) {
    auto timer = std::make_unique<Timer>(this->loop);
    timer->start(now_us(), [this] { this->calls.push_back('a'); });
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    timer.reset();
    this->io.poll();
    EXPECT_TRUE(this->calls.empty());
}

// Signals while a run is pending add nothing; a signal after the run starts another
TEST_F(TimerTest, AWakeupRunsOncePerBatchOfSignals) {
    Wakeup wakeup(this->loop, [this] { this->calls.push_back('w'); });
    std::thread other([&] {
        wakeup.signal();
        wakeup.signal();
    });
    other.join();
    wakeup.signal();
    this->io.poll();
    EXPECT_EQ(this->calls, std::vector<char>{'w'});
    this->io.restart();
    wakeup.signal();
    this->io.poll();
    EXPECT_EQ(this->calls, (std::vector<char>{'w', 'w'}));
}

TEST_F(TimerTest, DroppingAWakeupDiscardsItsPendingRun) {
    auto wakeup = std::make_unique<Wakeup>(this->loop, [this] { this->calls.push_back('w'); });
    const Wakeup::Handle handle = wakeup->handle();
    wakeup->signal();
    wakeup.reset();
    this->io.poll();
    // A handle that outlives its Wakeup signals nothing
    Wakeup::signal(handle);
    this->io.restart();
    this->io.poll();
    EXPECT_TRUE(this->calls.empty());
}

// A run keeps its closure for the next signal's run, and a run that drops its own Wakeup
// finishes with its captures intact; its closure is released then, though a handle still holds
// the Wakeup's state
TEST_F(TimerTest, ARunThatDropsItsWakeupFinishes) {
    std::unique_ptr<Wakeup> wakeup;
    // Heap storage in the closure, so ASan reports a closure freed under its run
    const std::string tag(64, 'w');
    auto capture = std::make_shared<int>(0);
    const std::weak_ptr<int> captured = capture;
    wakeup =
        std::make_unique<Wakeup>(this->loop, [this, &wakeup, tag, capture = std::move(capture)] {
            if (this->calls.size() == 1) {
                wakeup.reset();
            }
            this->calls.push_back(tag.back());
        });
    const Wakeup::Handle handle = wakeup->handle();
    wakeup->signal();
    this->io.poll();
    this->io.restart();
    wakeup->signal();
    this->io.poll();
    EXPECT_EQ(this->calls, (std::vector<char>{'w', 'w'}));
    EXPECT_TRUE(captured.expired());
}

// A signal from a handle after the loop is gone does nothing
TEST_F(TimerTest, AHandleOutlivingTheLoopSignalsNothing) {
    auto loop = std::make_unique<EventLoop>();
    Wakeup wakeup(*loop, [this] { this->calls.push_back('w'); });
    const Wakeup::Handle handle = wakeup.handle();
    loop.reset();
    Wakeup::signal(handle);
    EXPECT_TRUE(this->calls.empty());
}

}  // namespace
