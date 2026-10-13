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

/// @file test_live_feeder.cpp
/// @brief server_cli's live feeder playing PCM from a pipe through a live GroupInput of a running
/// server with no players: what arrives is written as it comes, through a pause that keeps taking
/// it, and drained at the end of the data; what the input refuses is dropped

#include "live_feeder.h"
#include "test_recorder.h"
#include <gtest/gtest.h>
#include <unistd.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

using sendspin::server::DrainId;
using sendspin::server::DrainOutcome;
using sendspin::server::GroupId;
using sendspin::server::InputFormat;
using sendspin::server::InputId;
using sendspin::server::InputOptions;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::ServerListener;
using sendspin::server::StartResult;
using sendspin::server::examples::LiveFeeder;
using sendspin::server::test::MemoryProvider;

constexpr uint32_t RATE = 48000;
constexpr size_t FRAME_BYTES = 4;

/// Hands each drain outcome to the feeder, as server_cli's listener does, and logs the feeder's
/// events
class LiveLog : public ServerListener {
public:
    void on_drained(GroupId /*group*/, InputId input, DrainId drain,
                    DrainOutcome outcome) override {
        const std::lock_guard<std::mutex> gate(this->gate);
        // Held across the call, so the fixture cannot destroy the feeder under it
        const std::lock_guard<std::mutex> lock(this->feeder_mutex_);
        if (this->feeder_ != nullptr) {
            this->feeder_->on_drained(input, drain, outcome);
        }
    }

    void on_event(LiveFeeder::Event event) {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->events_.push_back(event);
        }
        this->changed_.notify_all();
    }

    void set_feeder(LiveFeeder* feeder) {
        const std::lock_guard<std::mutex> lock(this->feeder_mutex_);
        this->feeder_ = feeder;
    }

    /// Waits with no timeout; the suite watchdog catches a hang
    std::vector<LiveFeeder::Event> wait_for(LiveFeeder::Event event) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] {
            return std::find(this->events_.begin(), this->events_.end(), event) !=
                   this->events_.end();
        });
        return this->events_;
    }

    /// Holding it keeps every drain outcome from reaching the feeder
    std::mutex gate;

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<LiveFeeder::Event> events_;
    std::mutex feeder_mutex_;
    LiveFeeder* feeder_{nullptr};
};

/// A started server, a pipe, and a live feeder reading the pipe into a live input of the server's
/// group
class LiveFixture {
public:
    LiveFixture() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Live";
        EXPECT_EQ(this->server.start(config, &this->provider, &this->log), StartResult::OK);
        EXPECT_EQ(::pipe(this->pipe_), 0);
        const InputFormat format{RATE, 2, SampleFormat::S16};
        this->feeder = std::make_unique<LiveFeeder>(
            this->pipe_[0], format,
            [this](GroupId /*group*/, LiveFeeder::Event event) { this->log.on_event(event); });
        this->log.set_feeder(this->feeder.get());
        InputOptions options;
        options.format = format;
        options.kind = sendspin::server::InputKind::LIVE;
        this->group = this->server.default_group().value_or(GroupId{});
        std::shared_ptr<sendspin::server::GroupInput> input =
            this->server.open_input(this->group, options).input;
        EXPECT_TRUE(input);
        // Takes nothing until the server attached it
        EXPECT_EQ(input->wait_writable(std::numeric_limits<int64_t>::max()),
                  sendspin::server::InputState::OPEN);
        EXPECT_TRUE(this->feeder->attach(std::move(input)));
    }

    ~LiveFixture() {
        this->close_writer();
        this->log.set_feeder(nullptr);
        this->feeder.reset();
        ::close(this->pipe_[0]);
        this->server.stop();
    }

    LiveFixture(const LiveFixture&) = delete;
    LiveFixture& operator=(const LiveFixture&) = delete;

    /// Writes frames of silence into the pipe, blocking while it is full
    void send(size_t frames) {
        this->send_bytes(frames * FRAME_BYTES);
    }

    /// Writes bytes of silence, which may end within a frame
    void send_bytes(size_t bytes) {
        const std::vector<uint8_t> pcm(bytes, 0);
        size_t sent = 0;
        while (sent < pcm.size()) {
            const ssize_t n = ::write(this->pipe_[1], pcm.data() + sent, pcm.size() - sent);
            ASSERT_GT(n, 0);
            sent += static_cast<size_t>(n);
        }
    }

    /// Writes frames in 10 ms blocks at real time's pace. A block more than two blocks late (a
    /// thread held up on a loaded machine) restarts the pace from there, so the data falls behind
    /// rather than bursting past the live write-ahead.
    void send_live(size_t frames) {
        using Clock = std::chrono::steady_clock;
        constexpr auto BLOCK_TIME = std::chrono::milliseconds(10);
        Clock::time_point due = Clock::now();
        for (size_t sent = 0; sent < frames; sent += RATE / 100) {
            std::this_thread::sleep_until(due);
            this->send(std::min<size_t>(RATE / 100, frames - sent));
            due += BLOCK_TIME;
            if (Clock::now() - due > 2 * BLOCK_TIME) {
                due = Clock::now();
            }
        }
    }

    /// Waits, with no timeout, until the feeder counted frames as written or dropped
    void wait_counted(uint64_t frames) {
        for (;;) {
            const std::optional<LiveFeeder::Status> status = this->feeder->status(this->group);
            if (!status || status->written + status->dropped >= frames) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /// The end of the data
    void close_writer() {
        if (this->pipe_[1] >= 0) {
            ::close(this->pipe_[1]);
            this->pipe_[1] = -1;
        }
    }

    MemoryProvider provider;
    LiveLog log;
    SendspinServer server;
    GroupId group;
    std::unique_ptr<LiveFeeder> feeder;

private:
    int pipe_[2]{-1, -1};
};

// Data delivered in real time is written whole as it arrives, through a pause that keeps taking it
// and a resume; at its end the feeder drains the input, which plays. Half a second, delivered in
// real time.
TEST(LiveFeeder, WritesWhatArrivesThroughAPauseAndDrainsAtTheEnd) {
    LiveFixture f;
    f.send_live(RATE / 5);
    EXPECT_TRUE(f.feeder->pause(f.group));
    EXPECT_FALSE(f.feeder->pause(f.group));
    f.send_live(RATE / 5);
    EXPECT_TRUE(f.feeder->resume(f.group));
    EXPECT_FALSE(f.feeder->resume(f.group));
    f.send_live(RATE / 10);
    f.close_writer();
    const std::vector<LiveFeeder::Event> events = f.log.wait_for(LiveFeeder::Event::PLAYED);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0], LiveFeeder::Event::END_OF_INPUT);
    const std::optional<LiveFeeder::Status> status = f.feeder->status(f.group);
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->ended);
    EXPECT_EQ(status->written, uint64_t{RATE / 2});
    EXPECT_EQ(status->dropped, 0U);
    // Control: no input of another group
    EXPECT_FALSE(f.feeder->status(GroupId{f.group.value + 1}));
}

