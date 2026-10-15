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

/// @file test_feeder.cpp
/// @brief server_cli's feeder playing files through a GroupInput of a running server with no
/// players: its drain at the end of the last file, a seek that cancels that drain, a queued file
/// of another format on the same run, and a jump to another file on the same input; and a looping
/// file played to a raw player, gaplessly on one stream

#include "feeder.h"
#include "raw_client.h"
#include "test_recorder.h"
#include "wire_messages.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
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
using sendspin::server::examples::AudioInfo;
using sendspin::server::examples::AudioSource;
using sendspin::server::examples::Feeder;
using sendspin::server::test::MemoryProvider;
namespace wire = sendspin::server::test::wire;

constexpr uint32_t RATE = 48000;

/// A file of seconds of format's silence
class Silence : public AudioSource {
public:
    Silence(const InputFormat& format, double seconds)
        : info_{format, static_cast<uint64_t>(seconds * format.sample_rate), false} {}

    AudioInfo info() const override {
        return this->info_;
    }

    size_t read(uint64_t frame, uint8_t* out, size_t frames) override {
        const auto n = static_cast<size_t>(
            std::min<uint64_t>(frames, this->info_.frames - std::min(frame, this->info_.frames)));
        std::fill_n(out, n * sendspin::server::examples::frame_bytes(this->info_.format), 0);
        return n;
    }

private:
    AudioInfo info_;
};

/// Hands each drain outcome to the feeder, as server_cli's listener does, and logs the outcomes
/// and the feeder's events. Optionally pauses the feeder from its first END_OF_FILE, on the
/// feeder's thread right after its drain(), so that drain cannot complete before a seek.
class Log : public ServerListener {
public:
    explicit Log(bool pause_at_end) : pause_at_end_(pause_at_end) {}

    void on_drained(GroupId /*group*/, InputId /*input*/, DrainId drain,
                    DrainOutcome outcome) override {
        {
            // Held across the call, so the fixture cannot destroy the feeder under it
            const std::lock_guard<std::mutex> lock(this->feeder_mutex_);
            if (this->feeder_ != nullptr) {
                this->feeder_->on_drained(drain, outcome);
            }
        }
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->drains_.emplace_back(drain, outcome);
        }
        this->changed_.notify_all();
    }

    void on_event(const Feeder::Report& report) {
        const Feeder::Event event = report.event;
        if (event == Feeder::Event::END_OF_FILE && this->pause_at_end_) {
            this->pause_at_end_ = false;
            std::unique_lock<std::mutex> lock(this->feeder_mutex_);
            this->feeder_set_.wait(lock, [this] { return this->feeder_ != nullptr; });
            this->paused_at_end_ = this->feeder_->pause().has_value();
        }
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->events_.push_back(event);
            this->reports_.push_back(report);
        }
        this->changed_.notify_all();
    }

    std::vector<Feeder::Report> reports() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->reports_;
    }

    void set_feeder(Feeder* feeder) {
        {
            const std::lock_guard<std::mutex> lock(this->feeder_mutex_);
            this->feeder_ = feeder;
        }
        this->feeder_set_.notify_all();
    }

    /// Waits with no timeout; the suite watchdog catches a hang
    void wait_for(Feeder::Event event, size_t count) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] {
            return static_cast<size_t>(
                       std::count(this->events_.begin(), this->events_.end(), event)) >= count;
        });
    }

    /// Waits with no timeout for count drain outcomes
    std::vector<std::pair<DrainId, DrainOutcome>> wait_for_drains(size_t count) {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [&] { return this->drains_.size() >= count; });
        return this->drains_;
    }

    /// Read on the feeder's thread before the END_OF_FILE it follows is logged
    bool paused_at_end() const {
        const std::lock_guard<std::mutex> lock(this->feeder_mutex_);
        return this->paused_at_end_;
    }

private:
    bool pause_at_end_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Feeder::Event> events_;
    std::vector<Feeder::Report> reports_;
    std::vector<std::pair<DrainId, DrainOutcome>> drains_;
    mutable std::mutex feeder_mutex_;
    std::condition_variable feeder_set_;
    Feeder* feeder_{nullptr};
    bool paused_at_end_{false};
};

