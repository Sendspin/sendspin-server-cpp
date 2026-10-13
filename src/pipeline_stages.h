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

/// @file pipeline_stages.h
/// @brief A pipeline instance's stages before the quantize stage: the channel mix, then the
/// resampler, both on floats with full scale at +-1.0

#pragma once

#include "profile.h"
#include "sendspin/server/config.h"
#include "sendspin/server/types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace micro_resampler {
class MicroResampler;
class ResamplerConfig;
}  // namespace micro_resampler

namespace sendspin::server {

/// @brief Whether format selection offers out_channels for an input of in_channels: the same
/// count, any count to mono, or mono to any count
bool channel_mix_supported(uint32_t in_channels, uint32_t out_channels);

/// @brief The channel mix, a fixed matrix with no history. Output channel c is the average of the
/// input channels whose index is c modulo out_channels when there are fewer outputs (to mono, every
/// channel's average), else input channel c modulo in_channels (from mono, the one channel in
/// every output channel); the same count copies. Pairs channel_mix_supported() refuses are mixed
/// only for a stream that continues across GroupInput::set_format().
/// @param in whole frames of in_channels
/// @param out room for as many frames of out_channels
void mix_channels(std::span<const float> in, uint32_t in_channels, std::span<float> out,
                  uint32_t out_channels);

/// @brief A micro-resampler instance as the pipelines run it: FLOAT32 coefficients generated in
/// DOUBLE precision over floats, or with profile::RESAMPLER_Q15 Q15 coefficients generated in
/// SINGLE precision over 16-bit samples; no dither. Its group delay is compensated inside, so its
/// first output sample is aligned with its first input frame.
class Resampler {
public:
    /// @brief The samples process(), prime() and flush() take and give, interleaved
    static constexpr SampleFormat SAMPLE_FORMAT =
        profile::RESAMPLER_Q15 ? SampleFormat::S16 : SampleFormat::F32;

    /// @param max_block the most input frames one process() call passes to micro-resampler; the
    /// history buffer is sized for it, so a running instance never reallocates
    /// @return nullptr when micro-resampler refuses the rates, or an allocation or the block size
    /// fails
    static std::unique_ptr<Resampler> create(uint32_t in_rate, uint32_t out_rate, uint32_t channels,
                                             ResamplerQuality quality, size_t max_block);

    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    /// @brief The micro-resampler configuration create() builds with (q15 =
    /// profile::RESAMPLER_Q15), which tests run their reference with; either arithmetic on any
    /// build, so a host test checks the embedded one too
    static micro_resampler::ResamplerConfig config(ResamplerQuality quality,
                                                   bool q15 = profile::RESAMPLER_Q15);

    /// @brief `N_p` of the resampler create() makes for the rates, without building its filter
    /// bank; 0 for equal rates or rates micro-resampler refuses
    static uint32_t filter_length(uint32_t in_rate, uint32_t out_rate, ResamplerQuality quality);

    /// @brief `N_p / 2 + 1`: the input frames an output sample needs past its own time
    size_t reach() const {
        return this->reach_;
    }

    /// @brief `N_p`
    uint32_t filter_length() const {
        return this->filter_length_;
    }

    /// @brief Starts a new stream with empty history
    void reset();

    /// @brief Fills the history by extrapolating back from first, the stream's first block,
    /// which process() is then given as well
    /// @param first whole frames in SAMPLE_FORMAT
    bool prime(std::span<const uint8_t> first);

    /// @brief Appends the output in's frames yield to out, at most max_block frames per
    /// micro-resampler call
    /// @param in whole frames in SAMPLE_FORMAT; out gets frames in it
    /// @return false when micro-resampler refused
    bool process(std::span<const uint8_t> in, std::vector<uint8_t>& out);

    /// @brief Appends the filter's tail to out, until micro-resampler reports it flushed
    bool flush(std::vector<uint8_t>& out);

private:
    Resampler(std::unique_ptr<micro_resampler::MicroResampler> resampler, uint32_t channels,
              size_t max_block, size_t output_frames);

    std::unique_ptr<micro_resampler::MicroResampler> resampler_;
    size_t max_block_;
    uint32_t filter_length_;
    size_t reach_;
    /// The bytes of one frame in SAMPLE_FORMAT
    size_t frame_bytes_;
    /// One call's output: max_block's frames at the output rate, plus two
    std::vector<uint8_t> scratch_;
};

}  // namespace sendspin::server