// Data delivered faster than real time is cut at the live write-ahead, and the feeder drops the
// rest rather than wait
TEST(LiveFeeder, DropsWhatTheInputRefuses) {
    LiveFixture f;
    f.send(RATE);
    f.close_writer();
    f.log.wait_for(LiveFeeder::Event::PLAYED);
    const std::optional<LiveFeeder::Status> status = f.feeder->status(f.group);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->written + status->dropped, uint64_t{RATE});
    EXPECT_GE(status->written, uint64_t{RATE / 5});
    EXPECT_GT(status->dropped, 0U);
}

// A read that ends within a frame keeps the partial frame until the rest of it arrives: 1922
// bytes are 480 frames and half of one, and with the next 1918 bytes, 960 frames in all
TEST(LiveFeeder, CarriesAPartialFrameIntoTheNextRead) {
    LiveFixture f;
    f.send_bytes((480 * FRAME_BYTES) + 2);
    f.wait_counted(480);
    f.send_bytes((480 * FRAME_BYTES) - 2);
    f.close_writer();
    f.log.wait_for(LiveFeeder::Event::PLAYED);
    const std::optional<LiveFeeder::Status> status = f.feeder->status(f.group);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->written + status->dropped, 960U);
}

// LiveFeeder::Event: once the data ends with no input attached, the feeder reports that nothing
// is left to play, so a caller waiting for the end is not left waiting
TEST(LiveFeeder, ReportsTheEndWithNothingLeftToPlay) {
    LiveFixture f;
    EXPECT_TRUE(f.feeder->detach(f.group));
    f.send(RATE / 100);
    f.close_writer();
    const std::vector<LiveFeeder::Event> events = f.log.wait_for(LiveFeeder::Event::ENDED);
    EXPECT_EQ(events, std::vector<LiveFeeder::Event>{LiveFeeder::Event::ENDED});
}

// LiveFeeder::Event: a detach after the data ended is the final event while the drain has no
// outcome yet, its outcome then reaching no feeder; once the drain played, the detach reports
// nothing more. The gate holds the outcome back until the detach.
TEST(LiveFeeder, ReportsADetachAfterTheEndOnlyBeforeTheDrainsOutcome) {
    using Event = LiveFeeder::Event;
    for (const bool played : {false, true}) {
        SCOPED_TRACE(played);
        LiveFixture f;
        f.send(RATE / 100);
        std::unique_lock<std::mutex> gate(f.log.gate);
        f.close_writer();
        f.log.wait_for(Event::END_OF_INPUT);
        if (played) {
            gate.unlock();
            f.log.wait_for(Event::PLAYED);
        }
        EXPECT_TRUE(f.feeder->detach(f.group));
        if (!played) {
            gate.unlock();
        }
        const std::vector<Event> events = f.log.wait_for(played ? Event::PLAYED : Event::ENDED);
        EXPECT_EQ(events,
                  (std::vector<Event>{Event::END_OF_INPUT, played ? Event::PLAYED : Event::ENDED}));
    }
}

}  // namespace
