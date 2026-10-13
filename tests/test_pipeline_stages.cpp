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

/// @file test_pipeline_stages.cpp
/// @brief A pipeline's stages before the quantize stage: the channel mix's matrix and the resampler
/// as the pipelines configure it

#include "pcm_convert.h"
#include "pipeline_stages.h"
#include <gtest/gtest.h>
#include <micro_resampler.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace {

using sendspin::server::convert_pcm;
using sendspin::server::Resampler;
using sendspin::server::ResamplerQuality;
using sendspin::server::sample_bytes;
using sendspin::server::SampleFormat;

// A fixed matrix with no history; to mono the average of every channel, from mono the one channel
// everywhere. Format selection offers only those; a stream that continues across a set_format()
// into a count it does not offer folds channels by index (fewer outputs average the inputs
// congruent to theirs) or repeats them (more outputs).
TEST(ChannelMix, MixesEachFrameByTheFixedMatrix) {
    struct Row {
        const char* name;
        std::vector<float> in;
        uint32_t in_channels;
        uint32_t out_channels;
        std::vector<float> out;
        bool offered{true};
    };
    const Row rows[] = {
        {"Control: the same count copies",
         {0.25F, -0.5F, 1.0F, 0.0F},
         2,
         2,
         {0.25F, -0.5F, 1.0F, 0.0F}},
        {"stereo to mono averages", {0.25F, -0.5F, 1.0F, 0.5F}, 2, 1, {-0.125F, 0.75F}},
        {"three channels to mono averages", {0.3F, 0.6F, 0.9F}, 3, 1, {0.6F}},
        {"mono to stereo duplicates", {0.25F, -1.0F}, 1, 2, {0.25F, 0.25F, -1.0F, -1.0F}},
        {"mono to four channels duplicates", {0.5F}, 1, 4, {0.5F, 0.5F, 0.5F, 0.5F}},
        {"six channels to stereo folds by index",
         {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F},
         6,
         2,
         {0.3F, 0.4F},
         false},
        {"five channels to stereo averages what each output gets",
         {1, 2, 3, 4, 5},
         5,
         2,
         {3, 3},
         false},
        {"stereo to six channels repeats",
         {0.25F, -0.5F},
         2,
         6,
         {0.25F, -0.5F, 0.25F, -0.5F, 0.25F, -0.5F},
         false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::channel_mix_supported(row.in_channels, row.out_channels),
                  row.offered);
        std::vector<float> out(row.out.size(), -7.0F);
        sendspin::server::mix_channels(row.in, row.in_channels, out, row.out_channels);
        ASSERT_EQ(out.size(), row.out.size());
        for (size_t i = 0; i < out.size(); ++i) {
            EXPECT_FLOAT_EQ(out[i], row.out[i]) << i;
        }
    }
    EXPECT_FALSE(sendspin::server::channel_mix_supported(0, 1));
}

std::unique_ptr<Resampler> resampler_44_to_48(size_t max_block) {
    return Resampler::create(44100, 48000, 2, ResamplerQuality::MEDIUM, max_block);
}

