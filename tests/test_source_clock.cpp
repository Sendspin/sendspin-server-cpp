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

/// @file test_source_clock.cpp
/// @brief A routed source's clock correction (roles/source/v1.md "Source Audio Chunks (Binary)"):
/// when SourceClock starts correcting and which way, the steps and outliers it keeps out of its
/// loop, its lock through SampleSlip and SourceAsrc against a model source whose clock drifts, and
/// the two corrections on their own

#include "source_clock.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

using sendspin::server::ResamplerQuality;
using sendspin::server::SampleSlip;
using sendspin::server::SOURCE_CLOCK_ENGAGE_US;
using sendspin::server::SOURCE_CLOCK_MAX_PPM;
using sendspin::server::SOURCE_CLOCK_STEP_US;
using sendspin::server::SourceAsrc;
using sendspin::server::SourceClock;

constexpr uint32_t RATE = 48000;
constexpr size_t CHUNK = RATE / 50;
constexpr int64_t CHUNK_US = 20000;

// ==============================================================================
// SourceClock, open loop: the positions ignore its correction
// ==============================================================================

struct Row {
    std::string name;
    /// The capture time of chunk k less its nominal time
    std::function<int64_t(int)> error_us;
    /// A chunk before which the source dropped one 3 ms block, which the clock is told of
    std::optional<int> skipped;
    bool engaged;
    /// The sign of the correction once engaged: positive drops audio
    int sign;
};

