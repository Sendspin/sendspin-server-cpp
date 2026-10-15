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

/// @file test_group_audio.cpp
/// @brief A group's pipelines on their own: what they post to core, in order, as the host view
/// moves through epochs, through the channel mix and the resampler as well. Jobs and the FIFO's
/// handler run on the test thread. White-box: swaps an instance's encoder to show a pipeline
/// failure, cuts a stretch directly and sets a paused position (see tests/CMakeLists.txt).

#include "clock.h"
#include "dispatcher.h"
#include "flac_reader.h"
#include "frame_encoder.h"
#include "group_audio.h"
#include "host_view.h"
#include "job_view.h"
#include "opus_reader.h"
#include "resampler_reference.h"
#include "sched/leads.h"
#include "transport_asio.h"
#include "visualizer_analysis.h"
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {

using sendspin::server::DrainStarted;
using sendspin::server::EpochCut;
using sendspin::server::EpochEnded;
using sendspin::server::EpochStarted;
using sendspin::server::FrameReady;
using sendspin::server::GroupEvent;
using sendspin::server::GroupId;
using sendspin::server::HostView;
using sendspin::server::InputFormat;
using sendspin::server::InputId;
using sendspin::server::InstanceFinal;
using sendspin::server::now_us;
using sendspin::server::SampleFormat;
using sendspin::server::test::job_view_of;
using sendspin::server::test::ReferenceResampler;
namespace sched = sendspin::server::sched;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t RATE = 48000;
constexpr uint64_t CELL = RATE / 50;
constexpr int64_t LEAD_US = 300000;
constexpr sched::InstanceId INSTANCE{7};

class GroupAudioTest : public testing::Test {
protected:
    GroupAudioTest()
        : view(std::make_shared<HostView>(
              GroupId{1}, InputId{1},
              sendspin::server::InputOptions{InputFormat{RATE, 1, SampleFormat::S16}},
              sendspin::server::GroupParams{LEAD_US, 0, 1000000},
              std::make_shared<sendspin::server::Dispatcher>(), nullptr)),
          fifo(this->loop, [this](GroupEvent& event) { this->events.push_back(event); }),
          audio(this->view, this->fifo.producer(), this->slot,
                sendspin::server::ResamplerQuality::MEDIUM) {
        // One 24-bit mono instance from the start
        this->slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}});
    }

    /// Writes frames samples whose values count up from first
    void write(uint64_t frames, uint16_t first) {
        Bytes pcm(frames * 2);
        for (uint64_t i = 0; i < frames; ++i) {
            const auto value = static_cast<uint16_t>(first + i);
            pcm[i * 2] = static_cast<uint8_t>(value);
            pcm[(i * 2) + 1] = static_cast<uint8_t>(value >> 8);
        }
        ASSERT_EQ(this->view->write(pcm.data(), frames).frames, frames);
    }

    /// Runs jobs while they make progress, then delivers what they posted
    /// @param late_us how far past now_us() the jobs run
    std::vector<GroupEvent> run(int64_t late_us = 0) {
        while (this->audio.run_job(now_us() + late_us)) {}
        this->loop.impl().io().restart();
        this->loop.impl().io().poll();
        return std::exchange(this->events, {});
    }

    /// One letter per event: Cut, Started, Ended, Anchor shift, Drain, Frame, final, Pipeline
    /// failed
    static std::string kinds(const std::vector<GroupEvent>& events) {
        std::string out;
        for (const GroupEvent& event : events) {
            out += "CSEADFfP"[event.index()];
        }
        return out;
    }

    sendspin::server::EventLoop loop;
    std::shared_ptr<HostView> view;
    std::vector<GroupEvent> events;
    sendspin::server::GroupToCoreFifo fifo;
    std::shared_ptr<sendspin::server::PipelineSetSlot> slot =
        std::make_shared<sendspin::server::PipelineSetSlot>();
    sendspin::server::GroupAudio audio;
};

// The epoch's start, then one frame per whole cell at its timestamp; a drain adds the short last
// frame and the instance's final index
TEST_F(GroupAudioTest, CutsFramesOnTheCellGridThroughADrain) {
    this->write(3 * CELL, 0);
    std::vector<GroupEvent> events = this->run();
    ASSERT_EQ(kinds(events), "SFFF");
    const sched::Anchor& anchor = std::get<EpochStarted>(events[0]).anchor;
    for (int64_t k = 0; k < 3; ++k) {
        const sched::Frame& frame = std::get<FrameReady>(events[1 + k]).frame;
        EXPECT_EQ(std::get<FrameReady>(events[1 + k]).instance, INSTANCE);
        EXPECT_EQ(frame.index, k);
        EXPECT_EQ(frame.ts_us, anchor.t0_us + (k * 20000));
        EXPECT_EQ(frame.duration_us, 20000);
        // 16-bit samples shifted up to 24 bits, little-endian
        ASSERT_EQ(frame.payload.size(), CELL * 3);
        EXPECT_EQ(frame.payload[3], 0);
        EXPECT_EQ(frame.payload[4], static_cast<uint8_t>((k * CELL) + 1));
    }

    // DrainStarted once the last frame is produced, so no rebase follows
    this->write((2 * CELL) + 100, static_cast<uint16_t>(3 * CELL));
    this->view->drain();
    events = this->run();
    ASSERT_EQ(kinds(events), "FFFDf");
    EXPECT_EQ(std::get<DrainStarted>(events[3]).last_cell, 5);
    const sched::Frame& last = std::get<FrameReady>(events[2]).frame;
    EXPECT_EQ(last.index, 5);
    EXPECT_EQ(last.samples, 100U);
    // ceil(100 / 48,000 s)
    EXPECT_EQ(last.duration_us, 2084);
    EXPECT_EQ(std::get<InstanceFinal>(events[4]).final_index, 5);
    EXPECT_EQ(this->run().size(), 0U);
}

/// Each instance's frames among events, in order
std::map<sched::InstanceId, std::vector<sched::Frame>> frames_by_instance(
    const std::vector<GroupEvent>& events) {
    std::map<sched::InstanceId, std::vector<sched::Frame>> out;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            out[ready->instance].push_back(ready->frame);
        }
    }
    return out;
}

// A FLAC instance beside a PCM one cuts the same cells at the same timestamps. Its frames, numbered
// from 0 in each epoch, decode through one decoder to the PCM instance's samples, which are the
// host's, bit for bit: the drained epoch's short last frame included (FLAC finish()). An instance
// started mid-epoch has its own encoder, so its first frame is numbered 0.
TEST_F(GroupAudioTest, EncodesFlacFramesBesidePcmOnes) {
    constexpr sched::InstanceId PCM{8};
    constexpr sched::InstanceId FLAC{9};
    constexpr sched::InstanceId JOINER{10};
    const sched::AudioFormat flac{sched::Codec::FLAC, RATE, 1, 16};
    this->slot->publish({{PCM, {}, {sched::Codec::PCM, RATE, 1, 16}, 0}, {FLAC, {}, flac, 0}});
    sendspin::server::test::FlacReader reader;
    ASSERT_TRUE(reader.open(*sendspin::server::codec_header(flac)));
    sendspin::server::test::FlacReader joiner_reader;
    ASSERT_TRUE(joiner_reader.open(*sendspin::server::codec_header(flac)));

    // Each FLAC frame against the PCM frame of its index
    const auto check =
        [&](const std::vector<sched::Frame>& encoded, const std::vector<sched::Frame>& pcm,
            sendspin::server::test::FlacReader& decoder, const std::vector<uint64_t>& numbers) {
            ASSERT_EQ(encoded.size(), numbers.size());
            for (size_t i = 0; i < encoded.size(); ++i) {
                SCOPED_TRACE(encoded[i].index);
                const auto same = std::find_if(pcm.begin(), pcm.end(), [&](const sched::Frame& f) {
                    return f.index == encoded[i].index;
                });
                ASSERT_NE(same, pcm.end());
                EXPECT_EQ(encoded[i].ts_us, same->ts_us);
                EXPECT_EQ(encoded[i].samples, same->samples);
                EXPECT_EQ(encoded[i].duration_us, same->duration_us);
                // Compressed: the ramp is predictable
                EXPECT_LT(encoded[i].payload.size(), same->payload.size());
                const auto decoded = decoder.decode(encoded[i].payload);
                ASSERT_TRUE(decoded);
                ASSERT_EQ(decoded->size(), 1U);
                EXPECT_EQ(decoded->front().number, numbers[i]);
                EXPECT_EQ(decoded->front().pcm, same->payload);
            }
        };

    this->write(3 * CELL, 0);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(this->run());
    ASSERT_EQ(frames[PCM].size(), 3U);
    check(frames[FLAC], frames[PCM], reader, {0, 1, 2});

    // A flush: the next epoch's frames are numbered from 0 again, through the same decoder
    this->view->flush();
    this->write(2 * CELL, 1000);
    std::vector<GroupEvent> events = this->run();
    ASSERT_EQ(kinds(events).substr(0, 2), "CS");
    const sched::EpochId epoch = std::get<EpochStarted>(events[1]).epoch;
    std::map<sched::InstanceId, std::vector<sched::Frame>> second = frames_by_instance(events);

    // A joiner's instance from cell 1 of the playing epoch
    this->slot->publish({{PCM, {}, {sched::Codec::PCM, RATE, 1, 16}, 0},
                         {FLAC, {}, flac, 0},
                         {JOINER, epoch, flac, 1}});
    std::map<sched::InstanceId, std::vector<sched::Frame>> joined = frames_by_instance(this->run());
    ASSERT_EQ(joined[JOINER].size(), 1U);
    EXPECT_EQ(joined[JOINER].front().index, 1);

    // The drain's short last frame: 100 samples, ending each FLAC stream
    this->write(CELL + 100, 3000);
    this->view->drain();
    events = this->run();
    EXPECT_EQ(kinds(events), "FFFFFFDfff");
    for (auto& [instance, list] : frames_by_instance(events)) {
        auto& all = instance == JOINER ? joined[instance] : second[instance];
        all.insert(all.end(), list.begin(), list.end());
    }
    ASSERT_EQ(second[PCM].size(), 4U);
    EXPECT_EQ(second[PCM].back().samples, 100U);
    check(second[FLAC], second[PCM], reader, {0, 1, 2, 3});
    check(joined[JOINER], second[PCM], joiner_reader, {0, 1, 2});
}

// A flush is a cut in the same run, anchored by the next write; a pause ends the run, and the
// resume starts another from the paused position
TEST_F(GroupAudioTest, FollowsFlushPauseAndResume) {
    this->write(CELL, 0);
    std::vector<GroupEvent> events = this->run();
    ASSERT_EQ(kinds(events), "SF");
    const EpochStarted first = std::get<EpochStarted>(events[0]);

    this->view->flush();
    events = this->run();
    ASSERT_EQ(kinds(events), "C");
    EXPECT_GT(std::get<EpochCut>(events[0]).epoch, first.epoch);

    this->write(2 * CELL, 500);
    events = this->run();
    ASSERT_EQ(kinds(events), "SFF");
    const EpochStarted flushed = std::get<EpochStarted>(events[0]);
    EXPECT_EQ(flushed.run, first.run);
    EXPECT_EQ(std::get<FrameReady>(events[1]).frame.index, 0);

    // Paused before the epoch played: the resume starts from its first position
    EXPECT_EQ(this->view->pause(), 0U);
    events = this->run();
    ASSERT_EQ(kinds(events), "E");
    EXPECT_EQ(std::get<EpochEnded>(events[0]).epoch, flushed.epoch);
    this->view->resume();
    events = this->run();
    ASSERT_EQ(kinds(events), "SFF");
    EXPECT_NE(std::get<EpochStarted>(events[0]).run, first.run);
    // The same audio again, from the start of the resumed epoch
    EXPECT_EQ(std::get<FrameReady>(events[1]).frame.payload[1], static_cast<uint8_t>(500 & 0xFF));
    EXPECT_EQ(std::get<FrameReady>(events[1]).frame.index, 0);

    this->view->stop();
    EXPECT_EQ(kinds(this->run()), "E");
}

// A drain on a pending epoch with no input has nothing to play and ends the run at once, whether
// the epoch is a flush's or a resume's; the next write begins a new run
TEST_F(GroupAudioTest, EndsTheRunForADrainWithNothingToPlay) {
    this->write(CELL, 0);
    std::vector<GroupEvent> events = this->run();
    ASSERT_EQ(kinds(events), "SF");
    const sched::RunId first = std::get<EpochStarted>(events[0]).run;
    // The pipelines see only where the calls left the view: the run ended
    this->view->flush();
    this->view->drain();
    ASSERT_EQ(kinds(this->run()), "E");
    this->write(CELL, 0);
    events = this->run();
    ASSERT_EQ(kinds(events), "SF");
    const sched::RunId second = std::get<EpochStarted>(events[0]).run;
    EXPECT_NE(second, first);

    this->view->flush();
    this->view->pause();
    this->view->drain();
    this->view->resume();
    ASSERT_EQ(kinds(this->run()), "E");
    this->write(CELL, 0);
    events = this->run();
    ASSERT_EQ(kinds(events), "SF");
    EXPECT_NE(std::get<EpochStarted>(events[0]).run, second);
}