// MEDIUM from 44.1 to 48 kHz has a 104-tap filter, so an output sample needs 53 input frames past
// it; a zero rate builds nothing. Blocks larger than the block size go through in pieces with the
// same output, and a flushed stream's output is ceil(N * out / in), one past the rounded count the
// pipelines cut it to: 435,375 for 400,000 input frames.
TEST(PipelineResampler, ConvertsAWholeStreamInBlocks) {
    EXPECT_EQ(Resampler::create(0, 48000, 2, ResamplerQuality::MEDIUM, 882), nullptr);
    EXPECT_EQ(Resampler::create(44100, 48000, 0, ResamplerQuality::MEDIUM, 882), nullptr);
    std::unique_ptr<Resampler> cells = resampler_44_to_48(882);
    std::unique_ptr<Resampler> pieces = resampler_44_to_48(300);
    ASSERT_TRUE(cells && pieces);
    EXPECT_EQ(cells->reach(), 53U);

    constexpr size_t FRAMES = 400000;
    std::vector<float> floats(FRAMES * 2);
    for (size_t i = 0; i < FRAMES; ++i) {
        floats[i * 2] = static_cast<float>(0.5 * std::sin(static_cast<double>(i) * 0.01));
        floats[(i * 2) + 1] = static_cast<float>(0.25 * std::sin(static_cast<double>(i) * 0.3));
    }
    // In the samples the profile's resampler takes
    const size_t frame_bytes = sample_bytes(Resampler::SAMPLE_FORMAT) * 2;
    std::vector<uint8_t> input(FRAMES * frame_bytes);
    const std::span<const uint8_t> float_bytes(reinterpret_cast<const uint8_t*>(floats.data()),
                                               floats.size() * sizeof(float));
    if (Resampler::SAMPLE_FORMAT == SampleFormat::F32) {
        std::copy(float_bytes.begin(), float_bytes.end(), input.begin());
    } else {
        convert_pcm(float_bytes, SampleFormat::F32, 16, input);
    }
    const auto block = [&](size_t at, size_t frames) {
        return std::span<const uint8_t>(input.data() + (at * frame_bytes), frames * frame_bytes);
    };
    std::vector<uint8_t> by_cell;
    std::vector<uint8_t> by_piece;
    ASSERT_TRUE(cells->prime(block(0, 882)));
    ASSERT_TRUE(pieces->prime(block(0, 882)));
    for (size_t at = 0; at < FRAMES; at += 882) {
        ASSERT_TRUE(cells->process(block(at, std::min<size_t>(882, FRAMES - at)), by_cell));
    }
    ASSERT_TRUE(pieces->process(input, by_piece));
    ASSERT_TRUE(cells->flush(by_cell));
    ASSERT_TRUE(pieces->flush(by_piece));
    EXPECT_EQ(by_cell.size(), size_t{435375} * frame_bytes);
    EXPECT_EQ(by_piece, by_cell);

    // After reset() the instance starts a new stream
    cells->reset();
    std::vector<uint8_t> again;
    ASSERT_TRUE(cells->prime(block(0, 882)));
    ASSERT_TRUE(cells->process(block(0, 882), again));
    ASSERT_FALSE(again.empty());
    EXPECT_TRUE(std::equal(again.begin(), again.end(), by_cell.begin()));
}

/// micro-resampler from 44.1 to 48 kHz in stereo over a whole signal in one block, as
/// Resampler::config() builds it with Q15 arithmetic (samples rounded to 16 bits in, read back over
/// 2^15) or FLOAT32
std::vector<float> resample_whole(const std::vector<float>& input, bool q15) {
    const size_t frames = input.size() / 2;
    micro_resampler::ResamplerConfig config = Resampler::config(ResamplerQuality::MEDIUM, q15);
    std::unique_ptr<micro_resampler::MicroResampler> resampler =
        micro_resampler::MicroResampler::create(44100, 48000, 2, config.set_max_block_size(frames));
    EXPECT_TRUE(resampler && resampler->initialize());
    const size_t capacity = (frames * 48000 / 44100) + 2;
    std::vector<float> out(capacity * 2);
    size_t consumed = 0;
    size_t generated = 0;
    if (!q15) {
        EXPECT_TRUE(resampler->process_block(input.data(), frames, out.data(), capacity, consumed,
                                             generated));
    } else {
        std::vector<uint8_t> samples(input.size() * 2);
        convert_pcm(std::span(reinterpret_cast<const uint8_t*>(input.data()), input.size() * 4),
                    SampleFormat::F32, 16, samples);
        std::vector<uint8_t> produced(capacity * 4);
        EXPECT_TRUE(resampler->process_block_raw(samples.data(), frames, produced.data(), capacity,
                                                 consumed, generated));
        sendspin::server::to_float(std::span(produced.data(), generated * 4), SampleFormat::S16,
                                   out);
    }
    out.resize(generated * 2);
    return out;
}

struct Distance {
    double snr_db;
    double worst_steps;
};