TEST(SourceClock, CorrectsOnlyASourceThatDriftsAndTheRightWay) {
    std::mt19937 random(7);
    std::uniform_int_distribution<int64_t> jitter(-1500, 1500);
    std::vector<int64_t> noise(1000);
    for (int64_t& value : noise) {
        value = jitter(random);
    }
    constexpr int64_t SKIPPED_US = 3000;
    const Row rows[] = {
        {"Control: a source on the server's clock", [](int) { return 0; }, {}, false, 0},
        {"Capture jitter averages out", [&](int k) { return noise[k]; }, {}, false, 0},
        {"The reference chunk's own error is centered away",
         [](int k) { return k == 0 ? 4000 : 0; },
         {},
         false,
         0},
        // Positions that run ahead of the capture times: more frames than the time they took
        {"A fast source clock is slowed by dropping audio",
         [](int k) { return -int64_t{k} * CHUNK_US / 1000; },
         {},
         true,
         1},
        {"A slow one is sped up by adding audio",
         [](int k) { return int64_t{k} * CHUNK_US / 1000; },
         {},
         true,
         -1},
        {"An outlier stays out of the loop",
         [](int k) { return k == 120 ? 200000 : 0; },
         {},
         false,
         0},
        {"A step is absorbed, and drift after it corrected",
         [](int k) { return k < 120 ? 0 : 30000 + (int64_t{k - 120} * CHUNK_US / 1000); },
         {},
         true,
         -1},
        {"Audio dropped before correction moves the reference",
         [](int k) { return k < 120 ? 0 : SKIPPED_US; }, 120, false, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        SourceClock clock;
        int64_t position = 0;
        for (int k = 0; k < 1000; ++k) {
            if (row.skipped == k) {
                clock.skip(SKIPPED_US);
            }
            const int64_t capture = 1000000 + (int64_t{k} * CHUNK_US) + row.error_us(k);
            (void)clock.observe(capture, static_cast<double>(position), RATE, CHUNK);
            position += CHUNK;
        }
        EXPECT_EQ(clock.engaged(), row.engaged);
        if (row.sign == 0) {
            EXPECT_EQ(clock.ppm(), 0.0);
        } else {
            EXPECT_GT(clock.ppm() * row.sign, 0.0) << clock.ppm();
        }
    }
}

// Clamped at the most it corrects, the loop holds its integral: once the error returns to zero
// the correction leaves the clamp at the next step, where a wound-up integral would hold it there
TEST(SourceClock, HoldsItsIntegralWhileClamped) {
    SourceClock clock;
    int64_t position = 0;
    for (int k = 0; k < 6000; ++k) {
        // An error just short of a step from the third second on, which the integral takes past
        // the clamp, then none from the 100th
        const bool late = k >= 150 && k < 5000;
        const int64_t error = late ? SOURCE_CLOCK_STEP_US - 1 : 0;
        (void)clock.observe(1000000 + (int64_t{k} * CHUNK_US) + error,
                            static_cast<double>(position), RATE, CHUNK);
        position += CHUNK;
        if (k == 4999) {
            EXPECT_EQ(clock.ppm(), -SOURCE_CLOCK_MAX_PPM);
        }
    }
    EXPECT_GT(clock.ppm(), -SOURCE_CLOCK_MAX_PPM);
}

// ==============================================================================
// The loop closed through each correction, against a model source
// ==============================================================================

/// A model source whose sample clock runs drift_ppm fast against the server's, stamping each 20 ms
/// chunk with its true capture time plus up to 1 ms of seeded jitter. After lock, the time each
/// chunk's first frame plays at its corrected position, less its true capture time, holds within
/// a quarter of a millisecond; the bound comes from the model's clock, not from the loop.
void expect_lock(double drift_ppm, bool asrc) {
    constexpr uint32_t rate = 8000;
    constexpr size_t chunk = rate / 50;
    constexpr int seconds = 240;
    std::mt19937 random(static_cast<uint32_t>(drift_ppm + 1000));
    std::uniform_real_distribution<double> jitter(-1000.0, 1000.0);
    SourceClock clock;
    SampleSlip slip;
    std::unique_ptr<SourceAsrc> resampler;
    if (asrc) {
        resampler = SourceAsrc::create(rate, 1, ResamplerQuality::LOW, chunk);
        ASSERT_NE(resampler, nullptr);
    }
    std::vector<float> in(chunk, 0.0F);
    std::vector<float> out(SourceAsrc::max_output(chunk));
    std::vector<uint8_t> pcm(chunk * 2, 0);
    std::vector<uint8_t> slipped(SampleSlip::max_output(chunk) * 2);
    double slip_position = 0.0;
    // Each second's mean of play time less capture time, over the last fifty seconds
    std::vector<double> offsets;
    double sum = 0.0;
    const int chunks = seconds * 50;
    for (int k = 0; k < chunks; ++k) {
        const double true_capture_us =
            static_cast<double>(k) * chunk * 1e6 / (rate * (1.0 + (drift_ppm * 1e-6)));
        const auto capture = static_cast<int64_t>(std::llround(true_capture_us + jitter(random)));
        const double position = asrc ? resampler->next_position() : slip_position;
        const double ppm = clock.observe(capture, position, rate, chunk);
        if (asrc) {
            ASSERT_TRUE(
                resampler->process(in.data(), chunk, clock.engaged() ? ppm : 0.0, out.data()));
        } else {
            slip_position += static_cast<double>(slip.slip(
                pcm.data(), chunk, 2, clock.engaged() ? static_cast<int32_t>(std::lround(ppm)) : 0,
                slipped.data()));
        }
        if (k >= 190 * 50) {
            sum += (position * 1e6 / rate) - true_capture_us;
            if (k % 50 == 49) {
                offsets.push_back(sum / 50);
                sum = 0.0;
            }
        }
    }
    EXPECT_TRUE(clock.engaged());
    const auto [low, high] = std::minmax_element(offsets.begin(), offsets.end());
    EXPECT_LT(*high - *low, 250.0) << "a correction of " << clock.ppm() << " ppm";
}

// Two hundred and forty seconds of chunks per run at 8 kHz, as the loop settles within about a
// hundred and ninety; each correction against a slow and a fast clock
TEST(SourceClock, LocksADriftingSourceThroughEitherCorrection) {
    for (const bool asrc : {false, true}) {
        for (const double drift : asrc ? std::vector{-150.0, 350.0} : std::vector{-400.0, 120.0}) {
            SCOPED_TRACE(std::string(asrc ? "ASRC" : "slip") + " at " + std::to_string(drift));
            expect_lock(drift, asrc);
        }
    }
}

// ==============================================================================
// SampleSlip
// ==============================================================================

/// Frames numbered from 0, two bytes each
std::vector<uint8_t> numbered(size_t frames, size_t from = 0) {
    std::vector<uint8_t> pcm(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<uint16_t>(from + i);
        std::memcpy(&pcm[i * 2], &value, 2);
    }
    return pcm;
}

std::vector<uint16_t> numbers(const std::vector<uint8_t>& pcm, size_t frames) {
    std::vector<uint16_t> values(frames);
    std::memcpy(values.data(), pcm.data(), frames * 2);
    return values;
}

// One frame each 10^6 / |ppm| frames, dropped for a positive rate and written twice for a
// negative one, the phase carried across calls, the rate clamped at SOURCE_CLOCK_MAX_PPM
TEST(SampleSlip, SlipsOneFrameEachMillionOverTheRate) {
    struct SlipRow {
        std::string name;
        int32_t ppm;
        /// Frames per call, over two calls
        size_t frames;
        std::vector<size_t> dropped;
        std::vector<size_t> repeated;
    };
    const SlipRow rows[] = {
        {"Control: no correction copies", 0, 3000, {}, {}},
        {"Drops each 2,000th frame at 500 ppm", 500, 3000, {1999, 3999, 5999}, {}},
        {"Repeats at -500 ppm", -500, 3000, {}, {1999, 3999, 5999}},
        {"Clamps past the largest rate", 2000, 3000, {1999, 3999, 5999}, {}},
        {"Carries the phase across calls", 250, 3000, {3999}, {}},
    };
    for (const SlipRow& row : rows) {
        SCOPED_TRACE(row.name);
        SampleSlip slip;
        std::vector<uint16_t> out;
        for (size_t call = 0; call < 2; ++call) {
            const std::vector<uint8_t> in = numbered(row.frames, call * row.frames);
            std::vector<uint8_t> written(SampleSlip::max_output(row.frames) * 2);
            const size_t count = slip.slip(in.data(), row.frames, 2, row.ppm, written.data());
            ASSERT_LE(count, SampleSlip::max_output(row.frames));
            const std::vector<uint16_t> values = numbers(written, count);
            out.insert(out.end(), values.begin(), values.end());
        }
        std::vector<uint16_t> expected;
        for (size_t i = 0; i < 2 * row.frames; ++i) {
            if (std::find(row.dropped.begin(), row.dropped.end(), i) != row.dropped.end()) {
                continue;
            }
            expected.push_back(static_cast<uint16_t>(i));
            if (std::find(row.repeated.begin(), row.repeated.end(), i) != row.repeated.end()) {
                expected.push_back(static_cast<uint16_t>(i));
            }
        }
        EXPECT_EQ(out, expected);
    }
}

// ==============================================================================
// SourceAsrc
// ==============================================================================

// Primed with its first block and at unity, the ASRC's output lines up with its input: frame for
// frame a low tone that starts at its peak, to the filter's error, from the first frame, and the
// next input frame lands at the input's count.
// That is what lets the correction start in the middle of a stream without a gap or a repeat.
TEST(SourceAsrc, LinesUpWithItsInputAtUnity) {
    const std::unique_ptr<SourceAsrc> asrc =
        SourceAsrc::create(RATE, 1, ResamplerQuality::MEDIUM, CHUNK);
    ASSERT_NE(asrc, nullptr);
    std::vector<float> in(CHUNK);
    std::vector<float> tone;
    std::vector<float> out;
    std::vector<float> block(SourceAsrc::max_output(CHUNK));
    for (size_t chunk = 0; chunk < 50; ++chunk) {
        for (size_t i = 0; i < CHUNK; ++i) {
            const double t = static_cast<double>((chunk * CHUNK) + i) / RATE;
            in[i] = static_cast<float>(0.5 * std::cos(2.0 * std::numbers::pi * 200.0 * t));
        }
        tone.insert(tone.end(), in.begin(), in.end());
        const std::optional<size_t> count = asrc->process(in.data(), CHUNK, 0.0, block.data());
        ASSERT_TRUE(count);
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*count));
        EXPECT_EQ(asrc->next_position(), static_cast<double>((chunk + 1) * CHUNK));
    }
    ASSERT_GT(out.size(), CHUNK);
    double worst = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(out[i] - tone[i])));
    }
    EXPECT_LT(worst, 1e-3);
}