// A buffered input whose next frame would be late shifts the timeline at that frame by what
// restores the full group lead, in the host view as well; a frame on time does not
TEST_F(GroupAudioTest, ShiftsAStalledInputBackToTheFullGroupLead) {
    this->write(CELL, 0);
    std::vector<GroupEvent> events = this->run();
    ASSERT_EQ(kinds(events), "SF");
    const int64_t t0 = std::get<EpochStarted>(events[0]).anchor.t0_us;

    // Control: the next frame is still a group lead ahead
    this->write(CELL, 0);
    events = this->run();
    ASSERT_EQ(kinds(events), "F");
    EXPECT_EQ(std::get<FrameReady>(events[0]).frame.ts_us, t0 + 20000);

    // The host stalled for 2 s before the third cell
    constexpr int64_t STALL_US = 2000000;
    this->write(CELL, 0);
    const int64_t before = now_us() + STALL_US;
    events = this->run(STALL_US);
    const int64_t after = now_us() + STALL_US;
    ASSERT_EQ(kinds(events), "AF");
    const auto& shift = std::get<sendspin::server::AnchorShift>(events[0]);
    EXPECT_EQ(shift.index, 2);
    const sched::Frame& frame = std::get<FrameReady>(events[1]).frame;
    EXPECT_EQ(frame.index, 2);
    EXPECT_EQ(frame.ts_us, t0 + 40000 + shift.delta_us);
    EXPECT_GE(frame.ts_us, before + LEAD_US);
    EXPECT_LE(frame.ts_us, after + LEAD_US);
    // The host view maps positions with the shift
    EXPECT_EQ(this->view->time_of(2 * CELL), frame.ts_us);
    EXPECT_EQ(this->view->time_of(CELL), t0 + 20000);
}

// With no instance there is no next frame to rebase, so admission takes the plain horizon; a k*
// left from an instance that is gone would hold the input short for good once its lead passed
TEST_F(GroupAudioTest, LeavesThePlainHorizonWhenNoInstanceProduces) {
    this->write(CELL, 0);
    ASSERT_EQ(kinds(this->run()), "SF");
    // The last player left: core publishes an empty pipeline set and the next job runs
    this->slot->publish({});
    EXPECT_EQ(kinds(this->run()), "");
    // The host fills the write-ahead, then waits past the lead of the frame that was next
    Bytes block(CELL * 2, 0);
    while (this->view->write(block.data(), CELL).frames == CELL) {}
    std::this_thread::sleep_for(std::chrono::microseconds(LEAD_US + 100000));
    EXPECT_EQ(this->view->wait_writable(0), sendspin::server::InputState::OPEN);
}

// ==============================================================================
// Channel mix and resampling
// ==============================================================================

using sendspin::server::PipelineFailed;
using sendspin::server::PipelineRequest;

/// A group's pipelines over an input of any format and kind
struct Pipelines {
    explicit Pipelines(const InputFormat& format, int64_t write_ahead_us = 1000000)
        : Pipelines(sendspin::server::InputOptions{format}, write_ahead_us) {}

    explicit Pipelines(const sendspin::server::InputOptions& options,
                       int64_t write_ahead_us = 1000000)
        : view(std::make_shared<HostView>(GroupId{1}, InputId{1}, options,
                                          sendspin::server::GroupParams{LEAD_US, 0, write_ahead_us},
                                          std::make_shared<sendspin::server::Dispatcher>(),
                                          nullptr)),
          fifo(this->loop, [this](GroupEvent& event) { this->events.push_back(event); }),
          audio(this->view, this->fifo.producer(), this->slot,
                sendspin::server::ResamplerQuality::MEDIUM) {}

    /// @param late_us how far past now_us() the jobs run
    std::vector<GroupEvent> run(int64_t late_us = 0) {
        while (this->audio.run_job(now_us() + late_us)) {}
        this->loop.impl().io().restart();
        this->loop.impl().io().poll();
        return std::exchange(this->events, {});
    }

    sendspin::server::EventLoop loop;
    std::shared_ptr<HostView> view;
    std::vector<GroupEvent> events;
    sendspin::server::GroupToCoreFifo fifo;
    std::shared_ptr<sendspin::server::PipelineSetSlot> slot =
        std::make_shared<sendspin::server::PipelineSetSlot>();
    sendspin::server::GroupAudio audio;
};

/// A little-endian signed sample of bytes bytes
int32_t sample_at(const Bytes& payload, size_t index, size_t bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= uint32_t{payload[(index * bytes) + i]} << (8 * i);
    }
    const uint32_t sign = 1U << ((8 * bytes) - 1);
    return static_cast<int32_t>(value ^ sign) - static_cast<int32_t>(sign);
}

/// Every frame's samples of one instance, concatenated
Bytes concatenated(const std::vector<sched::Frame>& frames) {
    Bytes out;
    for (const sched::Frame& frame : frames) {
        out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    }
    return out;
}

/// micro-resampler run directly, as the pipelines configure it, over a whole stream: primed with
/// the first block, fed in blocks of block frames, flushed, then cut to `round(N * out / in)`
std::vector<float> resample_offline(const std::vector<float>& input, uint32_t channels,
                                    uint32_t in_rate, uint32_t out_rate, size_t block) {
    auto resampler = std::make_unique<ReferenceResampler>(in_rate, out_rate, channels, block);
    EXPECT_TRUE(resampler->valid());
    const size_t frames = input.size() / channels;
    const size_t capacity = (block * out_rate / in_rate) + 2;
    std::vector<float> scratch(capacity * channels);
    std::vector<float> out;
    EXPECT_TRUE(resampler->prime(input.data(), std::min(block, frames)));
    for (size_t at = 0; at < frames;) {
        size_t consumed = 0;
        size_t generated = 0;
        EXPECT_TRUE(resampler->process_block(input.data() + (at * channels),
                                             std::min(block, frames - at), scratch.data(), capacity,
                                             consumed, generated));
        out.insert(out.end(), scratch.begin(),
                   scratch.begin() + static_cast<std::ptrdiff_t>(generated * channels));
        at += consumed;
    }
    while (!resampler->is_flushed()) {
        size_t generated = 0;
        EXPECT_TRUE(resampler->flush(scratch.data(), capacity, generated));
        out.insert(out.end(), scratch.begin(),
                   scratch.begin() + static_cast<std::ptrdiff_t>(generated * channels));
        if (generated == 0) {
            break;
        }
    }
    const uint64_t total = ((uint64_t{frames} * 2 * out_rate) + in_rate) / (2 * uint64_t{in_rate});
    EXPECT_GE(out.size(), total * channels);
    out.resize(total * channels);
    return out;
}

/// Each instance's InstanceFinal among events
std::map<sched::InstanceId, int64_t> final_indices(const std::vector<GroupEvent>& events) {
    std::map<sched::InstanceId, int64_t> out;
    for (const GroupEvent& event : events) {
        if (const auto* final = std::get_if<InstanceFinal>(&event)) {
            out[final->instance] = final->final_index;
        }
    }
    return out;
}

/// The last cell a drain reports, alone or, staged before the epoch's first job, with its start
std::optional<int64_t> drained_last_cell(const std::vector<GroupEvent>& events) {
    for (const GroupEvent& event : events) {
        if (const auto* drain = std::get_if<DrainStarted>(&event)) {
            return drain->last_cell;
        }
        if (const auto* started = std::get_if<EpochStarted>(&event); started && started->drain) {
            return started->drain->last_cell;
        }
    }
    return std::nullopt;
}

/// One letter per event, as GroupAudioTest::kinds()
std::string kinds_of(const std::vector<GroupEvent>& events) {
    std::string out;
    for (const GroupEvent& event : events) {
        out += "CSEADFfP"[event.index()];
    }
    return out;
}

/// The quantize stage's rule for a float at 16 bits: rounded, clipped, no dither
int32_t quantize16(float sample) {
    const double scaled = std::clamp(static_cast<double>(sample), -2.0, 2.0) * 32768.0;
    return static_cast<int32_t>(std::clamp<int64_t>(std::llround(scaled), -32768, 32767));
}

// From a 44.1 kHz stereo input, every instance cuts the same cells at the same timestamps: 882
// samples a cell at 44.1 kHz and 960 at 48 kHz, as PCM or FLAC, stereo or mixed to mono. 120,406
// input samples (2.7 s, which either profile's staging ring holds) drained give 136 full cells and
// a last one of 454 input samples, which the resamplers flush and cut to exactly round(120,406 *
// 48,000 / 44,100) = 131,054 samples: a last frame of 494. The resampled frames are
// micro-resampler's output run directly over the whole input, quantized, sample for sample; the
// FLAC frames decode to the PCM ones; the mono frames are the average of each frame's channels.
TEST(GroupAudioPipelines, CutEveryFormatOnTheCellGridThroughADrain) {
    constexpr uint32_t IN_RATE = 44100;
    constexpr uint64_t SAMPLES = 120406;
    static_assert(SAMPLES * 1000000 / IN_RATE < sendspin::server::STAGING_RING_US);
    Pipelines p(InputFormat{IN_RATE, 2, SampleFormat::S16}, 10000000);
    const sched::AudioFormat pcm44{sched::Codec::PCM, IN_RATE, 2, 16};
    const sched::AudioFormat pcm48{sched::Codec::PCM, 48000, 2, 16};
    const sched::AudioFormat flac48{sched::Codec::FLAC, 48000, 2, 16};
    const sched::AudioFormat mono44{sched::Codec::PCM, IN_RATE, 1, 16};
    const sched::AudioFormat mono48{sched::Codec::PCM, 48000, 1, 16};
    const std::vector<sched::AudioFormat> formats = {pcm44, pcm48, flac48, mono44, mono48};
    std::vector<PipelineRequest> set;
    for (size_t i = 0; i < formats.size(); ++i) {
        set.push_back({sched::InstanceId{20 + i}, {}, formats[i], 0});
    }
    p.slot->publish(set);

    // Two sines a channel, so the resampler has something to filter
    Bytes pcm(SAMPLES * 4);
    std::vector<float> floats(SAMPLES * 2);
    for (uint64_t i = 0; i < SAMPLES; ++i) {
        for (uint64_t channel = 0; channel < 2; ++channel) {
            const double t = static_cast<double>(i) / IN_RATE;
            const auto value = static_cast<int16_t>(
                std::lround((12000.0 * std::sin(2 * M_PI * (440.0 + (110.0 * channel)) * t)) +
                            (4000.0 * std::sin(2 * M_PI * 9000.0 * t))));
            const auto bits = static_cast<uint16_t>(value);
            pcm[(i * 4) + (channel * 2)] = static_cast<uint8_t>(bits);
            pcm[(i * 4) + (channel * 2) + 1] = static_cast<uint8_t>(bits >> 8);
            floats[(i * 2) + channel] = static_cast<float>(value) / 32768.0F;
        }
    }
    ASSERT_EQ(p.view->write(pcm.data(), SAMPLES).frames, SAMPLES);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    const sched::Anchor& anchor = std::get<EpochStarted>(events.front()).anchor;
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(drained_last_cell(events), 136);
    for (const GroupEvent& event : events) {
        EXPECT_FALSE(std::holds_alternative<PipelineFailed>(event));
    }

    struct Row {
        sched::AudioFormat format;
        uint32_t full;
        uint32_t last;
    };
    const Row rows[] = {
        {pcm44, 882, 454},  {pcm48, 960, 494},  {flac48, 960, 494},
        {mono44, 882, 454}, {mono48, 960, 494},
    };
    for (size_t i = 0; i < formats.size(); ++i) {
        const Row& row = rows[i];
        const sched::InstanceId id{20 + i};
        SCOPED_TRACE(i);
        const std::vector<sched::Frame>& list = frames[id];
        ASSERT_EQ(list.size(), 137U);
        uint64_t total = 0;
        for (size_t k = 0; k < list.size(); ++k) {
            EXPECT_EQ(list[k].index, static_cast<int64_t>(k));
            EXPECT_EQ(list[k].ts_us, anchor.t0_us + (static_cast<int64_t>(k) * 20000));
            EXPECT_EQ(list[k].samples, k + 1 < list.size() ? row.full : row.last);
            if (row.format.codec == sched::Codec::PCM) {
                EXPECT_EQ(list[k].payload.size(),
                          size_t{list[k].samples} * row.format.channels * 2);
            }
            total += list[k].samples;
        }
        EXPECT_EQ(total, row.format.sample_rate == IN_RATE ? SAMPLES : 131054U);
        EXPECT_EQ(finals.at(id), 136);
    }

    // The native instance is the input; the mono one its channels' average
    const Bytes native = concatenated(frames[sched::InstanceId{20}]);
    EXPECT_EQ(native, pcm);
    const Bytes mono = concatenated(frames[sched::InstanceId{23}]);
    for (uint64_t i = 0; i < SAMPLES; ++i) {
        const double average =
            (sample_at(native, i * 2, 2) + static_cast<double>(sample_at(native, (i * 2) + 1, 2))) /
            2.0;
        ASSERT_EQ(sample_at(mono, i, 2), std::lround(average)) << i;
    }

    // The resampled instance against micro-resampler over the whole input, fed in other blocks
    const std::vector<float> reference = resample_offline(floats, 2, IN_RATE, 48000, 1000);
    const Bytes resampled = concatenated(frames[sched::InstanceId{21}]);
    ASSERT_EQ(resampled.size(), reference.size() * 2);
    for (size_t i = 0; i < reference.size(); ++i) {
        ASSERT_EQ(sample_at(resampled, i, 2), quantize16(reference[i])) << i;
    }
    std::vector<float> mixed(SAMPLES);
    for (uint64_t i = 0; i < SAMPLES; ++i) {
        mixed[i] =
            static_cast<float>((static_cast<double>(floats[i * 2]) + floats[(i * 2) + 1]) / 2);
    }
    const std::vector<float> mono_reference = resample_offline(mixed, 1, IN_RATE, 48000, 1000);
    const Bytes mono_resampled = concatenated(frames[sched::InstanceId{24}]);
    ASSERT_EQ(mono_resampled.size(), mono_reference.size() * 2);
    for (size_t i = 0; i < mono_reference.size(); ++i) {
        ASSERT_EQ(sample_at(mono_resampled, i, 2), quantize16(mono_reference[i])) << i;
    }

    // The FLAC frames, the short last one included, decode to the PCM ones
    sendspin::server::test::FlacReader reader;
    ASSERT_TRUE(reader.open(*sendspin::server::codec_header(flac48)));
    const std::vector<sched::Frame>& pcm_frames = frames[sched::InstanceId{21}];
    const std::vector<sched::Frame>& flac_frames = frames[sched::InstanceId{22}];
    for (size_t k = 0; k < flac_frames.size(); ++k) {
        const auto decoded = reader.decode(flac_frames[k].payload);
        ASSERT_TRUE(decoded);
        ASSERT_EQ(decoded->size(), 1U);
        EXPECT_EQ(decoded->front().pcm, pcm_frames[k].payload) << k;
    }
}

