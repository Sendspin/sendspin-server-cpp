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

/// @file test_live_slew.cpp
/// @brief A live input played fast to lower its latency, through the ASRC and by dropping frames:
/// what it writes continues the frames written before it, at the slew's rate, in the input's
/// format, and the time it reports taken out

#include "live_slew.h"
#include "sched/constants.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

namespace {

using sendspin::server::InputFormat;
using sendspin::server::LIVE_SLEW_BLOCK_FRAMES;
using sendspin::server::LiveSlew;
using sendspin::server::ResamplerQuality;
using sendspin::server::SampleFormat;
using sendspin::server::sched::LOWERING_SLEW_PPM;

constexpr uint32_t RATE = 48000;
/// The frames written as they came before the slew starts, and the slew's own
constexpr size_t BEFORE = 4800;
constexpr size_t SLEWED = 2 * RATE;
constexpr size_t BLOCK = RATE / 100;

/// A 200 Hz tone at half scale, from its peak
double tone(double frame) {
    return 0.5 * std::cos(2.0 * std::numbers::pi * 200.0 * frame / RATE);
}

/// Frame k of the tone in format
std::vector<uint8_t> tone_frame(size_t k, SampleFormat format) {
    const double value = tone(static_cast<double>(k));
    if (format == SampleFormat::F32) {
        const auto sample = static_cast<float>(value);
        std::vector<uint8_t> bytes(sizeof(float));
        std::memcpy(bytes.data(), &sample, sizeof(float));
        return bytes;
    }
    const auto sample = static_cast<int16_t>(std::lround(value * 32768.0));
    std::vector<uint8_t> bytes(sizeof(int16_t));
    std::memcpy(bytes.data(), &sample, sizeof(int16_t));
    return bytes;
}

double sample_at(const std::vector<uint8_t>& out, size_t frame, SampleFormat format) {
    if (format == SampleFormat::F32) {
        float sample = 0.0F;
        std::memcpy(&sample, &out[frame * sizeof(float)], sizeof(float));
        return sample;
    }
    int16_t sample = 0;
    std::memcpy(&sample, &out[frame * sizeof(int16_t)], sizeof(int16_t));
    return sample / 32768.0;
}

struct Row {
    std::string name;
    bool asrc;
    SampleFormat format;
    /// The frames written before it that start() takes
    size_t history;
};

// Two seconds slewed at the slew rate after BEFORE frames written as they came: the output after
// them continues the tone with no gap or repeat at the start, frame j of it the input's frame
// BEFORE + j * (1 + ppm), through the ASRC to its filter's error and by dropping frames exactly one
// input frame within a frame of it; the time reported taken out is the rate's share
TEST(LiveSlew, ContinuesTheFramesBeforeItAtTheSlewRate) {
    const Row rows[] = {
        {"The ASRC, from its history", true, SampleFormat::F32, LIVE_SLEW_BLOCK_FRAMES},
        {"The ASRC, in the input's integer format", true, SampleFormat::S16,
         LIVE_SLEW_BLOCK_FRAMES},
        {"The ASRC with no history, at a segment's start", true, SampleFormat::F32, 0},
        {"Dropped frames", false, SampleFormat::S16, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const InputFormat format{RATE, 1, row.format};
        LiveSlew slew;
        slew.prepare(format, row.asrc, ResamplerQuality::MEDIUM);
        const size_t frame_bytes = tone_frame(0, row.format).size();
        // The frames before the slew, then its own
        std::vector<uint8_t> in;
        for (size_t k = 0; k < BEFORE + SLEWED; ++k) {
            const std::vector<uint8_t> bytes = tone_frame(k, row.format);
            in.insert(in.end(), bytes.begin(), bytes.end());
        }
        if (slew.awaits_start()) {
            std::memcpy(slew.history().data(), &in[(BEFORE - row.history) * frame_bytes],
                        row.history * frame_bytes);
            slew.start(row.history);
        }
        ASSERT_EQ(slew.engaged(), row.asrc);
        std::vector<uint8_t> out;
        int64_t slewed_us = 0;
        for (size_t at = BEFORE; at < BEFORE + SLEWED; at += BLOCK) {
            const LiveSlew::Block block = slew.process(
                &in[at * frame_bytes], BLOCK, static_cast<int32_t>(LOWERING_SLEW_PPM), 1000000);
            ASSERT_EQ(block.consumed, BLOCK);
            out.insert(out.end(), slew.output(), slew.output() + (block.written * frame_bytes));
            slewed_us += slew.take_slewed_us();
        }
        const double ratio = 1.0 + (static_cast<double>(LOWERING_SLEW_PPM) * 1e-6);
        const size_t written = out.size() / frame_bytes;
        ASSERT_GT(written, SLEWED - 100);
        double worst = 0.0;
        for (size_t j = 0; j < written; ++j) {
            const double at = static_cast<double>(BEFORE) + (static_cast<double>(j) * ratio);
            const double value = sample_at(out, j, row.format);
            if (row.asrc) {
                worst = std::max(worst, std::abs(value - tone(at)));
                continue;
            }
            // A dropped frame's output is one input frame as it came, within a frame of the rate
            const auto low = static_cast<size_t>(std::floor(at));
            const std::vector<uint8_t> want_low = tone_frame(low, row.format);
            const std::vector<uint8_t> want_high = tone_frame(low + 1, row.format);
            const bool exact =
                std::memcmp(&out[j * frame_bytes], want_low.data(), frame_bytes) == 0 ||
                std::memcmp(&out[j * frame_bytes], want_high.data(), frame_bytes) == 0;
            ASSERT_TRUE(exact) << j;
        }
        // A frame off at the start would leave 0.013; the integer format adds its own step
        EXPECT_LT(worst, 2e-3);
        // 2 s at 300 ppm: 600 us, to a frame
        EXPECT_NEAR(static_cast<double>(slewed_us), 600.0, 21.0);
    }
}

// The slew writes no more than its capacity. Dropped frames take only as many frames as it; the
// ASRC takes the whole block and keeps back what it wrote past the capacity for the next call, so
// a run whose calls each hold a frame back writes what one with room writes
TEST(LiveSlew, WritesNoMoreThanItsCapacityAndKeepsBackTheRest) {
    LiveSlew dropping;
    dropping.prepare({RATE, 1, SampleFormat::S16}, false, ResamplerQuality::MEDIUM);
    const std::vector<uint8_t> flat(LIVE_SLEW_BLOCK_FRAMES * 2, 3);
    const LiveSlew::Block dropped = dropping.process(flat.data(), LIVE_SLEW_BLOCK_FRAMES, 300, 100);
    EXPECT_EQ(dropped.consumed, 100U);
    EXPECT_LE(dropped.written, 100U);

    std::vector<uint8_t> in;
    for (size_t k = 0; k < 20 * BLOCK; ++k) {
        const std::vector<uint8_t> bytes = tone_frame(k, SampleFormat::F32);
        in.insert(in.end(), bytes.begin(), bytes.end());
    }
    std::vector<uint8_t> outputs[2];
    for (const size_t held : {size_t{0}, size_t{1}}) {
        SCOPED_TRACE(held);
        LiveSlew slew;
        slew.prepare({RATE, 1, SampleFormat::F32}, true, ResamplerQuality::MEDIUM);
        slew.start(0);
        for (size_t at = 0; at < 20 * BLOCK; at += BLOCK) {
            const size_t capacity = held == 0 ? 1000000 : BLOCK - 1;
            const LiveSlew::Block block =
                slew.process(&in[at * sizeof(float)], BLOCK, 300, capacity);
            ASSERT_EQ(block.consumed, BLOCK);
            ASSERT_LE(block.written, capacity);
            outputs[held].insert(outputs[held].end(), slew.output(),
                                 slew.output() + (block.written * sizeof(float)));
        }
    }
    ASSERT_GT(outputs[1].size(), 18 * BLOCK * sizeof(float));
    EXPECT_TRUE(std::equal(outputs[1].begin(), outputs[1].end(), outputs[0].begin()));
}

// An ASRC that refuses a block falls the slew back to dropped frames until it is prepared again,
// restarted or not, and the slew keeps it, so the write it refused in frees nothing. White-box:
// LiveSlew::asrc_.
TEST(LiveSlew, FallsBackToDroppedFramesKeepingTheAsrc) {
    std::vector<uint8_t> in;
    for (size_t k = 0; k < BLOCK; ++k) {
        const std::vector<uint8_t> bytes = tone_frame(k, SampleFormat::F32);
        in.insert(in.end(), bytes.begin(), bytes.end());
    }
    LiveSlew slew;
    slew.prepare({RATE, 1, SampleFormat::F32}, true, ResamplerQuality::MEDIUM);
    slew.start(0);
    ASSERT_TRUE(slew.engaged());
    // An empty first block cannot prime it
    EXPECT_EQ(slew.process(in.data(), 0, 300, 1000000).written, 0U);
    EXPECT_FALSE(slew.engaged());
    EXPECT_NE(slew.asrc_, nullptr);
    slew.restart();
    EXPECT_FALSE(slew.awaits_start());
    // At no slew a block passes as it came
    const LiveSlew::Block block = slew.process(in.data(), BLOCK, 0, 1000000);
    ASSERT_EQ(block.written, BLOCK);
    EXPECT_EQ(std::memcmp(slew.output(), in.data(), in.size()), 0);
    // Control: prepared again, it waits for start()
    slew.prepare({RATE, 1, SampleFormat::F32}, true, ResamplerQuality::MEDIUM);
    EXPECT_TRUE(slew.awaits_start());
}

}  // namespace