// A positive correction writes fewer frames, a negative one more, each by the ratio; the next
// input frame's position follows the frames written plus what the filter holds at the current
// ratio, and reset() starts the positions over
TEST(SourceAsrc, WritesByTheRatioAndTracksWhereInputLands) {
    for (const double ppm : {SOURCE_CLOCK_MAX_PPM, -SOURCE_CLOCK_MAX_PPM}) {
        SCOPED_TRACE(ppm);
        const std::unique_ptr<SourceAsrc> asrc =
            SourceAsrc::create(RATE, 2, ResamplerQuality::LOW, CHUNK);
        ASSERT_NE(asrc, nullptr);
        const std::vector<float> in(CHUNK * 2, 0.25F);
        std::vector<float> out(SourceAsrc::max_output(CHUNK) * 2);
        constexpr size_t CHUNKS = 250;
        for (size_t chunk = 0; chunk < CHUNKS; ++chunk) {
            ASSERT_TRUE(asrc->process(in.data(), CHUNK, ppm, out.data()));
        }
        // 5 s at the ratio: 120 frames fewer, or more
        const double expected = CHUNKS * CHUNK / (1.0 + (ppm * 1e-6));
        EXPECT_NEAR(asrc->next_position(), expected, 0.01);
        EXPECT_NEAR(static_cast<double>(asrc->produced()), expected, 64.0);
        asrc->reset();
        EXPECT_EQ(asrc->next_position(), 0.0);
        ASSERT_TRUE(asrc->process(in.data(), CHUNK, 0.0, out.data()));
        EXPECT_EQ(asrc->next_position(), static_cast<double>(CHUNK));
    }
}

}  // namespace