/// A started server that approves every client, a file of seconds of S16 stereo silence and a
/// feeder playing it to the server's group, looping when told to
class FeederFixture {
public:
    explicit FeederFixture(double seconds, bool pause_at_end = false, bool loop = false)
        : log(pause_at_end) {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Feeder";
        config.approve_all_clients = true;
        EXPECT_EQ(this->server.start(config, &this->provider, &this->log), StartResult::OK);
        auto file = std::make_shared<Silence>(InputFormat{RATE, 2, SampleFormat::S16}, seconds);
        InputOptions options;
        options.format = file->info().format;
        const std::optional<GroupId> group = this->server.default_group();
        EXPECT_TRUE(group.has_value());
        std::shared_ptr<sendspin::server::GroupInput> input =
            this->server.open_input(group.value_or(GroupId{}), options).input;
        EXPECT_TRUE(input);
        this->input = input.get();
        this->feeder = std::make_unique<Feeder>(
            std::move(input), std::move(file),
            [this](const Feeder::Report& report) { this->log.on_event(report); }, loop);
        this->log.set_feeder(this->feeder.get());
    }

    ~FeederFixture() {
        this->log.set_feeder(nullptr);
        this->feeder.reset();
        this->server.stop();
    }

    FeederFixture(const FeederFixture&) = delete;
    FeederFixture& operator=(const FeederFixture&) = delete;

    MemoryProvider provider;
    Log log;
    SendspinServer server;
    /// The feeder's, valid while it lives
    sendspin::server::GroupInput* input{nullptr};
    std::unique_ptr<Feeder> feeder;
};

TEST(Feeder, WritesTheWholeFileAndDrainsIt) {
    FeederFixture f(0.5);
    f.log.wait_for(Feeder::Event::PLAYED, 1);
    const Feeder::Status status = f.feeder->status();
    EXPECT_EQ(status.phase, Feeder::Phase::FINISHED);
    EXPECT_EQ(status.written_to, status.total);
    EXPECT_EQ(status.playing.frame, status.total);
    const auto drains = f.log.wait_for_drains(1);
    ASSERT_EQ(drains.size(), 1U);
    EXPECT_EQ(drains[0].second, DrainOutcome::PLAYED);
}

// Paused right after its drain() at the end of the file, the drain cannot complete, so the seek
// cancels it; an outcome for that replaced drain finishes nothing, and the resumed file drains
// again from the new position
TEST(Feeder, SeekingWhileDrainingFlushesAndPlaysOnFromTheNewPosition) {
    FeederFixture f(1.0, true);
    f.log.wait_for(Feeder::Event::END_OF_FILE, 1);
    ASSERT_TRUE(f.log.paused_at_end());
    EXPECT_TRUE(f.feeder->seek(RATE / 2));
    const auto flushed = f.log.wait_for_drains(1);
    ASSERT_EQ(flushed.size(), 1U);
    EXPECT_EQ(flushed[0].second, DrainOutcome::FLUSHED);
    f.feeder->on_drained(flushed[0].first, DrainOutcome::PLAYED);
    EXPECT_NE(f.feeder->status().phase, Feeder::Phase::FINISHED);

    EXPECT_TRUE(f.feeder->resume());
    f.log.wait_for(Feeder::Event::PLAYED, 1);
    const auto drains = f.log.wait_for_drains(2);
    ASSERT_EQ(drains.size(), 2U);
    EXPECT_NE(drains[1].first, drains[0].first);
    EXPECT_EQ(drains[1].second, DrainOutcome::PLAYED);
    const Feeder::Status status = f.feeder->status();
    EXPECT_EQ(status.phase, Feeder::Phase::FINISHED);
    EXPECT_EQ(status.written_to, status.total);
}

