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

#include "pipeline_stages.h"

#include "pcm_convert.h"
#include <micro_resampler.h>

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

micro_resampler::ResamplerQuality to_micro(ResamplerQuality quality) {
    switch (quality) {
        case ResamplerQuality::VERY_LOW:
            return micro_resampler::ResamplerQuality::VERY_LOW;
        case ResamplerQuality::LOW:
            return micro_resampler::ResamplerQuality::LOW;
        case ResamplerQuality::MEDIUM:
            break;
        case ResamplerQuality::HIGH:
            return micro_resampler::ResamplerQuality::HIGH;
        case ResamplerQuality::VERY_HIGH:
            return micro_resampler::ResamplerQuality::VERY_HIGH;
    }
    return micro_resampler::ResamplerQuality::MEDIUM;
}

}  // namespace

// ==============================================================================
// Channel mix
// ==============================================================================

bool channel_mix_supported(uint32_t in_channels, uint32_t out_channels) {
    return in_channels > 0 && out_channels > 0 &&
           (in_channels == out_channels || in_channels == 1 || out_channels == 1);
}

void mix_channels(std::span<const float> in, uint32_t in_channels, std::span<float> out,
                  uint32_t out_channels) {
    const size_t frames = in.size() / in_channels;
    if (in_channels == out_channels) {
        std::copy(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(frames * in_channels),
                  out.begin());
        return;
    }
    for (size_t frame = 0; frame < frames; ++frame) {
        const float* from = in.data() + (frame * in_channels);
        float* to = out.data() + (frame * out_channels);
        for (uint32_t channel = 0; channel < out_channels; ++channel) {
            if (in_channels < out_channels) {
                to[channel] = from[channel % in_channels];
                continue;
            }
            // Summed in double, so the average is rounded to float once
            double sum = 0;
            uint32_t count = 0;
            for (uint32_t source = channel; source < in_channels; source += out_channels) {
                sum += from[source];
                ++count;
            }
            to[channel] = static_cast<float>(sum / count);
        }
    }
}

// ==============================================================================
// Resampler
// ==============================================================================

// The configuration every pipeline's resampler is made with
micro_resampler::ResamplerConfig Resampler::config(ResamplerQuality quality, bool q15) {
    // Q15 coefficients take Q15 samples: float samples convert through double there, emulated on
    // a single-precision FPU
    const auto coefficients =
        q15 ? micro_resampler::CoefficientFormat::Q15 : micro_resampler::CoefficientFormat::FLOAT32;
    const auto samples =
        q15 ? micro_resampler::SampleFormat::Q15 : micro_resampler::SampleFormat::FLOAT32;
    return micro_resampler::ResamplerConfig()
        .set_coefficient_format(coefficients)
        .set_input_format(samples)
        .set_output_format(samples)
        .set_quality(to_micro(quality))
        // 16-bit coefficients need no double to generate them, which a target without a
        // double-precision FPU emulates
        .set_precision(q15 ? micro_resampler::PrecisionStrategyType::SINGLE
                           : micro_resampler::PrecisionStrategyType::DOUBLE);
}

std::unique_ptr<Resampler> Resampler::create(uint32_t in_rate, uint32_t out_rate, uint32_t channels,
                                             ResamplerQuality quality, size_t max_block) {
    std::unique_ptr<micro_resampler::MicroResampler> resampler =
        micro_resampler::MicroResampler::create(in_rate, out_rate, static_cast<int>(channels),
                                                config(quality).set_max_block_size(max_block));
    if (!resampler || !resampler->initialize() || resampler->get_max_block_size() < max_block) {
        return nullptr;
    }
    const size_t output_frames = max_block * out_rate / in_rate + 2;
    return std::unique_ptr<Resampler>(
        new Resampler(std::move(resampler), channels, max_block, output_frames));
}

Resampler::Resampler(std::unique_ptr<micro_resampler::MicroResampler> resampler, uint32_t channels,
                     size_t max_block, size_t output_frames)
    : resampler_(std::move(resampler)),
      max_block_(max_block),
      filter_length_(static_cast<uint32_t>(this->resampler_->get_filter_length())),
      reach_((this->filter_length_ / 2) + 1),
      frame_bytes_(sample_bytes(SAMPLE_FORMAT) * channels),
      scratch_(output_frames * this->frame_bytes_) {}

Resampler::~Resampler() = default;

uint32_t Resampler::filter_length(uint32_t in_rate, uint32_t out_rate, ResamplerQuality quality) {
    if (in_rate == out_rate) {
        return 0;
    }
    // The design is resolved when the instance is made; the filter bank waits for initialize()
    const std::unique_ptr<micro_resampler::MicroResampler> resampler =
        micro_resampler::MicroResampler::create(in_rate, out_rate, 1, config(quality));
    return resampler ? static_cast<uint32_t>(resampler->get_filter_length()) : 0;
}

void Resampler::reset() {
    this->resampler_->reset();
}

bool Resampler::prime(std::span<const uint8_t> first) {
    return this->resampler_->prime_raw(first.data(), first.size() / this->frame_bytes_);
}

bool Resampler::process(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
    const size_t capacity = this->scratch_.size() / this->frame_bytes_;
    size_t frames = in.size() / this->frame_bytes_;
    const uint8_t* from = in.data();
    while (frames > 0) {
        size_t consumed = 0;
        size_t generated = 0;
        if (!this->resampler_->process_block_raw(from, std::min(frames, this->max_block_),
                                                 this->scratch_.data(), capacity, consumed,
                                                 generated)) {
            return false;
        }
        out.insert(
            out.end(), this->scratch_.begin(),
            this->scratch_.begin() + static_cast<std::ptrdiff_t>(generated * this->frame_bytes_));
        // A call that takes nothing would never end
        if (consumed == 0 && generated == 0) {
            return false;
        }
        frames -= consumed;
        from += consumed * this->frame_bytes_;
    }
    return true;
}

bool Resampler::flush(std::vector<uint8_t>& out) {
    const size_t capacity = this->scratch_.size() / this->frame_bytes_;
    while (!this->resampler_->is_flushed()) {
        size_t generated = 0;
        if (!this->resampler_->flush_raw(this->scratch_.data(), capacity, generated)) {
            return false;
        }
        out.insert(
            out.end(), this->scratch_.begin(),
            this->scratch_.begin() + static_cast<std::ptrdiff_t>(generated * this->frame_bytes_));
        if (generated == 0) {
            break;
        }
    }
    return true;
}

}  // namespace sendspin::server