/// Two sines a channel at 16 bits, from sample first on, and the same as the stages' floats
struct Tone {
    Tone(uint32_t rate, uint32_t channels, uint64_t samples, uint64_t first = 0)
        : pcm(samples * channels * 2), floats(samples * channels) {
        for (uint64_t i = 0; i < samples; ++i) {
            for (uint64_t channel = 0; channel < channels; ++channel) {
                const double t = static_cast<double>(first + i) / rate;
                const auto value = static_cast<int16_t>(
                    std::lround((12000.0 * std::sin(2 * M_PI * (440.0 + (110.0 * channel)) * t)) +
                                (4000.0 * std::sin(2 * M_PI * 3000.0 * t))));
                const auto bits = static_cast<uint16_t>(value);
                const uint64_t at = (i * channels) + channel;
                this->pcm[at * 2] = static_cast<uint8_t>(bits);
                this->pcm[(at * 2) + 1] = static_cast<uint8_t>(bits >> 8);
                this->floats[at] = static_cast<float>(value) / 32768.0F;
            }
        }
    }

    Bytes pcm;
    std::vector<float> floats;
};

/// Each frame of one instance: its index steps by cells from 0, its timestamp is its cell's on
/// the anchor, and every frame but the last holds full samples
/// @return the frames' samples, concatenated
Bytes expect_on_grid(const std::vector<sched::Frame>& frames, const sched::Anchor& anchor,
                     uint32_t cells, uint32_t full) {
    for (size_t i = 0; i < frames.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(frames[i].index, static_cast<int64_t>(i * cells));
        EXPECT_EQ(frames[i].ts_us, sched::frame_timestamp(anchor, frames[i].index));
        if (i + 1 < frames.size()) {
            EXPECT_EQ(frames[i].samples, full);
        }
    }
    return concatenated(frames);
}

/// Every 16-bit sample of out from at on is the reference's, quantized
void expect_resampled(const Bytes& out, size_t at, const std::vector<float>& reference) {
    ASSERT_GE(out.size() / 2, at + reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
        ASSERT_EQ(sample_at(out, at + i, 2), quantize16(reference[i])) << i;
    }
}