/// How far got is from expected past the warm-up: the SNR, and the largest error in 16-bit steps
Distance distance(const std::vector<float>& got, const std::vector<float>& expected) {
    double signal = 0;
    double noise = 0;
    double worst = 0;
    for (size_t i = 200 * 2; i < std::min(got.size(), expected.size()); ++i) {
        const double error = static_cast<double>(got[i]) - expected[i];
        signal += static_cast<double>(expected[i]) * expected[i];
        noise += error * error;
        worst = std::max(worst, std::abs(error));
    }
    return {noise == 0 ? 200.0 : 10.0 * std::log10(signal / noise), worst * 32768.0};
}

// The Q15 resampler the embedded profile runs stays within 16-bit precision of the FLOAT32 one, on
// any build: 16-bit tones from 44.1 to 48 kHz, against FLOAT32 coefficients over the same samples
// as floats, after the warm-up
TEST(PipelineResampler, StaysWithinSixteenBitsOfTheFloatResampler) {
    struct Row {
        const char* name;
        double left_a;
        double left_b;
        double right;
        double min_snr_db;
        double max_steps;
    };
    // Measured on a host: 93.7 dB and 0.75 of a step; 97.8 dB and 0.88 near full scale; a quiet
    // signal 0.50 of a step, its SNR set by the 16-bit samples themselves
    for (const Row& row : {Row{"tones", 0.7, 0.1, 0.5, 90.0, 1.0},
                           Row{"near full scale", 0.999, 0.0, 0.999, 94.0, 1.0},
                           Row{"quiet", 0.01, 0.001, 0.005, 50.0, 1.0}}) {
        SCOPED_TRACE(row.name);
        constexpr size_t FRAMES = 44100;
        std::vector<float> floats(FRAMES * 2);
        for (size_t i = 0; i < FRAMES; ++i) {
            const double t = static_cast<double>(i) / 44100.0;
            const double left = (row.left_a * std::sin(2 * std::numbers::pi * 997 * t)) +
                                (row.left_b * std::sin(2 * std::numbers::pi * 7001 * t));
            const double right = row.right * std::sin(2 * std::numbers::pi * 3001 * t);
            floats[i * 2] = static_cast<float>(std::round(left * 32767.0) / 32768.0);
            floats[(i * 2) + 1] = static_cast<float>(std::round(right * 32767.0) / 32768.0);
        }
        const std::vector<float> reference = resample_whole(floats, false);
        const Distance d = distance(resample_whole(floats, true), reference);
        EXPECT_GE(d.snr_db, row.min_snr_db);
        EXPECT_LE(d.worst_steps, row.max_steps);
    }
}

// Core sizes a live group's lead by Resampler::filter_length() before any resampler is built, and
// the pipelines rebase by the built one's, so the two agree for upsampling and downsampling at
// every quality; equal rates need no filter
TEST(PipelineResampler, KnowsItsFilterLengthWithoutBuildingIt) {
    struct Pair {
        uint32_t in;
        uint32_t out;
    };
    for (const Pair pair : {Pair{44100, 48000}, Pair{48000, 11025}, Pair{96000, 44100}}) {
        for (const ResamplerQuality quality :
             {ResamplerQuality::VERY_LOW, ResamplerQuality::LOW, ResamplerQuality::MEDIUM,
              ResamplerQuality::HIGH, ResamplerQuality::VERY_HIGH}) {
            SCOPED_TRACE(testing::Message() << pair.in << " to " << pair.out << " quality "
                                            << static_cast<int>(quality));
            const std::unique_ptr<Resampler> built =
                Resampler::create(pair.in, pair.out, 2, quality, 1024);
            ASSERT_NE(built, nullptr);
            EXPECT_GT(built->filter_length(), 0U);
            EXPECT_EQ(Resampler::filter_length(pair.in, pair.out, quality), built->filter_length());
        }
    }
    EXPECT_EQ(Resampler::filter_length(44100, 48000, ResamplerQuality::MEDIUM), 104U);
    EXPECT_EQ(Resampler::filter_length(48000, 48000, ResamplerQuality::MEDIUM), 0U);
}

}  // namespace
