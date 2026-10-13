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

#include "pcm_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sendspin::server {

namespace {

constexpr uint32_t BITS_PER_BYTE = 8;

/// A little-endian signed integer of bytes bytes
int64_t read_int(const uint8_t* in, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= uint64_t{in[i]} << (BITS_PER_BYTE * i);
    }
    const uint32_t bits = static_cast<uint32_t>(bytes) * BITS_PER_BYTE;
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>(value ^ sign) - static_cast<int64_t>(sign);
}

void write_int(uint8_t* out, int64_t value, size_t bytes) {
    const auto bits = static_cast<uint64_t>(value);
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<uint8_t>(bits >> (BITS_PER_BYTE * i));
    }
}

/// write_int() for a value that fits 32 bits, with no 64-bit shift
void write_int32(uint8_t* out, int32_t value, size_t bytes) {
    const auto bits = static_cast<uint32_t>(value);
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<uint8_t>(bits >> (BITS_PER_BYTE * i));
    }
}

int64_t clip(int64_t value, uint32_t bits) {
    const int64_t max = (int64_t{1} << (bits - 1)) - 1;
    return std::clamp(value, -max - 1, max);
}

/// An integer sample of in_bits bits at out_bits bits
int64_t rescale(int64_t value, uint32_t in_bits, uint32_t out_bits) {
    if (out_bits >= in_bits) {
        return value * (int64_t{1} << (out_bits - in_bits));
    }
    const uint32_t shift = in_bits - out_bits;
    // Round half up, then floor: an arithmetic shift of a value offset by half a step
    return clip((value + (int64_t{1} << (shift - 1))) >> shift, out_bits);
}

/// The float quantize stage, in float and int32_t alone: on a single-precision FPU double
/// arithmetic and 64-bit conversions are library calls per sample
/// @param scale 2^(bits - 1)
/// @param top 2^(bits - 1) - 1
int32_t quantize(float sample, float scale, int32_t top) {
    if (std::isnan(sample)) {
        return 0;
    }
    if (sample >= 1.0F) {
        return top;
    }
    if (sample <= -1.0F) {
        return -top - 1;
    }
    // Exact: a power of two times a float in (-1, 1), below 2^31 in magnitude; the fraction left
    // by truncation is exact too, so the result rounds half away from zero as llround() does
    const float scaled = sample * scale;
    auto whole = static_cast<int32_t>(scaled);
    const float fraction = scaled - static_cast<float>(whole);
    if (fraction >= 0.5F) {
        ++whole;
    } else if (fraction <= -0.5F) {
        --whole;
    }
    return std::min(whole, top);
}

}  // namespace

size_t sample_bytes(SampleFormat format) {
    switch (format) {
        case SampleFormat::S16:
            return 2;
        case SampleFormat::S24:
            return 3;
        case SampleFormat::F32:
            break;
    }
    return 4;
}

bool is_pcm_output_depth(uint32_t bit_depth) {
    return bit_depth == 16 || bit_depth == 24 || bit_depth == 32;
}

size_t pcm_output_bytes(uint32_t bit_depth) {
    return bit_depth / BITS_PER_BYTE;
}

void convert_pcm(std::span<const uint8_t> in, SampleFormat in_format, uint32_t out_bit_depth,
                 std::span<uint8_t> out) {
    const size_t in_bytes = sample_bytes(in_format);
    const size_t out_bytes = pcm_output_bytes(out_bit_depth);
    const size_t samples = in.size() / in_bytes;
    if (in_format == SampleFormat::F32) {
        const uint32_t half = uint32_t{1} << (out_bit_depth - 1);
        const auto scale = static_cast<float>(half);
        const auto top = static_cast<int32_t>(half - 1);
        for (size_t i = 0; i < samples; ++i) {
            float sample = 0;
            std::memcpy(&sample, in.data() + (i * in_bytes), sizeof(sample));
            write_int32(out.data() + (i * out_bytes), quantize(sample, scale, top), out_bytes);
        }
        return;
    }
    const auto in_bits = static_cast<uint32_t>(in_bytes) * BITS_PER_BYTE;
    for (size_t i = 0; i < samples; ++i) {
        const int64_t value =
            rescale(read_int(in.data() + (i * in_bytes), in_bytes), in_bits, out_bit_depth);
        write_int(out.data() + (i * out_bytes), value, out_bytes);
    }
}

void to_float(std::span<const uint8_t> in, SampleFormat in_format, std::span<float> out) {
    const size_t in_bytes = sample_bytes(in_format);
    const size_t samples = in.size() / in_bytes;
    const uint8_t* source = in.data();
    // One loop per format, each sample through int32_t: an integer of at most 24 bits converts to
    // float exactly, and a 32-bit target has no 64-bit integer to float conversion, which is a
    // library call per sample there
    switch (in_format) {
        case SampleFormat::S16: {
            constexpr float SCALE = 1.0F / 32768.0F;
            for (size_t i = 0; i < samples; ++i, source += 2) {
                const auto value = static_cast<int16_t>(source[0] | (source[1] << BITS_PER_BYTE));
                out[i] = static_cast<float>(value) * SCALE;
            }
            return;
        }
        case SampleFormat::S24: {
            constexpr float SCALE = 1.0F / 8388608.0F;
            for (size_t i = 0; i < samples; ++i, source += 3) {
                // The 24 bits in the top of a uint32_t, then an arithmetic shift down
                const uint32_t bits = (uint32_t{source[0]} << BITS_PER_BYTE) |
                                      (uint32_t{source[1]} << (2 * BITS_PER_BYTE)) |
                                      (uint32_t{source[2]} << (3 * BITS_PER_BYTE));
                out[i] = static_cast<float>(static_cast<int32_t>(bits) >> BITS_PER_BYTE) * SCALE;
            }
            return;
        }
        case SampleFormat::F32:
            break;
    }
    for (size_t i = 0; i < samples; ++i, source += sizeof(float)) {
        float sample = 0;
        std::memcpy(&sample, source, sizeof(sample));
        out[i] = std::isnan(sample) ? 0.0F : sample;
    }
}

}  // namespace sendspin::server