// At 11,025 Hz a frame spans two cells, 441 samples, and starts on the even cells, as PCM and as
// FLAC; 11,325 samples drained end in cell 51, so the last frame starts at cell 50 with the 300
// left. The PCM frames are the input, bit for bit, and the FLAC frames decode to them. The same
// input resampled to 48 kHz, where cells do not end on whole input samples, gives one-cell frames
// on the same timestamps: micro-resampler's output, cut to round(11,325 * 48,000 / 11,025) = 49,306
// samples.
TEST(GroupAudioPipelines, CutsTwoCellFramesAt11025Hz) {
    constexpr uint32_t IN_RATE = 11025;
    constexpr uint64_t SAMPLES = 11325;
    Pipelines p(InputFormat{IN_RATE, 2, SampleFormat::S16}, 10000000);
    const sched::InstanceId pcm{140};
    const sched::InstanceId flac{141};
    const sched::InstanceId resampled{142};
    const sched::AudioFormat flac11{sched::Codec::FLAC, IN_RATE, 2, 16};
    p.slot->publish({{pcm, {}, {sched::Codec::PCM, IN_RATE, 2, 16}, 0},
                     {flac, {}, flac11, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Tone tone(IN_RATE, 2, SAMPLES);
    ASSERT_EQ(p.view->write(tone.pcm.data(), SAMPLES).frames, SAMPLES);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    const sched::Anchor& anchor = std::get<EpochStarted>(events.front()).anchor;
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(drained_last_cell(events), 51);

    ASSERT_EQ(frames[pcm].size(), 26U);
    EXPECT_EQ(expect_on_grid(frames[pcm], anchor, 2, 441), tone.pcm);
    EXPECT_EQ(frames[pcm].back().samples, 300U);
    // A full frame lasts its two cells
    EXPECT_EQ(frames[pcm].front().duration_us, 40000);
    EXPECT_EQ(finals.at(pcm), 50);
    EXPECT_EQ(finals.at(flac), 50);
    sendspin::server::test::FlacReader reader;
    ASSERT_TRUE(reader.open(*sendspin::server::codec_header(flac11)));
    ASSERT_EQ(frames[flac].size(), frames[pcm].size());
    for (size_t i = 0; i < frames[flac].size(); ++i) {
        EXPECT_EQ(frames[flac][i].index, frames[pcm][i].index);
        EXPECT_EQ(frames[flac][i].ts_us, frames[pcm][i].ts_us);
        const auto decoded = reader.decode(frames[flac][i].payload);
        ASSERT_TRUE(decoded);
        ASSERT_EQ(decoded->size(), 1U);
        EXPECT_EQ(decoded->front().pcm, frames[pcm][i].payload) << i;
    }

    ASSERT_EQ(frames[resampled].size(), 52U);
    const Bytes out = expect_on_grid(frames[resampled], anchor, 1, 960);
    EXPECT_EQ(out.size() / 4, 49306U);
    EXPECT_EQ(finals.at(resampled), 51);
    expect_resampled(out, 0, resample_offline(tone.floats, 2, IN_RATE, 48000, 221));
}

/// The floats of a mono tone in every channel
std::vector<float> duplicated(const std::vector<float>& mono, uint32_t channels) {
    std::vector<float> out;
    for (const float sample : mono) {
        out.insert(out.end(), channels, sample);
    }
    return out;
}

/// The average of each stereo frame, as the channel mix rounds it
std::vector<float> averaged(const std::vector<float>& stereo) {
    std::vector<float> out(stereo.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<float>((static_cast<double>(stereo[i * 2]) + stereo[(i * 2) + 1]) / 2);
    }
    return out;
}

// A second of 44.1 kHz stereo, then a set_format() to 48 kHz mono and half a second of it, drained.
// Every instance keeps its format and its frames on the one cell grid, running each segment through
// what that segment needs; at the boundary (1 s, exactly on a cell) a resampler is flushed and its
// output cut to the boundary's count, and the next segment's resampler starts primed with its first
// block. So each instance's output is, segment by segment, the input as it is, duplicated into both
// channels, or micro-resampler's output over that segment alone: 44,100 + 22,050 samples at 44.1
// kHz, 48,000 + 24,000 at 48 kHz, and 11,025 + round(24,000 * 11,025 / 48,000) = 16,538 in 11,025
// Hz frames of two cells.
TEST(GroupAudioPipelines, RunsEachSegmentThroughTheStagesItNeeds) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
    const sched::InstanceId at44{150};
    const sched::InstanceId at48{151};
    const sched::InstanceId at11{152};
    p.slot->publish({{at44, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {at48, {}, {sched::Codec::PCM, 48000, 2, 16}, 0},
                     {at11, {}, {sched::Codec::PCM, 11025, 1, 16}, 0}});
    const Tone first(44100, 2, 44100);
    const Tone second(48000, 1, 24000, 48000);
    ASSERT_EQ(p.view->write(first.pcm.data(), 44100).frames, 44100U);
    ASSERT_TRUE(p.view->set_format({48000, 1, SampleFormat::S16}));
    ASSERT_EQ(p.view->write(second.pcm.data(), 24000).frames, 24000U);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    for (const GroupEvent& event : events) {
        EXPECT_FALSE(std::holds_alternative<PipelineFailed>(event));
    }
    const sched::Anchor& anchor = std::get<EpochStarted>(events.front()).anchor;
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    // The last input sample plays 20.8 us before 1.5 s
    EXPECT_EQ(drained_last_cell(events), 74);

    const Bytes out44 = expect_on_grid(frames[at44], anchor, 1, 882);
    ASSERT_EQ(out44.size() / 4, 66150U);
    EXPECT_TRUE(std::equal(first.pcm.begin(), first.pcm.end(), out44.begin()));
    expect_resampled(out44, 44100 * 2,
                     resample_offline(duplicated(second.floats, 2), 2, 48000, 44100, 960));
    EXPECT_EQ(finals.at(at44), 74);

    const Bytes out48 = expect_on_grid(frames[at48], anchor, 1, 960);
    ASSERT_EQ(out48.size() / 4, 72000U);
    expect_resampled(out48, 0, resample_offline(first.floats, 2, 44100, 48000, 882));
    for (size_t i = 0; i < 24000; ++i) {
        const int32_t mono = sample_at(second.pcm, i, 2);
        ASSERT_EQ(sample_at(out48, (48000 + i) * 2, 2), mono) << i;
        ASSERT_EQ(sample_at(out48, ((48000 + i) * 2) + 1, 2), mono) << i;
    }
    EXPECT_EQ(finals.at(at48), 74);

    const Bytes out11 = expect_on_grid(frames[at11], anchor, 2, 441);
    ASSERT_EQ(out11.size() / 2, 16538U);
    expect_resampled(out11, 0, resample_offline(averaged(first.floats), 1, 44100, 11025, 882));
    expect_resampled(out11, 11025, resample_offline(second.floats, 1, 48000, 11025, 960));
    EXPECT_EQ(finals.at(at11), 74);
}

/// Every event the pipelines post until they post no more
std::vector<GroupEvent> run_all(Pipelines& p) {
    std::vector<GroupEvent> events = p.run();
    for (std::vector<GroupEvent> more = p.run(); !more.empty(); more = p.run()) {
        events.insert(events.end(), more.begin(), more.end());
    }
    return events;
}

/// The frames of one instance from index on, by index
std::map<int64_t, Bytes> payloads_from(const std::vector<sched::Frame>& frames, int64_t index) {
    std::map<int64_t, Bytes> out;
    for (const sched::Frame& frame : frames) {
        if (frame.index >= index) {
            out[frame.index] = frame.payload;
        }
    }
    return out;
}

// A second of 44.1 kHz stereo, then 48 kHz stereo; instances that start mid-epoch in the second
// segment give the frames of instances running since cell 0. At 48 kHz, where the segment needs no
// stage, bit for bit. At 44.1 kHz, one starting on the boundary cell has no history in its segment:
// its warm-up is clipped at the segment's start and its resampler primed, as the running one's is
// when it crosses the boundary, so it too is bit for bit; one starting two cells later warms up
// from the segment's history and is within one quantization step of the running one.
TEST(GroupAudioPipelines, StartsAnInstanceMidEpochInALaterSegment) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
    const sched::AudioFormat pcm48{sched::Codec::PCM, 48000, 2, 16};
    const sched::AudioFormat pcm44{sched::Codec::PCM, 44100, 2, 16};
    const sched::InstanceId run48{160};
    const sched::InstanceId mid48{161};
    const sched::InstanceId run44{162};
    const sched::InstanceId boundary44{163};
    const sched::InstanceId mid44{164};
    const Tone first(44100, 2, 44100);
    const Tone second(48000, 2, 48000, 48000);
    ASSERT_EQ(p.view->write(first.pcm.data(), 44100).frames, 44100U);
    // Requests from cell 0, and from later cells of the epoch the write anchored
    const sched::EpochId epoch = job_view_of(*p.view).run.epoch;
    p.slot->publish({{run48, {}, pcm48, 0},
                     {mid48, epoch, pcm48, 60},
                     {run44, {}, pcm44, 0},
                     {boundary44, epoch, pcm44, 50},
                     {mid44, epoch, pcm44, 52}});
    ASSERT_TRUE(p.view->set_format({48000, 2, SampleFormat::S16}));
    ASSERT_EQ(p.view->write(second.pcm.data(), 48000).frames, 48000U);
    p.view->drain();
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(run_all(p));

    const auto expect_same = [&](sched::InstanceId running, sched::InstanceId started, int64_t from,
                                 int32_t tolerance) {
        ASSERT_FALSE(frames[started].empty());
        EXPECT_EQ(frames[started].front().index, from);
        const std::map<int64_t, Bytes> reference = payloads_from(frames[running], from);
        const std::map<int64_t, Bytes> compared = payloads_from(frames[started], from);
        ASSERT_EQ(compared.size(), reference.size());
        for (const auto& [index, payload] : compared) {
            SCOPED_TRACE(index);
            const Bytes& expected = reference.at(index);
            ASSERT_EQ(payload.size(), expected.size());
            for (size_t i = 0; i < payload.size() / 2; ++i) {
                ASSERT_LE(std::abs(sample_at(payload, i, 2) - sample_at(expected, i, 2)), tolerance)
                    << i;
            }
        }
    };
    expect_same(run48, mid48, 60, 0);
    expect_same(run44, boundary44, 50, 0);
    expect_same(run44, mid44, 52, 1);
}

// A change of channel count alone keeps the resampler running: 44.1 kHz stereo then 44.1 kHz mono
// into a 48 kHz stereo instance is micro-resampler's output over the whole mixed input, one run,
// sample for sample
TEST(GroupAudioPipelines, KeepsTheResamplerThroughAChannelOnlyChange) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
    const sched::InstanceId resampled{170};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Tone first(44100, 2, 44100);
    const Tone second(44100, 1, 22050, 44100);
    ASSERT_EQ(p.view->write(first.pcm.data(), 44100).frames, 44100U);
    ASSERT_TRUE(p.view->set_format({44100, 1, SampleFormat::S16}));
    ASSERT_EQ(p.view->write(second.pcm.data(), 22050).frames, 22050U);
    p.view->drain();
    const Bytes out = concatenated(frames_by_instance(run_all(p))[resampled]);
    std::vector<float> mixed = first.floats;
    const std::vector<float> both = duplicated(second.floats, 2);
    mixed.insert(mixed.end(), both.begin(), both.end());
    const std::vector<float> reference = resample_offline(mixed, 2, 44100, 48000, 882);
    ASSERT_EQ(out.size() / 2, reference.size());
    expect_resampled(out, 0, reference);
}

// 44,643 samples of 44.1 kHz stereo end on no cell, then half a second of 48 kHz stereo. At 48 kHz
// the first segment is resampled and cut to round(44,643 * 48,000 / 44,100) = 48,591 samples and
// the second follows at that index, as written; at 44.1 kHz the first is as written and the second,
// resampled from the boundary, follows at index 44,643
TEST(GroupAudioPipelines, CrossesABoundaryOnNoCell) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
    const sched::InstanceId at44{180};
    const sched::InstanceId at48{181};
    p.slot->publish({{at44, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {at48, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Tone first(44100, 2, 44643);
    const Tone second(48000, 2, 24000, 48591);
    ASSERT_EQ(p.view->write(first.pcm.data(), 44643).frames, 44643U);
    ASSERT_TRUE(p.view->set_format({48000, 2, SampleFormat::S16}));
    ASSERT_EQ(p.view->write(second.pcm.data(), 24000).frames, 24000U);
    p.view->drain();
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(run_all(p));

    const Bytes out48 = concatenated(frames[at48]);
    const std::vector<float> first48 = resample_offline(first.floats, 2, 44100, 48000, 882);
    ASSERT_EQ(first48.size(), size_t{48591} * 2);
    ASSERT_EQ(out48.size() / 4, 48591U + 24000U);
    expect_resampled(out48, 0, first48);
    EXPECT_TRUE(std::equal(second.pcm.begin(), second.pcm.end(), out48.begin() + (48591 * 4)));

    const Bytes out44 = concatenated(frames[at44]);
    EXPECT_TRUE(std::equal(first.pcm.begin(), first.pcm.end(), out44.begin()));
    const std::vector<float> second44 = resample_offline(second.floats, 2, 48000, 44100, 960);
    ASSERT_EQ(out44.size() / 2, (size_t{44643} * 2) + second44.size());
    expect_resampled(out44, size_t{44643} * 2, second44);
}

// A resume whose paused position lies in a later segment starts the new epoch in that segment's
// format: one inside the 48 kHz segment, and one exactly on its first position. The 48 kHz
// instance's frames are the input from the paused position, bit for bit; on the boundary the 44.1
// kHz instance has no history in the segment, so it is micro-resampler's output over the segment
// from its start, primed, sample for sample
TEST(GroupAudioPipelines, ResumesInALaterSegment) {
    struct Row {
        const char* name;
        uint64_t paused;
    };
    const std::array<Row, 2> rows{{{"inside the segment", 44100 + 9600}, {"on its start", 44100}}};
    const Tone first(44100, 2, 44100);
    const Tone second(48000, 2, 48000, 48000);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Pipelines p(InputFormat{44100, 2, SampleFormat::S16}, 10000000);
        const sched::InstanceId at48{190};
        const sched::InstanceId at44{191};
        p.slot->publish({{at48, {}, {sched::Codec::PCM, 48000, 2, 16}, 0},
                         {at44, {}, {sched::Codec::PCM, 44100, 2, 16}, 0}});
        ASSERT_EQ(p.view->write(first.pcm.data(), 44100).frames, 44100U);
        ASSERT_TRUE(p.view->set_format({48000, 2, SampleFormat::S16}));
        ASSERT_EQ(p.view->write(second.pcm.data(), 48000).frames, 48000U);
        run_all(p);
        p.view->pause();
        // White-box: the paused position, which pause() takes from the time
        p.view->p_pause_ = row.paused;
        run_all(p);
        p.view->resume();
        p.view->drain();
        const std::vector<GroupEvent> events = run_all(p);
        const auto started = std::find_if(events.begin(), events.end(), [](const GroupEvent& e) {
            return std::holds_alternative<EpochStarted>(e);
        });
        ASSERT_NE(started, events.end());
        const sched::Anchor& anchor = std::get<EpochStarted>(*started).anchor;
        EXPECT_EQ(anchor.segments.front().position, row.paused);
        EXPECT_EQ(anchor.segments.front().rate, 48000U);
        std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);

        const Bytes out48 = concatenated(frames[at48]);
        const size_t skipped = (row.paused - 44100) * 4;
        ASSERT_EQ(out48.size(), second.pcm.size() - skipped);
        EXPECT_TRUE(std::equal(out48.begin(), out48.end(), second.pcm.begin() + skipped));
        if (row.paused == 44100) {
            const Bytes out44 = concatenated(frames[at44]);
            const std::vector<float> reference =
                resample_offline(second.floats, 2, 48000, 44100, 960);
            ASSERT_EQ(out44.size() / 2, reference.size());
            expect_resampled(out44, 0, reference);
        }
    }
}

/// One seeded run of GroupAudioProperty: input segments of random rates, channel counts and
/// lengths, written in random blocks with a set_format() between them, then drained, through
/// three instances of random formats, the last requested from a random cell of the epoch, on a
/// timeline with random shifts
void run_segments_seed(uint64_t seed) {
    std::mt19937_64 rng(seed);
    // A fixed mapping of mt19937_64's fixed output, as in test_sched_property.cpp
    const auto uniform = [&rng](int64_t low, int64_t high) {
        return low + static_cast<int64_t>(rng() % static_cast<uint64_t>(high - low + 1));
    };
    constexpr std::array<uint32_t, 6> RATES = {11025, 22050, 32000, 44100, 48000, 8010};
    const auto random_input = [&] {
        return InputFormat{RATES[static_cast<size_t>(uniform(0, 4))],
                           static_cast<uint8_t>(uniform(1, 2)), SampleFormat::S16};
    };
    const std::vector<sched::AudioFormat> outputs = {
        {sched::Codec::PCM, 48000, 2, 16},  {sched::Codec::PCM, 11025, 1, 16},
        {sched::Codec::FLAC, 44100, 2, 16}, {sched::Codec::PCM, 8010, 1, 16},
        {sched::Codec::PCM, 22050, 2, 16},  {sched::Codec::FLAC, 32000, 1, 16}};
    std::vector<PipelineRequest> set;
    std::vector<size_t> chosen;
    while (chosen.size() < 3) {
        const auto pick = static_cast<size_t>(uniform(0, outputs.size() - 1));
        if (std::find(chosen.begin(), chosen.end(), pick) == chosen.end()) {
            set.push_back({sched::InstanceId{200 + chosen.size()}, {}, outputs[pick], 0});
            chosen.push_back(pick);
        }
    }

    std::vector<InputFormat> formats = {random_input()};
    Pipelines p(formats.front(), 10000000);
    std::vector<Bytes> written;
    uint64_t head = 0;
    // No job runs before the drain, so every segment stays staged: the ring holds STAGING_RING_US
    // of the largest format so far (it grows within a run), and a segment is cut to the room left,
    // which only the embedded profile's ring comes to
    uint64_t ring_bytes = 0;
    uint64_t staged_bytes = 0;
    const int64_t segments = uniform(1, 8);
    for (int64_t j = 0; j < segments; ++j) {
        if (j > 0) {
            formats.push_back(random_input());
            ASSERT_TRUE(p.view->set_format(formats.back()));
        }
        const size_t bytes = size_t{formats.back().channels} * 2;
        ring_bytes = std::max<uint64_t>(
            ring_bytes,
            (formats.back().sample_rate * sendspin::server::STAGING_RING_US / 1000000) * bytes);
        const uint64_t room = (ring_bytes - staged_bytes) / bytes;
        ASSERT_GT(room, 0U);
        const uint64_t length = std::min(static_cast<uint64_t>(uniform(1, 30000)), room);
        staged_bytes += length * bytes;
        const Tone tone(formats.back().sample_rate, formats.back().channels, length, head);
        written.push_back(tone.pcm);
        for (uint64_t at = 0; at < length;) {
            const uint64_t block = std::min<uint64_t>(length - at, uniform(1, 5000));
            ASSERT_EQ(p.view->write(tone.pcm.data() + (at * bytes), block).frames, block);
            at += block;
        }
        head += length;
    }
    // Shifts at increasing random cells, and the last instance requested from a random cell, of
    // the epoch the first write anchored
    const sendspin::server::JobView view = job_view_of(*p.view);
    const int64_t last_cell =
        sched::cell_at(view.anchor, sched::unshifted_time(view.anchor, head - 1));
    int64_t shift_at = 0;
    for (int64_t i = uniform(0, 2); i > 0; --i) {
        shift_at += uniform(1, std::max<int64_t>(1, last_cell / 2));
        ASSERT_TRUE(p.view->add_shift(view.run.epoch, shift_at, uniform(1, 100) * 1000));
    }
    const int64_t requested = uniform(0, last_cell / 2);
    set.back().epoch = view.run.epoch;
    set.back().from_index = requested;
    p.slot->publish(set);
    p.view->drain();
    std::vector<GroupEvent> events = run_all(p);
    ASSERT_FALSE(events.empty());
    sched::Anchor anchor = std::get<EpochStarted>(events.front()).anchor;
    ASSERT_EQ(anchor.segments.size(), static_cast<size_t>(segments));
    for (const GroupEvent& event : events) {
        ASSERT_FALSE(std::holds_alternative<PipelineFailed>(event));
        if (const auto* shift = std::get_if<sendspin::server::AnchorShift>(&event)) {
            sched::add_shift(anchor, shift->index, shift->delta_us);
        }
    }
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    // The drain position's time, as the timeline gives it
    const double end_us = static_cast<double>(sched::unshifted_time(anchor, head) - anchor.t0_us);
    for (const PipelineRequest& request : set) {
        SCOPED_TRACE(request.format.sample_rate);
        const sched::FrameLayout layout =
            sched::frame_layout(request.format.sample_rate, request.format.codec);
        const std::vector<sched::Frame>& list = frames[request.instance];
        ASSERT_FALSE(list.empty());
        // From cell 0, or the request's frame boundary past every shift a frame would straddle;
        // later by a frame or two where no zero-phase step of the warm-up fits before it
        const int64_t start = sched::instance_start(anchor, request.from_index, layout.cells);
        ASSERT_GE(list.front().index, start);
        ASSERT_LE(list.front().index, start + (4 * int64_t{layout.cells}));
        const int64_t first_output = list.front().index / layout.cells * layout.samples;
        Bytes out;
        uint64_t total = 0;
        sendspin::server::test::FlacReader reader;
        if (request.format.codec == sched::Codec::FLAC) {
            ASSERT_TRUE(reader.open(*sendspin::server::codec_header(request.format)));
        }
        for (size_t i = 0; i < list.size(); ++i) {
            ASSERT_EQ(list[i].index, list.front().index + static_cast<int64_t>(i * layout.cells));
            ASSERT_EQ(list[i].ts_us, sched::frame_timestamp(anchor, list[i].index));
            if (i + 1 < list.size()) {
                ASSERT_EQ(list[i].samples, layout.samples);
            }
            total += list[i].samples;
            if (request.format.codec == sched::Codec::FLAC) {
                const auto decoded = reader.decode(list[i].payload);
                ASSERT_TRUE(decoded && decoded->size() == 1U) << i;
                out.insert(out.end(), decoded->front().pcm.begin(), decoded->front().pcm.end());
            } else {
                out.insert(out.end(), list[i].payload.begin(), list[i].payload.end());
            }
        }
        EXPECT_EQ(finals.at(request.instance), list.back().index);
        // Each rate change cuts the output to its exact boundary time, so the whole is within a
        // sample and the timeline's own rounding of the end (under 1 us a segment) of its time
        const double exact = (end_us * request.format.sample_rate / 1e6) - first_output;
        EXPECT_LE(std::abs(static_cast<double>(total) - exact),
                  1.0 + (static_cast<double>(segments) * request.format.sample_rate / 1e6))
            << total << " " << exact;
        // A segment at the instance's rate and channel count is the input, bit for bit, from the
        // boundary's output index on, or from the instance's first sample when it starts inside it
        const size_t sample_bytes = size_t{request.format.channels} * 2;
        for (size_t j = 0; j < formats.size(); ++j) {
            if (formats[j].sample_rate != request.format.sample_rate ||
                formats[j].channels != request.format.channels) {
                continue;
            }
            SCOPED_TRACE(j);
            const int64_t at =
                sched::segment_output_index(anchor, j, request.format.sample_rate) - first_output;
            const auto skipped = static_cast<size_t>(std::max<int64_t>(0, -at)) * sample_bytes;
            if (skipped >= written[j].size()) {
                continue;
            }
            const size_t from = static_cast<size_t>(std::max<int64_t>(0, at)) * sample_bytes;
            ASSERT_GE(out.size(), from + written[j].size() - skipped);
            EXPECT_TRUE(std::equal(written[j].begin() + static_cast<std::ptrdiff_t>(skipped),
                                   written[j].end(),
                                   out.begin() + static_cast<std::ptrdiff_t>(from)));
        }
    }
}

// Seeded: whatever segments the input goes through, every instance keeps its format, frames of its
// own length stepping by its cells from 0 on the epoch's cell timestamps, full but the last, its
// final index its last frame's, and as many samples as the input's time at its rate; a first
// segment it does not convert is the input as written
TEST(GroupAudioProperty, KeepsEveryInstanceOnTheGridAcrossSegments) {
    for (uint64_t seed = 1; seed <= 24; ++seed) {
        SCOPED_TRACE(seed);
        run_segments_seed(seed);
        if (testing::Test::HasFatalFailure()) {
            return;
        }
    }
}

// A stretch's output is cut or extended with its last sample to the boundary's output index; a
// warm-up's samples, below the next cell's first, are counted but never kept; an extension past two
// samples, which no stage gives, is refused and changes nothing. The stages give no short stretch
// through the property test above, so the extension is reached directly
TEST(GroupAudioPipelines, CutsOrExtendsAStretchToItsBoundaryCount) {
    struct Row {
        const char* name;
        int64_t produced;
        size_t pending;
        int64_t total;
        bool accepted;
        size_t kept;
        size_t extended;
    };
    // Mono 16-bit samples of 1 and a last sample of 7; cell 1's first output sample is 882
    const std::array<Row, 5> rows{{
        {"cut", 900, 18, 895, true, 13, 0},
        {"extended", 900, 18, 902, true, 18, 2},
        {"extended past a warm-up", 881, 0, 883, true, 0, 1},
        {"refused", 900, 18, 903, false, 18, 0},
        // Control: at the boundary already
        {"exact", 900, 18, 900, true, 18, 0},
    }};
    Pipelines p(InputFormat{44100, 1, SampleFormat::S16});
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        // White-box: an instance with only what the cut reads
        sendspin::server::GroupAudio::Instance instance;
        instance.cells = 1;
        instance.frame_samples = 882;
        instance.sample_bytes = 2;
        instance.next = 1;
        instance.produced = row.produced;
        instance.pending.assign(row.pending * 2, 0);
        for (size_t i = 0; i < row.pending; ++i) {
            instance.pending[2 * i] = 1;
        }
        instance.last_sample = {7, 0};
        EXPECT_EQ(p.audio.cut_to(instance, row.total), row.accepted);
        EXPECT_EQ(instance.produced, row.accepted ? row.total : row.produced);
        ASSERT_EQ(instance.pending.size(), (row.kept + row.extended) * 2);
        for (size_t i = 0; i < row.kept + row.extended; ++i) {
            EXPECT_EQ(instance.pending[2 * i], i < row.kept ? 1 : 7) << i;
        }
    }
}

// A mono input reaches a stereo player in both channels
TEST_F(GroupAudioTest, DuplicatesAMonoInputIntoEveryChannel) {
    constexpr sched::InstanceId STEREO{30};
    this->slot->publish({{STEREO, {}, {sched::Codec::PCM, RATE, 2, 16}, 0}});
    this->write(2 * CELL, 100);
    const std::vector<sched::Frame> frames = frames_by_instance(this->run())[STEREO];
    ASSERT_EQ(frames.size(), 2U);
    for (const sched::Frame& frame : frames) {
        ASSERT_EQ(frame.payload.size(), CELL * 4);
        for (uint64_t i = 0; i < CELL; ++i) {
            const int32_t expected = static_cast<int32_t>(100 + (frame.index * CELL) + i);
            ASSERT_EQ(sample_at(frame.payload, i * 2, 2), expected);
            ASSERT_EQ(sample_at(frame.payload, (i * 2) + 1, 2), expected);
        }
    }
}

// A downsampling instance can round a short last cell to no samples, so its final index is one cell
// below the input's last cell and another instance's
TEST(GroupAudioPipelines, EndsADownsampledEpochOnItsOwnLastFrame) {
    Pipelines p(InputFormat{48000, 1, SampleFormat::S16});
    const sched::InstanceId native{40};
    const sched::InstanceId low{41};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 48000, 1, 16}, 0},
                     {low, {}, {sched::Codec::PCM, 8000, 1, 16}, 0}});
    // Two samples into cell 3: a third of an output sample at 8 kHz, which rounds to none
    const Bytes pcm(((3 * 960) + 2) * 2, 1);
    ASSERT_EQ(p.view->write(pcm.data(), (3 * 960) + 2).frames, (3 * 960) + 2);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(drained_last_cell(events), 3);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    ASSERT_EQ(frames[low].size(), 3U);
    EXPECT_EQ(frames[low].back().samples, 160U);
    EXPECT_EQ(finals[low], 2);
    // Control: the input's rate keeps its two-sample last frame
    ASSERT_EQ(frames[native].size(), 4U);
    EXPECT_EQ(frames[native].back().samples, 2U);
    EXPECT_EQ(finals[native], 3);
}

