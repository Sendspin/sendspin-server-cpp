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

/// @file test_micro_resampler.cpp
/// @brief micro-resampler builds with the configuration the server's pipelines use on a host
/// (FLOAT32 coefficients, DOUBLE precision) and converts one 20 ms cell from 44.1 to 48 kHz.

#include <gtest/gtest.h>
#include <micro_resampler.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

namespace {

using micro_resampler::CoefficientFormat;
using micro_resampler::MicroResampler;
using micro_resampler::PrecisionStrategyType;
using micro_resampler::ResamplerConfig;
using micro_resampler::ResamplerQuality;

constexpr uint32_t INPUT_RATE = 44100;
constexpr uint32_t OUTPUT_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr size_t CELL_FRAMES = 882;  // 20 ms at 44.1 kHz

TEST(MicroResampler, ConvertsOneCellTo48kHz) {
    std::unique_ptr<MicroResampler> resampler =
        MicroResampler::create(INPUT_RATE, OUTPUT_RATE, CHANNELS,
                               ResamplerConfig()
                                   .set_coefficient_format(CoefficientFormat::FLOAT32)
                                   .set_quality(ResamplerQuality::MEDIUM)
                                   .set_precision(PrecisionStrategyType::DOUBLE));
    ASSERT_NE(resampler, nullptr);
    ASSERT_TRUE(resampler->initialize());
    EXPECT_EQ(resampler->get_coefficient_precision(), PrecisionStrategyType::DOUBLE);

    resampler->set_max_block_size(CELL_FRAMES);
    ASSERT_GE(resampler->get_max_block_size(), CELL_FRAMES);

    std::vector<float> input(CELL_FRAMES * CHANNELS);
    for (size_t frame = 0; frame < CELL_FRAMES; ++frame) {
        const float sample =
            0.5F * std::sin(2.0F * 3.14159265F * 1000.0F * static_cast<float>(frame) / INPUT_RATE);
        input[frame * CHANNELS] = sample;
        input[frame * CHANNELS + 1] = -sample;
    }
    const size_t output_capacity = CELL_FRAMES * OUTPUT_RATE / INPUT_RATE + 2;
    std::vector<float> output(output_capacity * CHANNELS);

    ASSERT_TRUE(resampler->prime(input.data(), CELL_FRAMES));
    size_t consumed = 0;
    size_t generated = 0;
    ASSERT_TRUE(resampler->process_block(input.data(), CELL_FRAMES, output.data(), output_capacity,
                                         consumed, generated));
    EXPECT_EQ(consumed, CELL_FRAMES);
    // The first block's output starts after the filter's half length, so it is short of a cell.
    EXPECT_GT(generated, 0U);
    EXPECT_LT(generated, CELL_FRAMES * OUTPUT_RATE / INPUT_RATE);
    for (size_t frame = 0; frame < generated; ++frame) {
        EXPECT_FLOAT_EQ(output[frame * CHANNELS], -output[frame * CHANNELS + 1]);
    }
}

}  // namespace
