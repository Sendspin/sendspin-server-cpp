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

/// @file source_sink_rig.h
/// @brief SinkRig, a live input a LiveSourceSink writes into and the rig reads back, with the
/// source's test tone

#pragma once

#include "dispatcher.h"
#include "host_view.h"
#include "job_view.h"
#include "pcm_convert.h"
#include "source_sink.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

namespace sendspin::server::test {

/// The source's write jitter, the staleness bound its chunks are judged by
inline constexpr int64_t JITTER_US = 60000;
/// The path from capture end to receipt when nothing holds the audio back
inline constexpr int64_t PATH_US = 3000;

/// The rig's rate, and a source's 20 ms chunk at it
inline constexpr uint32_t RATE = 48000;
inline constexpr size_t CHUNK = RATE / 50;
/// The test tone's largest step between two frames at RATE: 100 Hz at half scale
inline const double TONE_STEP = 0.5 * 2.0 * std::numbers::pi * 100.0 / RATE;

/// A source's tone at frame position, as a 16-bit sample
inline int16_t tone(uint64_t position) {
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
        const JobView job = job_view_of(*this->view);
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
            to_float(bytes, format, floats);
            this->staged.insert(this->staged.end(), floats.begin(), floats.end());
        }
        this->read_to_ = job.head;
        this->view->release_before(job.run.epoch, job.head);
    }

    uint32_t rate_;
    bool anchored_{false};
    uint64_t read_to_{0};
};

}  // namespace sendspin::server::test
