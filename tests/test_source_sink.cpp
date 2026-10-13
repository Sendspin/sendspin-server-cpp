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

/// @file test_source_sink.cpp
/// @brief What a routed source's capture times do on the live input (roles/source/v1.md "Source
/// Audio Chunks (Binary)"): judge_source_chunk(), the backlog of a stall dropped and a long gap a
/// new run; and LiveSourceSink, which passes a source on the server's clock through unaltered and
/// corrects one whose clock drifts, with the build's correction

#include "dispatcher.h"
#include "host_view.h"
#include "pcm_convert.h"
#include "profile.h"
#include "source_sink.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::judge_source_chunk;
using sendspin::server::SOURCE_RESTART_GAP_US;
using sendspin::server::SourceChunkAction;
using sendspin::server::SourceTiming;

constexpr int64_t CHUNK_US = 20000;
constexpr int64_t JITTER_US = 60000;
/// The path from capture end to receipt when nothing holds the audio back
constexpr int64_t PATH_US = 3000;

/// One chunk: captured at capture_us, received late_us after its path's usual time
struct Chunk {
    int64_t capture_us;
    int64_t late_us;
    SourceChunkAction action;
};

void expect_actions(const std::vector<Chunk>& chunks) {
    SourceTiming timing;
    for (size_t i = 0; i < chunks.size(); ++i) {
        SCOPED_TRACE(i);
        const Chunk& chunk = chunks[i];
        const int64_t end = chunk.capture_us + CHUNK_US;
        EXPECT_EQ(judge_source_chunk(timing, chunk.capture_us, end, end + PATH_US + chunk.late_us,
                                     JITTER_US),
                  chunk.action);
    }
}

TEST(SourceChunkJudgement, WritesAudioArrivingWithinTheJitter) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {20000, JITTER_US, SourceChunkAction::WRITE},
        {40000, 5000, SourceChunkAction::WRITE},
        // Capture times that overlap or step back are written, not cut points
        {50000, 0, SourceChunkAction::WRITE},
    });
}

// A stall's backlog arrives later than the jitter allows and is dropped, up to the point where
// live capture resumes on time; the gap it leaves is shorter than a restart's
TEST(SourceChunkJudgement, DropsAStallsBacklog) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {20000, 300000, SourceChunkAction::DROP},
        {40000, 280000, SourceChunkAction::DROP},
        {60000, JITTER_US + 1, SourceChunkAction::DROP},
        {80000, 0, SourceChunkAction::WRITE},
    });
}

// A gap of SOURCE_RESTART_GAP_US or more in the capture times, as after a backlog the source
// dropped itself, starts a new run; one just short of it does not
TEST(SourceChunkJudgement, RestartsTheRunAfterALongGap) {
    expect_actions({
        {0, 0, SourceChunkAction::WRITE},
        {CHUNK_US + SOURCE_RESTART_GAP_US - 1, 0, SourceChunkAction::WRITE},
        {2 * CHUNK_US + 2 * SOURCE_RESTART_GAP_US - 1, 0, SourceChunkAction::RESTART},
    });
}

// The spec's "Source timestamps ... may show discontinuities": every chunk late from some point
// on, for longer than a restart's gap, is a jump in the source's time mapping, not a backlog. The
// baseline restarts there and the audio plays again, in a new run.
TEST(SourceChunkJudgement, RebasesAfterStaleAudioOutlastsARestartsGap) {
    std::vector<Chunk> chunks{{0, 0, SourceChunkAction::WRITE}};
    const int64_t jump = 500000;
    int64_t capture = CHUNK_US;
    for (int64_t stale = 0; stale + CHUNK_US <= SOURCE_RESTART_GAP_US; stale += CHUNK_US) {
        chunks.push_back({capture, jump, SourceChunkAction::DROP});
        capture += CHUNK_US;
    }
    chunks.push_back({capture, jump, SourceChunkAction::RESTART});
    chunks.push_back({capture + CHUNK_US, jump, SourceChunkAction::WRITE});
    expect_actions(chunks);
}

// ==============================================================================
// LiveSourceSink
// ==============================================================================

using sendspin::server::Dispatcher;
using sendspin::server::GroupId;
using sendspin::server::GroupParams;
using sendspin::server::HostView;
using sendspin::server::InputId;
using sendspin::server::InputKind;
using sendspin::server::InputOptions;
using sendspin::server::LiveSourceSink;
using sendspin::server::ResamplerQuality;
using sendspin::server::SampleFormat;