// A file queued while the first is written follows it on the same run, as a new segment in its own
// format: one drain plays both, the feeder reports the second file begun at the first one's end on
// the timeline (its 3 s past the first's start), and finishes in it. The first file outlasts the
// write-ahead and the group lead by over a second, so the queue comes well before its end; the test
// plays the 3.3 s in real time.
TEST(Feeder, PlaysAQueuedFileOfAnotherFormatOnTheSameRun) {
    FeederFixture f(3.0);
    auto file = std::make_shared<Silence>(InputFormat{44100, 1, SampleFormat::S16}, 0.3);
    const AudioInfo info = file->info();
    ASSERT_TRUE(f.feeder->queue(file));
    // The run plays on well past the second file's start: its first position's time is anchored
    f.log.wait_for(Feeder::Event::NEXT_FILE, 1);
    const int64_t first_us = f.input->time_of(0);
    f.log.wait_for(Feeder::Event::PLAYED, 1);
    const std::vector<Feeder::Report> reports = f.log.reports();
    ASSERT_EQ(reports.size(), 3U);
    EXPECT_EQ(reports[0].event, Feeder::Event::NEXT_FILE);
    EXPECT_EQ(reports[0].file, 1U);
    EXPECT_EQ(reports[0].at_us - first_us, 3000000);
    EXPECT_EQ(reports[1].event, Feeder::Event::END_OF_FILE);
    const auto drains = f.log.wait_for_drains(1);
    ASSERT_EQ(drains.size(), 1U);
    EXPECT_EQ(drains[0].second, DrainOutcome::PLAYED);
    const Feeder::Status status = f.feeder->status();
    EXPECT_EQ(status.writing, 1U);
    EXPECT_EQ(status.playing.file, 1U);
    EXPECT_EQ(status.written_to, info.frames);
    EXPECT_EQ(f.feeder->info(1).format, info.format);
}

// spec messaging.md "stream/clear": play() while a file is written jumps to another on the same
// input, in its own format: the input is flushed, the queued files are dropped, and the new file
// plays to its end with one drain
TEST(Feeder, JumpsToAnotherFileOnTheSameInput) {
    FeederFixture f(1.5);
    ASSERT_TRUE(
        f.feeder->queue(std::make_shared<Silence>(InputFormat{48000, 2, SampleFormat::S16}, 0.2)));
    auto file = std::make_shared<Silence>(InputFormat{22050, 1, SampleFormat::S16}, 0.2);
    const AudioInfo info = file->info();
    ASSERT_TRUE(f.feeder->play(file));
    f.log.wait_for(Feeder::Event::PLAYED, 1);
    const auto drains = f.log.wait_for_drains(1);
    ASSERT_EQ(drains.size(), 1U);
    EXPECT_EQ(drains[0].second, DrainOutcome::PLAYED);
    const Feeder::Status status = f.feeder->status();
    EXPECT_EQ(status.writing, 0U);
    EXPECT_EQ(status.written_to, info.frames);
    EXPECT_EQ(f.feeder->info(0).format, info.format);
    // No NEXT_FILE: the queued file went with the jump
    for (const Feeder::Report& report : f.log.reports()) {
        EXPECT_NE(report.event, Feeder::Event::NEXT_FILE);
    }
}

// Looping, the end of the file goes on from its start on the same run: each pass is reported
// begun as file 0 and nothing drains, a seek keeps the loop going, and once the loop is turned
// off the file plays to its end and drains once. The passes written ahead play out in real time
// before the drain: about 2 s.
TEST(Feeder, LoopsUntilTheLoopIsTurnedOff) {
    FeederFixture f(0.5, false, true);
    f.log.wait_for(Feeder::Event::NEXT_FILE, 3);
    EXPECT_TRUE(f.feeder->seek(RATE / 4));
    f.log.wait_for(Feeder::Event::NEXT_FILE, 5);
    f.feeder->set_loop(false);
    f.log.wait_for(Feeder::Event::PLAYED, 1);
    for (const Feeder::Report& report : f.log.reports()) {
        if (report.event == Feeder::Event::NEXT_FILE) {
            EXPECT_EQ(report.file, 0U);
        }
    }
    const auto drains = f.log.wait_for_drains(1);
    ASSERT_EQ(drains.size(), 1U);
    EXPECT_EQ(drains[0].second, DrainOutcome::PLAYED);
    EXPECT_EQ(f.feeder->status().phase, Feeder::Phase::FINISHED);
}

