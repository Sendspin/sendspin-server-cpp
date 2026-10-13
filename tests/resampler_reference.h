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

/// @file resampler_reference.h
/// @brief micro-resampler run directly, configured as the pipelines configure it on the build's
/// profile (Resampler::config()), with float samples in and out: the reference the resampled
/// pipelines' output is compared with

#pragma once

#include "pcm_convert.h"
#include "pipeline_stages.h"
#include "sendspin/server/config.h"
#include <micro_resampler.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace sendspin::server::test {

/// @brief On the Q15 profile each input sample is rounded to 16 bits, as the pipelines' quantize
/// stage rounds a sample for the resampler, and each output sample reads back as itself over
/// 2^15, which a 16-bit or 24-bit instance quantizes exactly; on a host floats pass as they are
class ReferenceResampler {
public:
    ReferenceResampler(uint32_t in_rate, uint32_t out_rate, uint32_t channels, size_t max_block)
        : channels_(channels) {
        this->resampler_ = micro_resampler::MicroResampler::create(
            in_rate, out_rate, static_cast<int>(channels),
            Resampler::config(ResamplerQuality::MEDIUM).set_max_block_size(max_block));
        if (this->resampler_ && !this->resampler_->initialize()) {
            this->resampler_.reset();
        }
    }

    bool valid() const {
        return this->resampler_ != nullptr;
    }

    bool prime(const float* input, size_t frames) {
        return this->resampler_->prime_raw(this->samples(input, frames), frames);
    }

    bool process_block(const float* input, size_t frames, float* output, size_t capacity,
                       size_t& consumed, size_t& generated) {
        const void* in = this->samples(input, frames);
        this->out_.resize(capacity * this->frame_bytes());
        if (!this->resampler_->process_block_raw(in, frames, this->out_.data(), capacity, consumed,
                                                 generated)) {
            return false;
        }
        this->floats(output, generated);
        return true;
    }

    bool flush(float* output, size_t capacity, size_t& generated) {
        this->out_.resize(capacity * this->frame_bytes());
        if (!this->resampler_->flush_raw(this->out_.data(), capacity, generated)) {
            return false;
        }
        this->floats(output, generated);
        return true;
    }

    bool is_flushed() const {
        return this->resampler_->is_flushed();
    }

private:
    size_t frame_bytes() const {
        return sample_bytes(Resampler::SAMPLE_FORMAT) * this->channels_;
    }

    const void* samples(const float* input, size_t frames) {
        const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(input),
                                             frames * this->channels_ * sizeof(float));
        if (Resampler::SAMPLE_FORMAT == SampleFormat::F32) {
            return input;
        }
        this->in_.resize(frames * this->frame_bytes());
        convert_pcm(bytes, SampleFormat::F32, 16, this->in_);
        return this->in_.data();
    }

    void floats(float* output, size_t frames) {
        to_float(std::span<const uint8_t>(this->out_.data(), frames * this->frame_bytes()),
                 Resampler::SAMPLE_FORMAT, std::span<float>(output, frames * this->channels_));
    }

    std::unique_ptr<micro_resampler::MicroResampler> resampler_;
    uint32_t channels_;
    std::vector<uint8_t> in_;
    std::vector<uint8_t> out_;
};

}  // namespace sendspin::server::test
