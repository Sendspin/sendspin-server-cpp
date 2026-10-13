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

/// @file test_micro_flac.cpp
/// @brief The micro-flac encoder builds with the server's encoder settings, and one 20 ms block
/// it encodes decodes back to the same samples.

#include <gtest/gtest.h>
#include <micro_flac/flac_decoder.h>
#include <micro_flac/flac_encoder.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using micro_flac::FLACDecoder;
using micro_flac::FLACEncoder;
using micro_flac::FLACEncoderOptions;
using micro_flac::PcmFormat;

constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint8_t CHANNELS = 2;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr uint32_t CELL_FRAMES = 882;  // 20 ms at 44.1 kHz

TEST(MicroFlac, EncodedCellDecodesToTheSameSamples) {
    FLACEncoderOptions options;
    options.block_size = CELL_FRAMES;
    options.lpc = true;
    options.max_lpc_order = 12;
    options.lpc_stereo_search = true;
    options.max_rice_partition_order = 6;
    // The encoder cannot be moved, so the server holds it in a unique_ptr
    auto encoder =
        std::make_unique<FLACEncoder>(PcmFormat{SAMPLE_RATE, CHANNELS, BITS_PER_SAMPLE}, options);

    std::vector<int16_t> pcm(static_cast<size_t>(CELL_FRAMES) * CHANNELS);
    for (size_t frame = 0; frame < CELL_FRAMES; ++frame) {
        const double phase =
            2.0 * 3.141592653589793 * 440.0 * static_cast<double>(frame) / SAMPLE_RATE;
        pcm[frame * CHANNELS] = static_cast<int16_t>(12000.0 * std::sin(phase));
        pcm[frame * CHANNELS + 1] = static_cast<int16_t>(8000.0 * std::sin(phase + 0.5));
    }
    std::vector<uint8_t> input(pcm.size() * sizeof(int16_t));
    std::memcpy(input.data(), pcm.data(), input.size());  // little-endian host
    ASSERT_EQ(input.size(), encoder->get_input_block_bytes());

    std::vector<uint8_t> stream(FLACEncoder::HEADER_BYTES + encoder->get_max_output_bytes());
    size_t header_bytes = 0;
    ASSERT_EQ(encoder->write_header(stream.data(), stream.size(), header_bytes),
              micro_flac::FLAC_ENCODER_SUCCESS);
    ASSERT_EQ(header_bytes, FLACEncoder::HEADER_BYTES);
    size_t consumed = 0;
    size_t frame_bytes = 0;
    ASSERT_EQ(encoder->encode(input.data(), input.size(), stream.data() + header_bytes,
                              stream.size() - header_bytes, consumed, frame_bytes),
              micro_flac::FLAC_ENCODER_SUCCESS);
    EXPECT_EQ(consumed, input.size());
    ASSERT_GT(frame_bytes, 0U);
    EXPECT_LT(frame_bytes, input.size());
    stream.resize(header_bytes + frame_bytes);

    FLACDecoder decoder;
    std::vector<int16_t> decoded(pcm.size());
    size_t offset = 0;
    size_t samples = 0;
    while (offset < stream.size() && samples == 0) {
        size_t used = 0;
        const micro_flac::FLACDecoderResult result =
            decoder.decode(stream.data() + offset, stream.size() - offset,
                           reinterpret_cast<uint8_t*>(decoded.data()),
                           decoded.size() * sizeof(int16_t), used, samples);
        ASSERT_GE(result, 0) << "decoder error " << static_cast<int>(result);
        ASSERT_TRUE(used > 0 || samples > 0) << "decoder made no progress";
        offset += used;
    }
    ASSERT_EQ(samples, pcm.size());
    EXPECT_EQ(decoded, pcm);
}

}  // namespace
