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

/// @file test_pcm_convert.cpp
/// @brief The host's PCM to a player's: repacking between integer depths and quantizing floats

#include "pcm_convert.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace {

using sendspin::server::SampleFormat;
using Bytes = std::vector<uint8_t>;

Bytes float_bytes(float value) {
    Bytes bytes(4);
    std::memcpy(bytes.data(), &value, 4);
    return bytes;
}

// spec player/v1.md "PCM Encoding Convention": little-endian signed integers, 24-bit packed in
// 3 bytes; rounding and clipping, no dither
TEST(PcmConvert, RepacksAndQuantizesOneSample) {
    struct Row {
        const char* name;
        Bytes in;
        SampleFormat format;
        uint32_t bits;
        Bytes out;
    };
    const Row rows[] = {
        {"Control: 16 to 16 copies", {0x34, 0x12}, SampleFormat::S16, 16, {0x34, 0x12}},
        {"16 to 24 shifts up", {0x34, 0x92}, SampleFormat::S16, 24, {0x00, 0x34, 0x92}},
        {"16 to 32 shifts up", {0xFF, 0xFF}, SampleFormat::S16, 32, {0x00, 0x00, 0xFF, 0xFF}},
        {"24 to 24 copies", {0x01, 0x02, 0x83}, SampleFormat::S24, 24, {0x01, 0x02, 0x83}},
        {"24 to 16 rounds half up", {0x80, 0x34, 0x12}, SampleFormat::S24, 16, {0x35, 0x12}},
        {"24 to 16 rounds down below half",
         {0x7F, 0x34, 0x12},
         SampleFormat::S24,
         16,
         {0x34, 0x12}},
        {"24 to 16 rounds a negative half up",
         {0x80, 0xFF, 0xFF},
         SampleFormat::S24,
         16,
         {0x00, 0x00}},
        {"24 to 16 clips the top", {0xFF, 0xFF, 0x7F}, SampleFormat::S24, 16, {0xFF, 0x7F}},
        {"24 to 16 keeps the bottom", {0x00, 0x00, 0x80}, SampleFormat::S24, 16, {0x00, 0x80}},
        {"float half scale", float_bytes(0.5F), SampleFormat::F32, 16, {0x00, 0x40}},
        {"float full scale clips", float_bytes(1.0F), SampleFormat::F32, 16, {0xFF, 0x7F}},
        {"float past full scale clips",
         float_bytes(3.0F),
         SampleFormat::F32,
         24,
         {0xFF, 0xFF, 0x7F}},
        {"float negative full scale",
         float_bytes(-1.0F),
         SampleFormat::F32,
         24,
         {0x00, 0x00, 0x80}},
        {"float rounds to nearest",
         float_bytes(1.6F / 32768.0F),
         SampleFormat::F32,
         16,
         {0x02, 0x00}},
        {"float rounds a half away from zero",
         float_bytes(2.5F / 32768.0F),
         SampleFormat::F32,
         16,
         {0x03, 0x00}},
        {"float rounds a negative half away from zero",
         float_bytes(-2.5F / 32768.0F),
         SampleFormat::F32,
         16,
         {0xFD, 0xFF}},
        {"float rounds a negative below half toward zero",
         float_bytes(-2.25F / 32768.0F),
         SampleFormat::F32,
         16,
         {0xFE, 0xFF}},
        {"float just below full scale rounds up and clips",
         float_bytes(std::nextafter(1.0F, 0.0F)),
         SampleFormat::F32,
         16,
         {0xFF, 0x7F}},
        {"float just below full scale to 32 bits",
         float_bytes(std::nextafter(1.0F, 0.0F)),
         SampleFormat::F32,
         32,
         {0x80, 0xFF, 0xFF, 0x7F}},
        {"float at 2^23 + 1 to 32 bits stays exact",
         float_bytes(static_cast<float>((1 << 23) + 1) / 2147483648.0F),
         SampleFormat::F32,
         32,
         {0x01, 0x00, 0x80, 0x00}},
        {"float just below a half rounds down",
         float_bytes(std::nextafter(0.5F, 0.0F) / 32768.0F),
         SampleFormat::F32,
         16,
         {0x00, 0x00}},
        {"float +inf clips to the top",
         float_bytes(std::numeric_limits<float>::infinity()),
         SampleFormat::F32,
         16,
         {0xFF, 0x7F}},
        {"float -inf clips to the bottom",
         float_bytes(-std::numeric_limits<float>::infinity()),
         SampleFormat::F32,
         24,
         {0x00, 0x00, 0x80}},
        {"float full scale to 32 bits clips",
         float_bytes(1.0F),
         SampleFormat::F32,
         32,
         {0xFF, 0xFF, 0xFF, 0x7F}},
        {"float negative full scale to 32 bits",
         float_bytes(-1.0F),
         SampleFormat::F32,
         32,
         {0x00, 0x00, 0x00, 0x80}},
        {"float half scale to 32 bits",
         float_bytes(0.5F),
         SampleFormat::F32,
         32,
         {0x00, 0x00, 0x00, 0x40}},
        {"float NaN is silence",
         float_bytes(std::numeric_limits<float>::quiet_NaN()),
         SampleFormat::F32,
         16,
         {0x00, 0x00}},
        {"float NaN is silence at 32 bits",
         float_bytes(std::numeric_limits<float>::quiet_NaN()),
         SampleFormat::F32,
         32,
         {0x00, 0x00, 0x00, 0x00}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Bytes out(row.out.size(), 0xEE);
        sendspin::server::convert_pcm(row.in, row.format, row.bits, out);
        EXPECT_EQ(out, row.out);
    }
}

TEST(PcmConvert, ConvertsEverySampleOfABlock) {
    const Bytes in{0x01, 0x00, 0x02, 0x00, 0x03, 0x00};
    Bytes out(9);
    sendspin::server::convert_pcm(in, SampleFormat::S16, 24, out);
    EXPECT_EQ(out, (Bytes{0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00}));
    EXPECT_TRUE(sendspin::server::is_pcm_output_depth(24));
    EXPECT_FALSE(sendspin::server::is_pcm_output_depth(20));
    EXPECT_EQ(sendspin::server::sample_bytes(SampleFormat::S24), 3U);
}

// The stages before the quantize stage take floats; an integer of n bits is exactly itself over
// 2^(n - 1), so the quantize stage at the input's depth gives it back
TEST(PcmConvert, TakesTheHostsPcmToFloats) {
    struct Row {
        const char* name;
        Bytes in;
        SampleFormat format;
        float value;
    };
    const Row rows[] = {
        {"Control: a 16-bit sample", {0x00, 0x40}, SampleFormat::S16, 0.5F},
        {"16-bit full scale", {0x00, 0x80}, SampleFormat::S16, -1.0F},
        {"16-bit one step", {0x01, 0x00}, SampleFormat::S16, 1.0F / 32768},
        {"24-bit one step below zero", {0xFF, 0xFF, 0xFF}, SampleFormat::S24, -1.0F / 8388608},
        {"24-bit largest", {0xFF, 0xFF, 0x7F}, SampleFormat::S24, 8388607.0F / 8388608},
        {"a float as it is", float_bytes(1.5F), SampleFormat::F32, 1.5F},
        {"float NaN is silence", float_bytes(std::numeric_limits<float>::quiet_NaN()),
         SampleFormat::F32, 0.0F},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        float out = -7.0F;
        sendspin::server::to_float(row.in, row.format, std::span<float>(&out, 1));
        EXPECT_EQ(out, row.value);
    }
    for (const SampleFormat format : {SampleFormat::S16, SampleFormat::S24}) {
        const uint32_t bits = format == SampleFormat::S16 ? 16 : 24;
        const Bytes in = format == SampleFormat::S16 ? Bytes{0x01, 0x80, 0xFF, 0x7F, 0x35, 0x12}
                                                     : Bytes{0x01, 0x00, 0x80, 0xFF, 0xFF, 0x7F};
        std::vector<float> floats(in.size() / (bits / 8));
        sendspin::server::to_float(in, format, floats);
        Bytes back(in.size());
        sendspin::server::convert_pcm(
            std::span(reinterpret_cast<const uint8_t*>(floats.data()), floats.size() * 4),
            SampleFormat::F32, bits, back);
        EXPECT_EQ(back, in) << bits;
    }
}

}  // namespace