constexpr uint32_t RATE = 48000;
constexpr size_t CHUNK = RATE / 50;
/// The test tone's largest step between two frames at RATE: 100 Hz at half scale
const double TONE_STEP = 0.5 * 2.0 * std::numbers::pi * 100.0 / RATE;

/// A source's tone at frame position, as a 16-bit sample
int16_t tone(uint64_t position) {
    return static_cast<int16_t>(std::lround(
        16383.0 * std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(position) / RATE)));
}

/// A mono live input a LiveSourceSink writes into, whose staged audio the rig reads back as
/// floats and releases, as the pipelines would. Its group lead is raised past everything written
/// once the run is anchored, so the input never cuts a write to lower its latency.
class SinkRig {
public:
    explicit SinkRig(uint32_t rate = RATE, ResamplerQuality quality = ResamplerQuality::MEDIUM)
        : rate_(rate) {
        InputOptions options{{rate, 1, SampleFormat::S16}};
        options.kind = InputKind::LIVE;
        this->view = std::make_shared<HostView>(GroupId{1}, InputId{2}, options,
                                                GroupParams{300000, 0, WRITE_AHEAD_US},
                                                this->dispatcher, nullptr);
        this->sink = std::make_unique<LiveSourceSink>(this->view, JITTER_US, quality);
    }

    ~SinkRig() {
        this->dispatcher->finish();
        this->dispatcher->join();
    }

    SinkRig(const SinkRig&) = delete;
    SinkRig& operator=(const SinkRig&) = delete;

    /// One chunk of the tone from position, captured at capture_us and received on its path
    void write(uint64_t position, size_t frames, int64_t capture_us, int64_t late_us = 0) {
        std::vector<int16_t> pcm(frames);
        for (size_t i = 0; i < frames; ++i) {
            pcm[i] = tone(position + i);
        }
        const int64_t end = capture_us + static_cast<int64_t>(frames) * 1000000 / this->rate_;
        this->sink->write(reinterpret_cast<const uint8_t*>(pcm.data()), frames, capture_us,
                          end + PATH_US + late_us);
        if (!this->anchored_) {
            this->view->set_group_params({LEAD_PAST_ALL_US, 0, WRITE_AHEAD_US});
            this->anchored_ = true;
        }
        this->collect();
    }

    std::shared_ptr<Dispatcher> dispatcher = std::make_shared<Dispatcher>();
    std::shared_ptr<HostView> view;
    std::unique_ptr<LiveSourceSink> sink;
    /// What the input staged, as floats, unless only counted
    std::vector<float> staged;
    bool count_only{false};
    uint64_t staged_count{0};

private:
    static constexpr int64_t WRITE_AHEAD_US = int64_t{1000} * 1000000;
    static constexpr int64_t LEAD_PAST_ALL_US = int64_t{1000} * 1000000;

    void collect() {
        const sendspin::server::JobView job = this->view->job_view();
        if (job.head <= this->read_to_) {
            return;
        }
        const auto frames = static_cast<size_t>(job.head - this->read_to_);
        this->staged_count += frames;
        if (!this->count_only) {
            std::vector<uint8_t> bytes;
            ASSERT_TRUE(this->view->read(job.run.epoch, this->read_to_, frames, bytes));
            const SampleFormat format = this->view->format().format.sample_format;
            std::vector<float> floats(frames);
            sendspin::server::to_float(bytes, format, floats);
            this->staged.insert(this->staged.end(), floats.begin(), floats.end());
        }
        this->read_to_ = job.head;
        this->view->release_before(job.run.epoch, job.head);
    }

    uint32_t rate_;
    bool anchored_{false};
    uint64_t read_to_{0};
};

/// Where staged and written first differ, past the start: where the correction began
size_t first_difference(const std::vector<float>& staged, const std::vector<float>& written) {
    size_t at = 0;
    while (at < staged.size() && at < written.size() && staged[at] == written[at]) {
        ++at;
    }
    return at;
}