// A file of no frame ends the run where a loop would start it over, and a file of one frame
// loops one frame at a time until the loop is turned off
TEST(Feeder, LoopsAFileOfNoFrameOrOneFrame) {
    for (const uint64_t frames : {0, 1}) {
        SCOPED_TRACE(frames);
        FeederFixture f((static_cast<double>(frames) + 0.5) / RATE, false, true);
        if (frames == 1) {
            f.log.wait_for(Feeder::Event::NEXT_FILE, 1000);
            f.feeder->set_loop(false);
        }
        // With no frame the drain may play before the fixture hands the feeder to the listener,
        // so its outcome is waited for there and handed on again, which a feeder that had it
        // ignores
        const auto drains = f.log.wait_for_drains(1);
        ASSERT_EQ(drains.size(), 1U);
        EXPECT_EQ(drains[0].second, DrainOutcome::PLAYED);
        f.feeder->on_drained(drains[0].first, drains[0].second);
        f.log.wait_for(Feeder::Event::PLAYED, 1);
        const std::vector<Feeder::Report> reports = f.log.reports();
        if (frames == 0) {
            EXPECT_EQ(std::count_if(reports.begin(), reports.end(),
                                    [](const Feeder::Report& report) {
                                        return report.event == Feeder::Event::NEXT_FILE;
                                    }),
                      0);
        }
        EXPECT_EQ(f.log.wait_for_drains(1).size(), 1U);
        EXPECT_EQ(f.feeder->status().phase, Feeder::Phase::FINISHED);
    }
}

/// Frames that carry their position, from first on: stereo, the left sample the position and the
/// right its complement, so silence is told apart from position 0. In S24 the 16-bit values are
/// the top bytes, so a 16-bit stream carries them unchanged.
class Positions : public AudioSource {
public:
    Positions(uint64_t frames, SampleFormat format, uint16_t first)
        : frames_(frames), format_(format), first_(first) {}

    AudioInfo info() const override {
        return {InputFormat{RATE, 2, this->format_}, this->frames_, false};
    }

    size_t read(uint64_t frame, uint8_t* out, size_t frames) override {
        {
            std::unique_lock<std::mutex> lock(this->gate_mutex_);
            this->gate_changed_.wait(lock, [this] { return !this->held_; });
        }
        const auto n = static_cast<size_t>(
            std::min<uint64_t>(frames, this->frames_ - std::min(frame, this->frames_)));
        const size_t sample = this->format_ == SampleFormat::S24 ? 3 : 2;
        uint8_t* at = out;
        for (size_t i = 0; i < n; ++i) {
            const auto position = static_cast<uint16_t>(this->first_ + frame + i);
            for (const auto value : {position, static_cast<uint16_t>(~position)}) {
                if (sample == 3) {
                    *at++ = 0;
                }
                *at++ = static_cast<uint8_t>(value);
                *at++ = static_cast<uint8_t>(value >> 8);
            }
        }
        return n;
    }

    /// The position a 16-bit frame carries; nullopt for one it did not write
    static std::optional<uint16_t> position_of(const uint8_t* frame) {
        const auto position = static_cast<uint16_t>(frame[0] | (frame[1] << 8));
        const auto check = static_cast<uint16_t>(frame[2] | (frame[3] << 8));
        return check == static_cast<uint16_t>(~position) ? std::optional<uint16_t>(position)
                                                         : std::nullopt;
    }

    /// Holds every read until release(), so a test can queue behind a feeder that has not read
    void hold() {
        const std::lock_guard<std::mutex> lock(this->gate_mutex_);
        this->held_ = true;
    }

    void release() {
        {
            const std::lock_guard<std::mutex> lock(this->gate_mutex_);
            this->held_ = false;
        }
        this->gate_changed_.notify_all();
    }

private:
    uint64_t frames_;
    SampleFormat format_;
    uint16_t first_;
    std::mutex gate_mutex_;
    std::condition_variable gate_changed_;
    bool held_{false};
};