/// Where a mono 24-bit instance's frames put their one non-zero sample: its time in microseconds
std::optional<double> impulse_time_us(const std::vector<sched::Frame>& frames, uint32_t rate) {
    std::optional<double> at;
    int32_t peak = 0;
    for (const sched::Frame& frame : frames) {
        for (uint32_t i = 0; i < frame.samples; ++i) {
            const int32_t value = std::abs(sample_at(frame.payload, i, 3));
            if (value > peak) {
                peak = value;
                at = static_cast<double>(frame.ts_us) + (i * 1e6 / rate);
            }
        }
    }
    return at;
}

struct RatePair {
    uint32_t in;
    uint32_t out;
};

class GroupAudioImpulse : public testing::TestWithParam<RatePair> {};

// The resampler's group delay is compensated inside it, so an impulse lands at the same timeline
// time in a native and a resampled instance of one group, within one output sample, upsampling and
// downsampling
TEST_P(GroupAudioImpulse, LandsAtTheSameTimeInANativeAndAResampledInstance) {
    const RatePair rates = GetParam();
    Pipelines p(InputFormat{rates.in, 1, SampleFormat::F32});
    const sched::InstanceId native{50};
    const sched::InstanceId resampled{51};
    p.slot->publish({{native, {}, {sched::Codec::PCM, rates.in, 1, 24}, 0},
                     {resampled, {}, {sched::Codec::PCM, rates.out, 1, 24}, 0}});
    const uint64_t samples = rates.in / 2;
    // Off the grid: 10,001 input samples is a fractional output position either way
    constexpr uint64_t IMPULSE = 10001;
    std::vector<float> pcm(samples, 0.0F);
    pcm[IMPULSE] = 0.5F;
    ASSERT_EQ(p.view->write(pcm.data(), samples).frames, samples);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    const sched::Anchor& anchor = std::get<EpochStarted>(events.front()).anchor;
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::optional<double> at_native = impulse_time_us(frames[native], rates.in);
    const std::optional<double> at_resampled = impulse_time_us(frames[resampled], rates.out);
    ASSERT_TRUE(at_native && at_resampled);
    EXPECT_DOUBLE_EQ(*at_native, static_cast<double>(anchor.t0_us) + (IMPULSE * 1e6 / rates.in));
    EXPECT_NEAR(*at_resampled, *at_native, 1e6 / rates.out);
}

INSTANTIATE_TEST_SUITE_P(Rates, GroupAudioImpulse,
                         testing::Values(RatePair{44100, 48000}, RatePair{48000, 44100}),
                         [](const testing::TestParamInfo<RatePair>& info) {
                             return std::to_string(info.param.in) + "To" +
                                    std::to_string(info.param.out);
                         });

// The ring keeps the largest warm-up behind the position playing, so a resumed resampled instance
// starts from a cell of real history instead of a primed block: its frames are those of an instance
// that ran from far earlier, sample for sample
TEST(GroupAudioPipelines, ResumesAResampledInstanceFromRealHistory) {
    constexpr uint32_t IN_RATE = 44100;
    Pipelines p(InputFormat{IN_RATE, 1, SampleFormat::F32});
    const sched::InstanceId resampled{60};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 1, 24}, 0}});
    constexpr uint64_t SAMPLES = IN_RATE;
    std::vector<float> pcm(SAMPLES);
    for (uint64_t i = 0; i < SAMPLES; ++i) {
        pcm[i] = static_cast<float>((0.4 * std::sin(i * 0.031)) + (0.2 * std::sin(i * 1.7)));
    }
    ASSERT_EQ(p.view->write(pcm.data(), SAMPLES).frames, SAMPLES);
    p.run();
    // Past the epoch's start, a job lets the ring drop what plays before now
    std::this_thread::sleep_for(std::chrono::microseconds(LEAD_US + 150000));
    p.run();
    const uint64_t paused = p.view->pause();
    ASSERT_GE(paused, uint64_t{2 * 882});
    p.run();
    p.view->resume();
    p.view->drain();
    std::vector<sched::Frame> frames = frames_by_instance(p.run())[resampled];
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.front().index, 0);

    // An instance that ran from 20 cells before the resume position, unprimed, past its warm-up
    const uint64_t from = paused >= 20 * 882 ? paused - (20 * 882) : paused % 147;
    const std::vector<float> history(pcm.begin() + static_cast<std::ptrdiff_t>(from), pcm.end());
    auto running = std::make_unique<ReferenceResampler>(IN_RATE, 48000, 1, history.size());
    ASSERT_TRUE(running->valid());
    std::vector<float> out((history.size() * 48000 / IN_RATE) + 2);
    size_t consumed = 0;
    size_t generated = 0;
    ASSERT_TRUE(running->process_block(history.data(), history.size(), out.data(), out.size(),
                                       consumed, generated));
    // The resume position is a whole output sample past from: (paused - from) is a multiple of 147
    const size_t skip = (paused - from) / 147 * 160;
    const Bytes resumed = concatenated(frames);
    const size_t compared = std::min<size_t>(resumed.size() / 3, generated - skip - 1000);
    ASSERT_GT(compared, 10000U);
    for (size_t i = 0; i < compared; ++i) {
        const double scaled = std::clamp(static_cast<double>(out[skip + i]), -2.0, 2.0) * 8388608.0;
        ASSERT_EQ(sample_at(resumed, i, 3),
                  std::clamp<int64_t>(std::llround(scaled), -8388608, 8388607))
            << i;
    }
}

// A resampled instance core adds while the run plays catches up from the staging ring's history,
// frame for frame the frames of an instance running since cell 0, and in a draining epoch posts its
// own final index
TEST(GroupAudioPipelines, CatchesUpAResampledInstanceAddedMidEpoch) {
    constexpr uint32_t IN_RATE = 44100;
    Pipelines p(InputFormat{IN_RATE, 2, SampleFormat::S16});
    const sched::AudioFormat pcm48{sched::Codec::PCM, 48000, 2, 16};
    const sched::InstanceId running{70};
    const sched::InstanceId caught_up{72};
    p.slot->publish({{running, {}, pcm48, 0}});
    Bytes pcm(20 * 882 * 4);
    for (size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<uint8_t>((i * 7919) >> 3);
    }
    ASSERT_EQ(p.view->write(pcm.data(), 10 * 882).frames, 10 * 882U);
    std::vector<GroupEvent> events = p.run();
    const sched::EpochId epoch = std::get<EpochStarted>(events.front()).epoch;
    std::vector<sched::Frame> first = frames_by_instance(events)[running];

    p.slot->publish({{running, {}, pcm48, 0}, {caught_up, epoch, pcm48, 4}});
    ASSERT_EQ(p.view->write(pcm.data() + (10 * 882 * 4), 10 * 882).frames, 10 * 882U);
    p.view->drain();
    events = p.run();
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    first.insert(first.end(), frames[running].begin(), frames[running].end());
    ASSERT_FALSE(frames[caught_up].empty());
    EXPECT_EQ(frames[caught_up].front().index, 4);
    for (const sched::Frame& frame : frames[caught_up]) {
        const auto same = std::find_if(first.begin(), first.end(), [&](const sched::Frame& f) {
            return f.index == frame.index;
        });
        ASSERT_NE(same, first.end());
        EXPECT_EQ(frame.payload, same->payload) << frame.index;
    }
    std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(drained_last_cell(events), 19);
    EXPECT_EQ(finals[running], 19);
    EXPECT_EQ(finals[caught_up], 19);
}

// A resampled frame needs the reach of input past its cell, so a stalled input is rebased when that
// lookahead arrives and the frame can be produced, once, by what restores the full group lead then;
// not when only the cell's own input is staged, which would shift by too little and need a second
// shift
TEST(GroupAudioPipelines, RebasesAStalledResampledInputWhenItsLookaheadArrives) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId resampled{90};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(3 * 882 * 4, 5);
    // Cell 1 is staged, but not the 53 samples past it
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 882).frames, 2 * 882U);
    std::vector<GroupEvent> events = p.run();
    ASSERT_EQ(frames_by_instance(events)[resampled].size(), 1U);
    constexpr int64_t STALL_US = 2000000;
    EXPECT_TRUE(p.run(STALL_US).empty());

    // The lookahead arrives later still
    ASSERT_EQ(p.view->write(pcm.data(), 882).frames, 882U);
    const int64_t before = now_us() + (2 * STALL_US);
    events = p.run(2 * STALL_US);
    ASSERT_EQ(kinds_of(events), "AF");
    const sched::Frame& frame = std::get<FrameReady>(events[1]).frame;
    EXPECT_EQ(frame.index, 1);
    EXPECT_GE(frame.ts_us, before + LEAD_US);
}