// A source whose capture times keep its frame count's pace is staged unaltered, a stale chunk
// dropped and its time moving the reference along with it. One whose clock drifts is corrected
// once its phase error passes SOURCE_CLOCK_ENGAGE_US: a fast clock loses frames, a slow one gains
// them, at most SOURCE_CLOCK_MAX_PPM. Where the correction begins, the staged tone continues from
// the last unaltered frame: the ASRC's output lines up frame for frame with the source's, to its
// filter's error, and a slip drops or repeats one frame. Fifteen seconds of a model source's 20 ms
// chunks, written at once: the loop needs about five to reach its clamp and the rest to stage a
// count clear of the ASRC's hold.
TEST(LiveSourceSink, CorrectsOnlyASourceWhoseClockDrifts) {
    struct Row {
        std::string name;
        /// The source's clock against the server's: positive runs fast
        double drift_ppm;
        /// A 3 ms chunk received late enough to be stale, after this many chunks
        std::optional<int> stale;
        /// Frames staged less frames written: the bounds
        int64_t least;
        int64_t most;
    };
    constexpr int CHUNKS = 750;
    constexpr int64_t WRITTEN = int64_t{CHUNKS} * CHUNK;
    // At the largest correction for the whole run, plus what the ASRC holds; a third of it is a
    // floor well clear of the hold
    const auto full = static_cast<int64_t>(WRITTEN * sendspin::server::SOURCE_CLOCK_MAX_PPM / 1e6);
    const int64_t floor = full / 3;
    const Row rows[] = {
        {"Control: a source on the server's clock", 0.0, {}, 0, 0},
        {"A stale chunk is dropped", 0.0, 300, 0, 0},
        {"A fast clock is corrected by losing frames", 2000.0, {}, -full - 64, -floor},
        {"A slow clock is corrected by gaining frames", -2000.0, {}, floor, full + 64},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        SinkRig rig;
        ASSERT_TRUE(rig.sink->begin({RATE, 1, SampleFormat::S16}));
        std::vector<float> written;
        uint64_t position = 0;
        int64_t capture = 1000000;
        for (int k = 0; k < CHUNKS; ++k) {
            if (row.stale == k) {
                constexpr size_t SHORT = RATE * 3 / 1000;
                rig.write(position, SHORT, capture, 500000);
                position += SHORT;
                capture += 3000;
            }
            rig.write(position, CHUNK, capture);
            for (size_t i = 0; i < CHUNK; ++i) {
                written.push_back(static_cast<float>(tone(position + i)) / 32768.0F);
            }
            position += CHUNK;
            capture += std::llround(20000.0 / (1.0 + (row.drift_ppm * 1e-6)));
        }
        const auto difference =
            static_cast<int64_t>(rig.staged.size()) - static_cast<int64_t>(written.size());
        EXPECT_GE(difference, row.least);
        EXPECT_LE(difference, row.most);
        if (row.least == 0 && row.most == 0) {
            EXPECT_EQ(rig.staged, written);
            continue;
        }
        const size_t begun = first_difference(rig.staged, written);
        ASSERT_LT(begun + 200, rig.staged.size());
        ASSERT_GT(begun, 0U);
        for (size_t i = begun; i < begun + 200; ++i) {
            SCOPED_TRACE(i);
            if (sendspin::server::profile::SOURCE_ASRC) {
                // 200 frames at the clamp move the source's by a tenth of a frame
                EXPECT_NEAR(rig.staged[i], written[i], (0.1 * TONE_STEP) + 0.002);
            } else {
                // The slip that begins the correction, and nothing else in 200 frames
                EXPECT_EQ(rig.staged[i], row.drift_ppm > 0 ? written[i + 1] : written[i - 1]);
            }
        }
    }
}

// A source whose clock drifts within the correction's reach is locked onto: after the loop
// settles, the input stages as many fewer (or more) frames than the source sent as its clock's
// drift, measured on the corrected audio rather than on the source's. A hundred and sixty seconds
// of a model source's 20 ms chunks at 8 kHz, written at once: the loop takes about a hundred to
// settle; the rate and the ASRC's quality only set the cost.
TEST(LiveSourceSink, LocksOntoADriftWithinItsReach) {
    constexpr uint32_t rate = 8000;
    constexpr size_t chunk = rate / 50;
    constexpr int CHUNKS = 8000;
    constexpr int SETTLED = 6000;
    for (const double drift_ppm : {300.0, -300.0}) {
        SCOPED_TRACE(drift_ppm);
        SinkRig rig(rate, ResamplerQuality::LOW);
        rig.count_only = true;
        ASSERT_TRUE(rig.sink->begin({rate, 1, SampleFormat::S16}));
        uint64_t settled_at = 0;
        int64_t capture = 1000000;
        for (int k = 0; k < CHUNKS; ++k) {
            if (k == SETTLED) {
                settled_at = rig.staged_count;
            }
            rig.write(uint64_t{chunk} * static_cast<uint64_t>(k), chunk, capture);
            capture += std::llround(20000.0 / (1.0 + (drift_ppm * 1e-6)));
        }
        const auto written = static_cast<double>(CHUNKS - SETTLED) * chunk;
        const double correction_ppm =
            (1.0 - (static_cast<double>(rig.staged_count - settled_at) / written)) * 1e6;
        EXPECT_NEAR(correction_ppm, drift_ppm, 50.0);
    }
}

}  // namespace