/// server_cli --loop for a sync measurement, with files written as a playlist: a raw player gets
/// them on one stream, with no stream/clear or stream/end, every chunk's timestamp where the one
/// before it ended, and the files' frames in order, from the last file's last frame straight to the
/// first file's first, pass after pass
void expect_gapless_loop(const std::vector<std::shared_ptr<Positions>>& files, uint64_t frames) {
    constexpr size_t PASSES = 3;
    constexpr int64_t US_PER_SECOND = 1000000;
    MemoryProvider provider;
    Log log(false);
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Loop";
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &log), StartResult::OK);
    sendspin::server::test::RawClient client;
    ASSERT_TRUE(server.connect_url(client.url()));
    client.activate(sendspin::server::test::player_hello_json(
        "Loop", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 2000000));
    client.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(150, 0, 0)));

    InputOptions options;
    options.format = files.front()->info().format;
    std::shared_ptr<sendspin::server::GroupInput> input =
        server.open_input(server.default_group().value_or(GroupId{}), options).input;
    ASSERT_TRUE(input);
    // Every file queued before the first is read, so a slow test thread cannot let a short first
    // file end, and the loop start over, before the playlist is whole
    files.front()->hold();
    auto feeder = std::make_unique<Feeder>(
        std::move(input), files.front(),
        [&log](const Feeder::Report& report) { log.on_event(report); }, true);
    log.set_feeder(feeder.get());
    for (size_t i = 1; i < files.size(); ++i) {
        const bool queued = feeder->queue(files[i]);
        if (!queued) {
            files.front()->release();
        }
        ASSERT_TRUE(queued);
    }
    files.front()->release();

    size_t starts = 0;
    std::optional<int64_t> next_ts;
    std::optional<uint16_t> previous;
    size_t wraps = 0;
    while (wraps < PASSES) {
        const std::optional<std::vector<uint8_t>> message = client.receive_plaintext();
        ASSERT_TRUE(message);
        ASSERT_FALSE(message->empty());
        if (message->front() != wire::AUDIO_CHUNK_ID) {
            JsonDocument doc;
            ASSERT_EQ(deserializeJson(doc, message->data() + 1, message->size() - 1),
                      DeserializationError::Ok);
            const std::string type = doc["type"].as<std::string>();
            EXPECT_NE(type, "stream/clear");
            EXPECT_NE(type, "stream/end");
            starts += type == "stream/start" ? 1 : 0;
            continue;
        }
        ASSERT_GT(message->size(), wire::AUDIO_CHUNK_HEADER);
        const int64_t ts = wire::timestamp_at(*message);
        const size_t chunk_frames = (message->size() - wire::AUDIO_CHUNK_HEADER) / 4;
        if (next_ts) {
            EXPECT_EQ(ts, *next_ts) << "a gap or overlap in the timeline";
        }
        ASSERT_EQ(chunk_frames * US_PER_SECOND % RATE, 0U);
        next_ts = ts + static_cast<int64_t>(chunk_frames * US_PER_SECOND / RATE);
        for (size_t i = 0; i < chunk_frames; ++i) {
            const std::optional<uint16_t> position =
                Positions::position_of(message->data() + wire::AUDIO_CHUNK_HEADER + (4 * i));
            ASSERT_TRUE(position) << "a frame the files do not hold";
            if (previous) {
                const auto expected = static_cast<uint16_t>((*previous + 1) % frames);
                ASSERT_EQ(*position, expected);
                wraps += *position == 0 ? 1 : 0;
            }
            previous = position;
        }
    }
    EXPECT_EQ(starts, 1U);
    log.set_feeder(nullptr);
    feeder.reset();
    server.stop();
}

TEST(Feeder, LoopsGaplesslyOnOneStream) {
    expect_gapless_loop({std::make_shared<Positions>(RATE / 2, SampleFormat::S16, 0)}, RATE / 2);
}

// A playlist of two formats: the loop starts over with GroupInput::set_format() as a queued file
// does, and the player's stream goes on across each format change, the loop point included
TEST(Feeder, LoopsAPlaylistOfTwoFormatsGaplessly) {
    constexpr uint16_t FRAMES = 9600;
    expect_gapless_loop({std::make_shared<Positions>(FRAMES, SampleFormat::S16, 0),
                         std::make_shared<Positions>(FRAMES, SampleFormat::S24, FRAMES)},
                        2 * FRAMES);
}

}  // namespace