// A drained epoch's flushed resampler starts the next epoch over. A pause during a drain resumes
// into an epoch draining from its first job, and a completed drain's next run can be a clip drained
// before its first job: in both, the resampled instance produces the epoch's frames and its own
// final index, as the native one does.
TEST(GroupAudioPipelines, ResamplesEveryEpochAfterADrainedOne) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId native{100};
    const sched::InstanceId resampled{101};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(3 * 882 * 4, 9);
    const auto expect_epoch = [&](const std::vector<GroupEvent>& events, size_t cells) {
        std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
        std::map<sched::InstanceId, int64_t> finals = final_indices(events);
        EXPECT_EQ(frames[native].size(), cells);
        EXPECT_EQ(frames[resampled].size(), cells);
        EXPECT_EQ(finals[native], static_cast<int64_t>(cells) - 1);
        EXPECT_EQ(finals[resampled], static_cast<int64_t>(cells) - 1);
    };
    ASSERT_EQ(p.view->write(pcm.data(), 3 * 882).frames, 3 * 882U);
    p.view->drain();
    std::vector<GroupEvent> events = p.run();
    const sched::EpochId first = std::get<EpochStarted>(events.front()).epoch;
    expect_epoch(events, 3);

    // Paused before the epoch played: the resume replays all of it, draining from its start
    EXPECT_EQ(p.view->pause(), 0U);
    EXPECT_EQ(kinds_of(p.run()), "E");
    p.view->resume();
    events = p.run();
    ASSERT_EQ(kinds_of(events).front(), 'S');
    const sched::EpochId resumed = std::get<EpochStarted>(events.front()).epoch;
    EXPECT_NE(resumed, first);
    expect_epoch(events, 3);

    // The drain completes; the next run's clip is drained before its first job
    ASSERT_TRUE(p.view->complete_drain(resumed));
    p.view->release_drained();
    p.run();
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 882).frames, 2 * 882U);
    p.view->drain();
    expect_epoch(p.run(), 2);
}

// In a group with a native and a resampled instance, the native one waits at k* with the resampled
// one until the resampler's lookahead is staged and the rebase decided, so a stall shifts both from
// k* and no frame of either is produced late
TEST(GroupAudioPipelines, HoldsEveryInstanceAtTheShiftIndexUntilTheRebaseIsDecided) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId native{110};
    const sched::InstanceId resampled{111};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(3 * 882 * 4, 5);
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 882).frames, 2 * 882U);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(p.run());
    EXPECT_EQ(frames[native].size(), 1U);
    EXPECT_EQ(frames[resampled].size(), 1U);
    constexpr int64_t STALL_US = 2000000;
    EXPECT_TRUE(p.run(STALL_US).empty());

    ASSERT_EQ(p.view->write(pcm.data(), 882).frames, 882U);
    const int64_t before = now_us() + (2 * STALL_US);
    const std::vector<GroupEvent> events = p.run(2 * STALL_US);
    ASSERT_EQ(kinds_of(events), "AFF");
    EXPECT_EQ(std::get<sendspin::server::AnchorShift>(events[0]).index, 1);
    for (size_t i = 1; i < 3; ++i) {
        const sched::Frame& frame = std::get<FrameReady>(events[i]).frame;
        EXPECT_EQ(frame.index, 1);
        EXPECT_GE(frame.ts_us, before + LEAD_US);
    }
}

// With input staged past cell 1 but short of the resampler's reach, every instance waits at k* = 1;
// a drain() with no further input ends the input there, so the wait lifts, both instances produce
// through the drain position, short last frames included, and DrainStarted and both final indices
// follow
TEST(GroupAudioPipelines, LiftsTheHoldAtADrainShortOfTheReach) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId native{130};
    const sched::InstanceId resampled{131};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(((2 * 882) + 20) * 4, 7);
    ASSERT_EQ(p.view->write(pcm.data(), (2 * 882) + 20).frames, (2 * 882) + 20U);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(p.run());
    EXPECT_EQ(frames[native].size(), 1U);
    EXPECT_EQ(frames[resampled].size(), 1U);

    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    frames = frames_by_instance(events);
    ASSERT_EQ(frames[native].size(), 2U);
    ASSERT_EQ(frames[resampled].size(), 2U);
    EXPECT_EQ(frames[native].back().samples, 20U);
    // round(1,784 * 48,000 / 44,100) = 1,942: two samples past cell 2's start
    EXPECT_EQ(frames[resampled].back().samples, 22U);
    EXPECT_EQ(drained_last_cell(events), 2);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(finals.at(native), 2);
    EXPECT_EQ(finals.at(resampled), 2);
}

// A job builds at most one resampler, whose filter bank is the costly allocation, and reports more
// work while another waits; an instance with none, listed after one that waits, is built at once
TEST(GroupAudioPipelines, BuildsOneResamplerPerJob) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    p.slot->publish({{sched::InstanceId{120}, {}, {sched::Codec::PCM, 48000, 2, 16}, 0},
                     {sched::InstanceId{122}, {}, {sched::Codec::PCM, 96000, 2, 16}, 0},
                     {sched::InstanceId{121}, {}, {sched::Codec::PCM, 44100, 2, 24}, 0}});
    const auto built = [&] {
        return std::count_if(p.audio.instances_.begin(), p.audio.instances_.end(),
                             [](const auto& instance) { return instance.configured; });
    };
    // White-box: one job at a time, before any input
    EXPECT_TRUE(p.audio.run_job(now_us()));
    EXPECT_EQ(built(), 2);
    EXPECT_FALSE(p.audio.run_job(now_us()));
    EXPECT_EQ(built(), 3);
}

// An encoder that refuses fails its instance once, which then produces nothing and no longer holds
// a drain back; the group's other instance carries on through the drain
TEST(GroupAudioPipelines, FailsAnInstanceWhoseEncoderRefuses) {
    Pipelines p(InputFormat{44100, 2, SampleFormat::S16});
    const sched::InstanceId native{80};
    const sched::InstanceId resampled{81};
    p.slot->publish({{native, {}, {sched::Codec::PCM, 44100, 2, 16}, 0},
                     {resampled, {}, {sched::Codec::PCM, 48000, 2, 16}, 0}});
    const Bytes pcm(6 * 882 * 4, 3);
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 882).frames, 2 * 882U);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(p.run());
    ASSERT_EQ(frames[resampled].size(), 1U);
    // Cell 1 waits for the resampler's lookahead in both instances
    ASSERT_EQ(frames[native].size(), 1U);

    // White-box: an encoder whose frame is shorter refuses a 48 kHz frame
    auto& instances = p.audio.instances_;
    const auto it = std::find_if(instances.begin(), instances.end(),
                                 [&](const auto& instance) { return instance.id == resampled; });
    ASSERT_NE(it, instances.end());
    it->encoder = sendspin::server::FrameEncoder::create({sched::Codec::FLAC, 8000, 2, 16});
    ASSERT_TRUE(it->encoder);
    ASSERT_EQ(p.view->write(pcm.data(), 4 * 882).frames, 4 * 882U);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    size_t failures = 0;
    for (const GroupEvent& event : events) {
        if (const auto* failed = std::get_if<PipelineFailed>(&event)) {
            EXPECT_EQ(failed->instance, resampled);
            ++failures;
        }
    }
    EXPECT_EQ(failures, 1U);
    frames = frames_by_instance(events);
    EXPECT_TRUE(frames[resampled].empty());
    EXPECT_EQ(frames[native].size(), 5U);
    EXPECT_EQ(drained_last_cell(events), 5);
    std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    EXPECT_EQ(finals.count(resampled), 0U);
    EXPECT_EQ(finals[native], 5);
    EXPECT_TRUE(p.run().empty());
}

// ==============================================================================
// Live inputs
// ==============================================================================

/// A live input's options: its write jitter `J_in`
sendspin::server::InputOptions live_input(const InputFormat& format, int64_t jitter_us) {
    sendspin::server::InputOptions options{format};
    options.kind = sendspin::server::InputKind::LIVE;
    options.write_jitter_us = jitter_us;
    return options;
}

constexpr int64_t JITTER_US = 20000;
/// `L_p` of a one-cell frame with no resampler: the frame and `J_in`
constexpr int64_t LIVE_LATENCY_US = 20000 + JITTER_US;
/// The lead a live rebase restores: `G_eff - L_p + J_in`
constexpr int64_t ON_TIME_LEAD_US = LEAD_US - LIVE_LATENCY_US + JITTER_US;

// A live frame whose lead fell under G_eff - L_p - M_pump (250 ms here) shifts the timeline at it,
// to the lead of a frame whose input came on time, G_eff - L_p + J_in, in the host view too. Its
// lead of about 150 ms is far above a buffered input's threshold, od_max + M_pump.
TEST(GroupAudioLive, ShiftsALateFrameToTheOnTimeLead) {
    Pipelines p(live_input({RATE, 1, SampleFormat::S16}, JITTER_US), 200000);
    p.slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}});
    const Bytes cell(CELL * 2, 1);
    ASSERT_EQ(p.view->write(cell.data(), CELL).frames, CELL);
    std::vector<GroupEvent> events = p.run();
    ASSERT_EQ(kinds_of(events), "SF");
    const int64_t t0 = std::get<EpochStarted>(events[0]).anchor.t0_us;

    // Control: the next frame on time
    ASSERT_EQ(p.view->write(cell.data(), CELL).frames, CELL);
    events = p.run();
    ASSERT_EQ(kinds_of(events), "F");
    EXPECT_EQ(std::get<FrameReady>(events[0]).frame.ts_us, t0 + 20000);

    // The third cell comes 190 ms late
    constexpr int64_t LATE_US = 190000;
    ASSERT_EQ(p.view->write(cell.data(), CELL).frames, CELL);
    const int64_t before = now_us() + LATE_US;
    events = p.run(LATE_US);
    const int64_t after = now_us() + LATE_US;
    ASSERT_EQ(kinds_of(events), "AF");
    const auto& shift = std::get<sendspin::server::AnchorShift>(events[0]);
    EXPECT_EQ(shift.index, 2);
    const sched::Frame& frame = std::get<FrameReady>(events[1]).frame;
    EXPECT_EQ(frame.ts_us, t0 + 40000 + shift.delta_us);
    EXPECT_GE(frame.ts_us, before + ON_TIME_LEAD_US);
    EXPECT_LE(frame.ts_us, after + ON_TIME_LEAD_US);
    EXPECT_EQ(p.view->time_of(2 * CELL), frame.ts_us);
}

// A live rebase takes the largest `L_p` over the frame instances. From 44.1 kHz, a 48 kHz
// instance's resampler reads 53 input frames, 1202 us, past each cell, which a native 44.1 kHz
// instance does not, so the late frame shifts to G - (20000 + 1202 + J) + J in both, and admission
// is published that `L_p`
TEST(GroupAudioLive, RebasesByTheResampledInstancesLookahead) {
    constexpr uint32_t IN_RATE = 44100;
    constexpr uint64_t IN_CELL = IN_RATE / 50;
    constexpr int64_t RESAMPLED_LATENCY_US = 20000 + 1202 + JITTER_US;
    ASSERT_EQ(
        sched::pipeline_latency_us({1,
                                    sendspin::server::Resampler::filter_length(
                                        IN_RATE, 48000, sendspin::server::ResamplerQuality::MEDIUM),
                                    IN_RATE},
                                   sendspin::server::InputKind::LIVE, JITTER_US),
        RESAMPLED_LATENCY_US);
    Pipelines p(live_input({IN_RATE, 1, SampleFormat::S16}, JITTER_US), 200000);
    const sched::InstanceId resampled{120};
    const sched::InstanceId native{121};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 1, 24}, 0},
                     {native, {}, {sched::Codec::PCM, IN_RATE, 1, 24}, 0}});
    const Bytes cells(5 * IN_CELL * 2, 1);
    ASSERT_EQ(p.view->write(cells.data(), 2 * IN_CELL).frames, 2 * IN_CELL);
    std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    const int64_t t0 = std::get<EpochStarted>(events[0]).anchor.t0_us;

    // Cells 2 to 4 come 190 ms late. The shift index is 1, which the resampled instance could not
    // make without the input past cell 1, and the native one holds at until the rebase is decided
    constexpr int64_t LATE_US = 190000;
    ASSERT_EQ(p.view->write(cells.data(), 3 * IN_CELL).frames, 3 * IN_CELL);
    const int64_t before = now_us() + LATE_US;
    events = p.run(LATE_US);
    const int64_t after = now_us() + LATE_US;
    std::optional<sendspin::server::AnchorShift> shift;
    for (const GroupEvent& event : events) {
        if (const auto* found = std::get_if<sendspin::server::AnchorShift>(&event)) {
            ASSERT_FALSE(shift) << "a second shift";
            shift = *found;
        }
    }
    ASSERT_TRUE(shift);
    ASSERT_EQ(shift->index, 1);
    const int64_t at = t0 + 20000 + shift->delta_us;
    EXPECT_GE(at, before + LEAD_US - RESAMPLED_LATENCY_US + JITTER_US);
    EXPECT_LE(at, after + LEAD_US - RESAMPLED_LATENCY_US + JITTER_US);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    for (const sched::InstanceId instance : {resampled, native}) {
        const auto frame = std::find_if(frames[instance].begin(), frames[instance].end(),
                                        [](const sched::Frame& f) { return f.index == 1; });
        ASSERT_NE(frame, frames[instance].end()) << instance.value;
        EXPECT_EQ(frame->ts_us, at) << instance.value;
    }
    EXPECT_EQ(p.view->rebase_latency_us_, RESAMPLED_LATENCY_US);
}

