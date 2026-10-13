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

/// @file pcm_convert.h
/// @brief The host's PCM to a player's PCM at the same rate and channel count: a repack between
/// integer depths, or the quantize stage for float input; and the host's PCM as floats for the
/// stages before it. Pure functions.

#pragma once

#include "sendspin/server/types.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sendspin::server {

/// @brief The bytes of one sample of the host's input
size_t sample_bytes(SampleFormat format);

/// @brief Whether convert_pcm() produces this output bit depth
bool is_pcm_output_depth(uint32_t bit_depth);

/// @brief The bytes of one output sample: bit_depth / 8
size_t pcm_output_bytes(uint32_t bit_depth);

/// @brief Writes in's samples as little-endian signed PCM of out_bit_depth bits (spec
/// player/v1.md "PCM Encoding Convention"). A deeper integer is shifted up; a shallower one is
/// rounded to nearest and clipped; a float is scaled by 2^(bits - 1), rounded to nearest and
/// clipped, with no dither, and NaN becomes 0.
/// @param in whole samples in in_format
/// @param out_bit_depth one is_pcm_output_depth() accepts
/// @param out room for every sample at pcm_output_bytes(out_bit_depth)
void convert_pcm(std::span<const uint8_t> in, SampleFormat in_format, uint32_t out_bit_depth,
                 std::span<uint8_t> out);

/// @brief Writes in's samples as floats with full scale at +-1.0, the form the channel mix and
/// the resampler take: an integer of n bits divided by 2^(n - 1), exactly; a float as it is, with
/// NaN as 0. convert_pcm() of the result from F32 at the input's depth gives back the input.
/// @param in whole samples in in_format
/// @param out room for every sample
void to_float(std::span<const uint8_t> in, SampleFormat in_format, std::span<float> out);

}  // namespace sendspin::server
