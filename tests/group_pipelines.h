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

/// @file group_pipelines.h
/// @brief Pipelines, a group's pipelines over an input of any format and kind, with their jobs and
/// the FIFO's `Wakeup` run on the test thread, and what the tests read of their output: frames,
/// samples, a reference resampling, a test tone

#pragma once

#include "clock.h"
#include "dispatcher.h"
#include "frame_payloads.h"
#include "group_audio.h"
#include "host_view.h"
#include "resampler_reference.h"
#include "transport.h"
#include "transport_asio.h"
#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace sendspin::server::test {

/// Each instance's frames among events, in order
inline std::map<sched::InstanceId, std::vector<sched::Frame>> frames_by_instance(
    const std::vector<GroupEvent>& events) {
    std::map<sched::InstanceId, std::vector<sched::Frame>> out;
    for (const GroupEvent& event : events) {
        if (const auto* ready = std::get_if<FrameReady>(&event)) {
            out[ready->instance].push_back(copy_of(ready->frame));
        }
    }
    return out;
}

/// Appends event, a frame in a store of its own, so a frame the test holds returns its instance's
/// slot
inline void keep_event(std::vector<GroupEvent>& events, GroupEvent& event) {
    if (auto* ready = std::get_if<FrameReady>(&event)) {
        ready->frame = copy_of(ready->frame);
    }
    events.push_back(std::move(event));
}

/// A group's pipelines over an input of any format and kind
struct Pipelines {
    /// The group lead `G`
    static constexpr int64_t LEAD_US = 300000;

    explicit Pipelines(const InputFormat& format, int64_t write_ahead_us = 1000000)
        : Pipelines(InputOptions{format}, write_ahead_us) {}

    explicit Pipelines(const InputOptions& options, int64_t write_ahead_us = 1000000)
        : view(std::make_shared<HostView>(GroupId{1}, InputId{1}, options,
                                          GroupParams{LEAD_US, 0, write_ahead_us},
                                          std::make_shared<Dispatcher>(), nullptr)),
          fifo(this->loop, [this](GroupEvent& event) { keep_event(this->events, event); }),
          audio(this->view, this->fifo.producer(), this->slot, ResamplerQuality::MEDIUM) {}

    /// @param late_us how far past now_us() the jobs run
    std::vector<GroupEvent> run(int64_t late_us = 0) {
        while (this->audio.run_job(now_us() + late_us)) {}
        this->loop.impl().io().restart();
        this->loop.impl().io().poll();
        return std::exchange(this->events, {});
    }

    EventLoop loop;
    std::shared_ptr<HostView> view;
    std::vector<GroupEvent> events;
    GroupToCoreFifo fifo;
    std::shared_ptr<PipelineSetSlot> slot = std::make_shared<PipelineSetSlot>();
    GroupAudio audio;
};

/// A little-endian signed sample of bytes bytes
inline int32_t sample_at(std::span<const uint8_t> payload, size_t index, size_t bytes) {
    uint32_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= uint32_t{payload[(index * bytes) + i]} << (8 * i);
    }
    const uint32_t sign = 1U << ((8 * bytes) - 1);
    return static_cast<int32_t>(value ^ sign) - static_cast<int32_t>(sign);
}

/// Every frame's samples of one instance, concatenated
inline std::vector<uint8_t> concatenated(const std::vector<sched::Frame>& frames) {
    std::vector<uint8_t> out;
    for (const sched::Frame& frame : frames) {
        out.insert(out.end(), frame.payload.bytes().begin(), frame.payload.bytes().end());
    }
    return out;
}

/// micro-resampler run directly, as the pipelines configure it, over a whole stream: primed with
/// the first block, fed in blocks of block frames, flushed, then cut to `round(N * out / in)`
inline std::vector<float> resample_offline(const std::vector<float>& input, uint32_t channels,
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
inline std::map<sched::InstanceId, int64_t> final_indices(const std::vector<GroupEvent>& events) {
    std::map<sched::InstanceId, int64_t> out;
    for (const GroupEvent& event : events) {
        if (const auto* final = std::get_if<InstanceFinal>(&event)) {
            out[final->instance] = final->final_index;
        }
    }
    return out;
}

/// The last cell a drain reports, alone or, staged before the epoch's first job, with its start
inline std::optional<int64_t> drained_last_cell(const std::vector<GroupEvent>& events) {
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

/// The quantize stage's rule for a float at 16 bits: rounded, clipped, no dither
inline int32_t quantize16(float sample) {
    const double scaled = std::clamp(static_cast<double>(sample), -2.0, 2.0) * 32768.0;
    return static_cast<int32_t>(std::clamp<int64_t>(std::llround(scaled), -32768, 32767));
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

    std::vector<uint8_t> pcm;
    std::vector<float> floats;
};

/// Every 16-bit sample of out from at on is the reference's, quantized
inline void expect_resampled(const std::vector<uint8_t>& out, size_t at,
                             const std::vector<float>& reference) {
    ASSERT_GE(out.size() / 2, at + reference.size());
    for (size_t i = 0; i < reference.size(); ++i) {
        ASSERT_EQ(sample_at(out, at + i, 2), quantize16(reference[i])) << i;
    }
}

/// Every event the pipelines post until they post no more
inline std::vector<GroupEvent> run_all(Pipelines& p) {
    std::vector<GroupEvent> events = p.run();
    for (std::vector<GroupEvent> more = p.run(); !more.empty(); more = p.run()) {
        events.insert(events.end(), std::make_move_iterator(more.begin()),
                      std::make_move_iterator(more.end()));
    }
    return events;
}

/// A live input's options: its write jitter `J_in`
inline InputOptions live_input(const InputFormat& format, int64_t jitter_us) {
    InputOptions options{format};
    options.kind = InputKind::LIVE;
    options.write_jitter_us = jitter_us;
    return options;
}

}  // namespace sendspin::server::test