// A joiner raising the group lead raises a live epoch's G_eff, so the next frame shifts by the
// deficit, to the new on-time lead. Control: a buffered input keeps its timeline, its frames far
// enough ahead already.
TEST(GroupAudioLive, ShiftsByTheDeficitWhenTheGroupLeadRises) {
    constexpr int64_t RAISED_US = LEAD_US + 200000;
    const Bytes cell(CELL * 2, 1);
    for (const bool live : {true, false}) {
        SCOPED_TRACE(live);
        const InputFormat format{RATE, 1, SampleFormat::S16};
        Pipelines p(live ? live_input(format, JITTER_US) : sendspin::server::InputOptions{format},
                    200000);
        p.slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}});
        ASSERT_EQ(p.view->write(cell.data(), CELL).frames, CELL);
        std::vector<GroupEvent> events = p.run();
        ASSERT_EQ(kinds_of(events), "SF");
        const int64_t t0 = std::get<EpochStarted>(events[0]).anchor.t0_us;

        p.view->set_group_params({RAISED_US, 0, 200000});
        ASSERT_EQ(p.view->write(cell.data(), CELL).frames, CELL);
        const int64_t before = now_us();
        events = p.run();
        const int64_t after = now_us();
        if (!live) {
            ASSERT_EQ(kinds_of(events), "F");
            EXPECT_EQ(std::get<FrameReady>(events[0]).frame.ts_us, t0 + 20000);
            continue;
        }
        ASSERT_EQ(kinds_of(events), "AF");
        const sched::Frame& frame = std::get<FrameReady>(events[1]).frame;
        EXPECT_EQ(frame.index, 1);
        EXPECT_GE(frame.ts_us, before + RAISED_US - LIVE_LATENCY_US + JITTER_US);
        EXPECT_LE(frame.ts_us, after + RAISED_US - LIVE_LATENCY_US + JITTER_US);
    }
}

// A paused live input takes every write, the staging ring keeping only what a resume warms up from,
// so writes go on past the ring's 12 s; resume() anchors the new run at the head, and its first
// frame is the input written after it
TEST(GroupAudioLive, TakesEveryWriteWhilePausedAndResumesAtTheHead) {
    Pipelines p(live_input({RATE, 1, SampleFormat::S16}, JITTER_US), 200000);
    p.slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}});
    Bytes block(RATE * 2, 0);
    ASSERT_EQ(p.view->write(block.data(), CELL).frames, CELL);
    ASSERT_EQ(kinds_of(p.run()), "SF");
    p.view->pause();
    ASSERT_EQ(kinds_of(p.run()), "E");
    uint64_t head = CELL;
    for (int second = 0; second < 20; ++second) {
        SCOPED_TRACE(second);
        const sendspin::server::WriteResult result = p.view->write(block.data(), RATE);
        ASSERT_EQ(result.frames, RATE);
        EXPECT_EQ(result.state, sendspin::server::InputState::OPEN);
        head += RATE;
        EXPECT_TRUE(p.run().empty());
    }

    p.view->resume();
    // The samples written after the resume count up from 1000
    for (uint64_t i = 0; i < CELL; ++i) {
        const auto value = static_cast<uint16_t>(1000 + i);
        block[i * 2] = static_cast<uint8_t>(value);
        block[(i * 2) + 1] = static_cast<uint8_t>(value >> 8);
    }
    ASSERT_EQ(p.view->write(block.data(), CELL).frames, CELL);
    const std::vector<GroupEvent> events = p.run();
    ASSERT_EQ(kinds_of(events), "SF");
    EXPECT_EQ(std::get<EpochStarted>(events[0]).anchor.segments.front().position, head);
    const sched::Frame& frame = std::get<FrameReady>(events[1]).frame;
    EXPECT_EQ(frame.index, 0);
    EXPECT_EQ(sample_at(frame.payload, 0, 3), 1000 * 256);
}

// Through a live pause, the staging ring keeps the history before the head a resampled instance
// warms up from, so the resumed instance computes what one running through the input would, sample
// for sample
TEST(GroupAudioLive, ResumesAResampledInstanceFromTheHistoryBeforeTheHead) {
    constexpr uint32_t IN_RATE = 44100;
    Pipelines p(live_input({IN_RATE, 1, SampleFormat::F32}, JITTER_US), 1000000);
    const sched::InstanceId resampled{110};
    p.slot->publish({{resampled, {}, {sched::Codec::PCM, 48000, 1, 24}, 0}});
    constexpr uint64_t SAMPLES = 2 * IN_RATE;
    std::vector<float> pcm(SAMPLES);
    for (uint64_t i = 0; i < SAMPLES; ++i) {
        pcm[i] = static_cast<float>((0.4 * std::sin(i * 0.031)) + (0.2 * std::sin(i * 1.7)));
    }
    // A second playing, then half a second written while paused
    constexpr uint64_t HEAD = IN_RATE + (IN_RATE / 2);
    ASSERT_EQ(p.view->write(pcm.data(), IN_RATE).frames, IN_RATE);
    p.run();
    p.view->pause();
    ASSERT_EQ(p.view->write(pcm.data() + IN_RATE, IN_RATE / 2).frames, IN_RATE / 2);
    p.run();
    p.view->resume();
    ASSERT_EQ(p.view->write(pcm.data() + HEAD, SAMPLES - HEAD).frames, SAMPLES - HEAD);
    p.view->drain();
    std::vector<sched::Frame> frames = frames_by_instance(p.run())[resampled];
    ASSERT_FALSE(frames.empty());
    EXPECT_EQ(frames.front().index, 0);

    // An instance that ran from 20 cells before the head, unprimed, past its warm-up; the head is
    // a whole output sample past it, (HEAD - from) being a multiple of 147
    constexpr uint64_t FROM = HEAD - (20 * 882);
    const std::vector<float> history(pcm.begin() + static_cast<std::ptrdiff_t>(FROM), pcm.end());
    auto running = std::make_unique<ReferenceResampler>(IN_RATE, 48000, 1, history.size());
    ASSERT_TRUE(running->valid());
    std::vector<float> out((history.size() * 48000 / IN_RATE) + 2);
    size_t consumed = 0;
    size_t generated = 0;
    ASSERT_TRUE(running->process_block(history.data(), history.size(), out.data(), out.size(),
                                       consumed, generated));
    const size_t skip = (HEAD - FROM) / 147 * 160;
    const Bytes resumed = concatenated(frames);
    const size_t compared = std::min<size_t>(resumed.size() / 3, generated - skip - 1000);
    ASSERT_GT(compared, 10000U);
    for (size_t i = 0; i < compared; ++i) {
        const double scaled = std::clamp(static_cast<double>(out[skip + i]), -2.0, 2.0) * 8388608.0;
        ASSERT_EQ(sample_at(resumed, i, 3),
                  std::clamp<int64_t>(std::llround(scaled), -8388608, 8388607))
            << i;
    }
}

// ==============================================================================
// Opus
// ==============================================================================

// spec player/v1.md "Codec framing": an Opus instance beside a PCM one cuts the same cells at the
// same timestamps, one packet a frame. The encoder is fed its output its lookahead ahead
// (encoder_delay_samples()), so the packets, decoded in order, give the input at their own
// timestamps, not 2.5 ms late; a drained epoch's input ends in a padded last packet at the PCM
// instance's last index, so the tail is heard
TEST(GroupAudioOpus, EncodesPacketsAlignedWithTheInput) {
    constexpr uint32_t IN_RATE = 48000;
    constexpr uint64_t BLOCK = IN_RATE / 50;
    Pipelines p(InputFormat{IN_RATE, 2, SampleFormat::S16}, 2000000);
    const sched::InstanceId opus{200};
    const sched::InstanceId pcm{201};
    p.slot->publish({{opus, {}, {sched::Codec::OPUS, IN_RATE, 2, 16}, 0},
                     {pcm, {}, {sched::Codec::PCM, IN_RATE, 2, 16}, 0}});
    // A second and 100 samples: the last cell is short
    constexpr uint64_t SAMPLES = IN_RATE + 100;
    const Bytes input = sendspin::server::test::tone_frames(0, SAMPLES, IN_RATE);
    ASSERT_EQ(p.view->write(input.data(), SAMPLES).frames, SAMPLES);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(events);
    const std::map<sched::InstanceId, int64_t> finals = final_indices(events);
    ASSERT_EQ(frames[pcm].size(), 51U);
    ASSERT_EQ(frames[opus].size(), 51U);
    EXPECT_EQ(finals.at(opus), 50);
    EXPECT_EQ(finals.at(pcm), 50);
    sendspin::server::test::OpusReader reader(IN_RATE, 2);
    std::vector<int16_t> decoded;
    for (size_t k = 0; k < frames[opus].size(); ++k) {
        const sched::Frame& frame = frames[opus][k];
        EXPECT_EQ(frame.index, static_cast<int64_t>(k));
        EXPECT_EQ(frame.ts_us, frames[pcm][k].ts_us);
        // A packet decodes to a whole frame, the last one too
        EXPECT_EQ(frame.samples, BLOCK);
        const std::optional<std::vector<int16_t>> samples = reader.decode(frame.payload);
        ASSERT_TRUE(samples);
        ASSERT_EQ(samples->size(), BLOCK * 2);
        decoded.insert(decoded.end(), samples->begin(), samples->end());
    }
    EXPECT_EQ(frames[pcm].back().samples, 100U);
    std::vector<int16_t> reference(input.size() / 2);
    std::memcpy(reference.data(), input.data(), input.size());
    // From the fifth frame on, clear of the stream's start, through the last input sample
    const std::span<const int16_t> tail(reference.data() + (5 * BLOCK * 2),
                                        reference.size() - (5 * BLOCK * 2));
    const std::span<const int16_t> heard(decoded.data() + (5 * BLOCK * 2),
                                         decoded.size() - (5 * BLOCK * 2));
    ASSERT_GE(heard.size(), tail.size());
    EXPECT_EQ(sendspin::server::test::best_lag(tail, heard, 480, 2), 0);
    EXPECT_GT(sendspin::server::test::snr_db(tail, heard), 20.0);
    // The short last cell's 100 samples on their own
    const std::span<const int16_t> last(reference.data() + (50 * BLOCK * 2), 200);
    const std::span<const int16_t> last_heard(decoded.data() + (50 * BLOCK * 2), 200);
    EXPECT_GT(sendspin::server::test::snr_db(last, last_heard), 10.0);
}

// Events pushed before core drains, more than the FIFO has room for at first and more than one
// handler's budget, are delivered once each, in push order
TEST(GroupToCoreFifo, DeliversInOrderPastItsRoomAndItsBudget) {
    using sendspin::server::AnchorShift;
    using sendspin::server::GroupToCoreFifo;
    constexpr int64_t EVENTS = (2 * GroupToCoreFifo::DRAIN_BUDGET) + 3;
    sendspin::server::EventLoop loop;
    std::vector<int64_t> delivered;
    GroupToCoreFifo fifo(loop, [&](GroupEvent& event) {
        delivered.push_back(std::get<AnchorShift>(event).index);
        if (delivered.size() == EVENTS) {
            loop.shutdown(0);
        }
    });
    const GroupToCoreFifo::Producer producer = fifo.producer();
    for (int64_t i = 0; i < EVENTS; ++i) {
        producer.push(AnchorShift{{}, i, 0});
    }
    loop.run();
    ASSERT_EQ(delivered.size(), static_cast<size_t>(EVENTS));
    for (int64_t i = 0; i < EVENTS; ++i) {
        EXPECT_EQ(delivered[static_cast<size_t>(i)], i);
    }
}

// ==============================================================================
// On a group audio thread
// ==============================================================================

// On a group audio thread, woken by the host's calls alone, the pipelines post the epoch's start,
// every frame in index order, the drain and the final index, and core's loop delivers them in that
// order
TEST(GroupAudioOnItsThread, PostsInOrderWhenWokenByTheHost) {
    using sendspin::server::AudioWorkers;
    sendspin::server::EventLoop loop;
    std::mutex mutex;
    std::condition_variable delivered;
    std::vector<GroupEvent> events;
    sendspin::server::GroupToCoreFifo fifo(loop, [&](GroupEvent& event) {
        const std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
        delivered.notify_all();
    });
    const auto view = std::make_shared<HostView>(
        GroupId{1}, InputId{1},
        sendspin::server::InputOptions{InputFormat{RATE, 1, SampleFormat::S16}},
        sendspin::server::GroupParams{LEAD_US, 0, 1000000},
        std::make_shared<sendspin::server::Dispatcher>(), nullptr);
    auto slot = std::make_shared<sendspin::server::PipelineSetSlot>();
    slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}});
    auto audio = std::make_shared<sendspin::server::GroupAudio>(
        view, fifo.producer(), slot, sendspin::server::ResamplerQuality::MEDIUM);
    AudioWorkers workers(2);
    const std::shared_ptr<AudioWorkers::Task> task =
        workers.add([audio] { return audio->run_job(now_us()); });
    view->attach(nullptr, task);
    std::thread core([&] { loop.run(); });

    // Four cells; once their frames are delivered, six more, 100 samples and the drain. No job
    // is run by hand.
    Bytes pcm(CELL * 11 * 2, 0x11);
    const auto wait_for = [&](auto done) {
        std::unique_lock<std::mutex> lock(mutex);
        delivered.wait(lock, [&] { return done(); });
    };
    ASSERT_EQ(view->write(pcm.data(), 4 * CELL).frames, 4 * CELL);
    wait_for([&] { return events.size() == 5; });
    ASSERT_EQ(view->write(pcm.data(), (6 * CELL) + 100).frames, (6 * CELL) + 100);
    view->drain();
    wait_for(
        [&] { return !events.empty() && std::holds_alternative<InstanceFinal>(events.back()); });
    workers.stop();
    // As the group does at its input's end: the view lets go of the task, the task of the job
    view->close();
    task->stop();
    loop.post([&] { loop.shutdown(0); });
    core.join();
    EXPECT_EQ(kinds_of(events), "S" + std::string(11, 'F') + "Df");
    int64_t index = 0;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            EXPECT_EQ(ready->frame.index, index++);
        }
    }
    EXPECT_EQ(std::get<InstanceFinal>(events.back()).final_index, 10);
}

// ==============================================================================
// Visualizer analyses
// ==============================================================================

constexpr sched::InstanceId ANALYSIS{90};

/// The timestamps of the messages of one binary type in an analysis's frames
std::vector<int64_t> message_timestamps(const std::vector<GroupEvent>& events, uint8_t id,
                                        size_t bytes) {
    std::vector<int64_t> out;
    for (const GroupEvent& event : events) {
        const auto* ready = std::get_if<FrameReady>(&event);
        if (ready == nullptr || ready->instance != ANALYSIS) {
            continue;
        }
        const Bytes& payload = ready->frame.payload;
        for (size_t at = 0; at < payload.size(); at += bytes) {
            EXPECT_EQ(payload[at], id);
            int64_t ts = 0;
            for (size_t i = 1; i < 9; ++i) {
                ts = (ts << 8) | payload[at + i];
            }
            out.push_back(ts);
        }
    }
    return out;
}

// An analysis posts a frame for every cell of a drained epoch, in order, holding the messages of
// the instants in it (at 30 per second, `T0 + floor(j * 10^6 / 30)`), then its final index, the
// cell holding the last input sample
TEST(GroupAudioAnalyses, AnalyzeEachCellOnTheTimelineThroughADrain) {
    Pipelines p(InputFormat{RATE, 2, SampleFormat::S16}, 10000000);
    p.slot->publish({}, {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 30, {}}, 0}});
    // Half a second: 25 cells
    constexpr uint64_t SAMPLES = RATE / 2;
    Bytes pcm(SAMPLES * 4, 0x10);
    ASSERT_EQ(p.view->write(pcm.data(), SAMPLES).frames, SAMPLES);
    p.view->drain();
    const std::vector<GroupEvent> events = p.run();
    ASSERT_FALSE(events.empty());
    const int64_t t0 = std::get<EpochStarted>(events.front()).anchor.t0_us;
    std::vector<int64_t> cells;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            EXPECT_EQ(ready->instance, ANALYSIS);
            cells.push_back(ready->frame.index);
        }
    }
    ASSERT_EQ(cells.size(), 25U);
    for (size_t i = 0; i < cells.size(); ++i) {
        EXPECT_EQ(cells[i], static_cast<int64_t>(i));
    }
    EXPECT_EQ(final_indices(events).at(ANALYSIS), 24);
    EXPECT_EQ(drained_last_cell(events), 24);
    std::vector<int64_t> expected;
    // Through the last instant in the last cell, before 500 ms
    for (int64_t j = 0; (j * 1000000 / 30) < 500000; ++j) {
        expected.push_back(t0 + (j * 1000000 / 30));
    }
    EXPECT_EQ(message_timestamps(events, 16, 11), expected);
}

// A message carries its instant shifted as its cell is, so a stall rebase moves the analyses'
// messages with the frames; at 50 per second each cell's message has its frame's timestamp, before
// and after the shift
TEST_F(GroupAudioTest, ShiftsAnalysisMessagesWithTheFrames) {
    this->slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}},
                        {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    this->write(2 * CELL, 0);
    std::vector<GroupEvent> events = this->run();
    constexpr int64_t STALL_US = 2000000;
    this->write(2 * CELL, 0);
    std::vector<GroupEvent> stalled = this->run(STALL_US);
    events.insert(events.end(), stalled.begin(), stalled.end());
    ASSERT_TRUE(std::any_of(events.begin(), events.end(), [](const GroupEvent& event) {
        return std::holds_alternative<sendspin::server::AnchorShift>(event);
    }));
    std::map<int64_t, int64_t> frames;
    std::map<int64_t, int64_t> analyses;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            (ready->instance == ANALYSIS ? analyses : frames)[ready->frame.index] =
                ready->frame.ts_us;
        }
    }
    ASSERT_EQ(frames.size(), 4U);
    EXPECT_EQ(analyses, frames);
    std::vector<int64_t> shifted;
    for (const auto& [index, ts_us] : frames) {
        shifted.push_back(ts_us);
    }
    EXPECT_EQ(message_timestamps(events, 16, 11), shifted);
}

/// The analysis frames among events: index to timestamp
std::map<int64_t, int64_t> analysis_frames(const std::vector<GroupEvent>& events) {
    std::map<int64_t, int64_t> out;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            if (ready->instance == ANALYSIS) {
                out[ready->frame.index] = ready->frame.ts_us;
            }
        }
    }
    return out;
}

// A group with analyses and no frame instance rebases them as it would a one-cell instance with no
// latency, so after a stall its messages are on time again
TEST_F(GroupAudioTest, RebasesAnAnalysisWithNoFrameInstance) {
    this->slot->publish({},
                        {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    this->write(2 * CELL, 0);
    const std::vector<GroupEvent> events = this->run();
    // Control: on time, no shift
    EXPECT_EQ(kinds(events).find('A'), std::string::npos);
    EXPECT_EQ(analysis_frames(events).size(), 2U);
    constexpr int64_t STALL_US = 2000000;
    this->write(2 * CELL, 0);
    const int64_t before = now_us() + STALL_US;
    const std::vector<GroupEvent> stalled = this->run(STALL_US);
    ASSERT_FALSE(stalled.empty());
    ASSERT_TRUE(std::holds_alternative<sendspin::server::AnchorShift>(stalled.front()));
    EXPECT_EQ(std::get<sendspin::server::AnchorShift>(stalled.front()).index, 2);
    const std::map<int64_t, int64_t> frames = analysis_frames(stalled);
    ASSERT_EQ(frames.size(), 2U);
    for (const auto& [index, ts_us] : frames) {
        EXPECT_GE(index, 2);
        EXPECT_GE(ts_us, before + LEAD_US) << index;
    }
}

// `k*` is past every analysis's frontier. An Opus instance's frame 1 waits for its encoder's
// lookahead past cell 2, while an analysis needs no more than cell 1's input, so it analyzes cell 1
// first; a stall then shifts from cell 2, after the cell it analyzed, never under a message already
// produced
TEST(GroupAudioAnalyses, ShiftAfterTheCellsAnAnalysisProduced) {
    Pipelines p(InputFormat{48000, 2, SampleFormat::S16});
    const sched::InstanceId opus{140};
    p.slot->publish({{opus, {}, {sched::Codec::OPUS, 48000, 2, 16}, 0}},
                    {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    const Bytes pcm(3 * 960 * 4, 5);
    ASSERT_EQ(p.view->write(pcm.data(), 2 * 960).frames, 2 * 960U);
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames = frames_by_instance(p.run());
    EXPECT_EQ(frames[opus].size(), 1U);
    EXPECT_EQ(frames[ANALYSIS].size(), 2U);
    constexpr int64_t STALL_US = 2000000;
    EXPECT_TRUE(p.run(STALL_US).empty());
    ASSERT_EQ(p.view->write(pcm.data(), 960).frames, 960U);
    const std::vector<GroupEvent> events = p.run(2 * STALL_US);
    ASSERT_FALSE(events.empty());
    ASSERT_TRUE(std::holds_alternative<sendspin::server::AnchorShift>(events.front()));
    EXPECT_EQ(std::get<sendspin::server::AnchorShift>(events.front()).index, 2);
}

// An analysis that fell behind with its input staged skips the cells that ended by now and restarts
// at the cell playing now, rather than analyze cells every cursor skips and hold the staging ring
// behind the players. Here the frame instance has produced 30 cells ahead of the clock when the
// analysis is added from cell 0, with the clock 5 ms into cell 20
TEST_F(GroupAudioTest, RestartsALateAnalysisAtTheCellPlayingNow) {
    this->write(30 * CELL, 0);
    const int64_t start = now_us();
    while (this->audio.run_job(start)) {}
    const sched::Anchor anchor = job_view_of(*this->view).anchor;
    this->slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}},
                        {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    const int64_t late = anchor.t0_us + (20 * sched::CELL_US) + 5000;
    while (this->audio.run_job(late)) {}
    this->loop.impl().io().restart();
    this->loop.impl().io().poll();
    const std::map<int64_t, int64_t> frames = analysis_frames(std::exchange(this->events, {}));
    std::vector<int64_t> indices;
    for (const auto& [index, ts_us] : frames) {
        indices.push_back(index);
        if (index > 20) {
            EXPECT_GT(ts_us, late) << index;
        }
    }
    EXPECT_EQ(indices, (std::vector<int64_t>{20, 21, 22, 23, 24, 25, 26, 27, 28, 29}));
}

// The staging ring keeps the input an analysis reads next. Its next cell is late but not all
// staged, a stall, which the rebase is held for: the ring keeps the input from where the analysis
// stopped, behind the position playing now
TEST_F(GroupAudioTest, HoldsTheInputOfAnAnalysisWaitingThroughAStall) {
    this->slot->publish({},
                        {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    this->write(CELL + (CELL / 2), 0);
    const int64_t start = now_us();
    while (this->audio.run_job(start)) {}
    const sched::Anchor anchor = job_view_of(*this->view).anchor;
    // Cell 0 analyzed, cell 1's input half staged; the clock a quarter into cell 1
    const int64_t late = anchor.t0_us + sched::CELL_US + (sched::CELL_US / 4);
    while (this->audio.run_job(late)) {}
    EXPECT_LE(job_view_of(*this->view).retain_from, sched::cell_position(anchor, 1));
    // Control: the rest of the input comes, the rebase shifts cell 1, and it is analyzed
    this->write(CELL, 0);
    while (this->audio.run_job(late)) {}
    this->loop.impl().io().restart();
    this->loop.impl().io().poll();
    const std::map<int64_t, int64_t> frames = analysis_frames(std::exchange(this->events, {}));
    EXPECT_TRUE(frames.contains(1));
}

// While the rebase at `k*` waits for its input, a late analysis does not restart at or past `k*`,
// which would move `k*` past the frames the rebase is for. The instance has produced cells 0 and 1
// when the analysis joins from cell 0 with the clock at cell 5; the analysis catches up to `k*` =
// 2, and the input that follows shifts from cell 2
TEST_F(GroupAudioTest, KeepsALateAnalysisBehindAnUndecidedRebase) {
    this->write(2 * CELL, 0);
    const int64_t start = now_us();
    while (this->audio.run_job(start)) {}
    const sched::Anchor anchor = job_view_of(*this->view).anchor;
    this->slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}},
                        {{ANALYSIS, {}, {sendspin::server::visualizer_type::LOUDNESS, 50, {}}, 0}});
    const int64_t late = anchor.t0_us + (5 * sched::CELL_US);
    while (this->audio.run_job(late)) {}
    this->loop.impl().io().restart();
    this->loop.impl().io().poll();
    std::vector<int64_t> indices;
    for (const auto& [index, ts_us] : analysis_frames(std::exchange(this->events, {}))) {
        indices.push_back(index);
    }
    EXPECT_EQ(indices, (std::vector<int64_t>{0, 1}));
    this->write(4 * CELL, 0);
    const std::vector<GroupEvent> events = this->run(late - now_us());
    ASSERT_FALSE(events.empty());
    ASSERT_TRUE(std::holds_alternative<sendspin::server::AnchorShift>(events.front()));
    EXPECT_EQ(std::get<sendspin::server::AnchorShift>(events.front()).index, 2);
}

// A job builds one analysis's FFT and tables at most; another waits for a later job, the window of
// input before its start held in the staging ring meanwhile. As above, three analyses join 30 cells
// ahead, with the clock 5 ms into cell 20: each restarts there
TEST_F(GroupAudioTest, BuildsOneAnalysisPerJobAndHoldsTheOthersInput) {
    namespace type = sendspin::server::visualizer_type;
    const sched::InstanceId second{91};
    const sched::InstanceId third{92};
    this->write(30 * CELL, 0);
    const int64_t start = now_us();
    while (this->audio.run_job(start)) {}
    const sched::Anchor anchor = job_view_of(*this->view).anchor;
    this->slot->publish({{INSTANCE, {}, {sched::Codec::PCM, RATE, 1, 24}, 0}},
                        {{ANALYSIS, {}, {type::LOUDNESS, 50, {}}, 0},
                         {second, {}, {type::LOUDNESS, 25, {}}, 0},
                         {third, {}, {type::LOUDNESS, 10, {}}, 0}});
    const int64_t late = anchor.t0_us + (20 * sched::CELL_US) + 5000;
    const uint64_t held =
        sched::cell_position(anchor, 20) - sendspin::server::analysis_window(RATE);
    // The first analysis builds; the other two wait
    EXPECT_TRUE(this->audio.run_job(late));
    EXPECT_LE(job_view_of(*this->view).retain_from, held);
    while (this->audio.run_job(late)) {}
    // Control: every analysis started, and the ring let go of the window before cell 20
    EXPECT_GT(job_view_of(*this->view).retain_from, held);
    this->loop.impl().io().restart();
    this->loop.impl().io().poll();
    std::map<sched::InstanceId, std::vector<sched::Frame>> frames =
        frames_by_instance(std::exchange(this->events, {}));
    for (const sched::InstanceId id : {ANALYSIS, second, third}) {
        ASSERT_FALSE(frames[id].empty()) << id.value;
        EXPECT_EQ(frames[id].front().index, 20) << id.value;
        EXPECT_EQ(frames[id].back().index, 29) << id.value;
    }
}

}  // namespace
