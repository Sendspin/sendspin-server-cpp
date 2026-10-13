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

#include "source_clock.h"

#include "pipeline_stages.h"
#include "sched/constants.h"
#include <micro_resampler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace sendspin::server {

namespace {

constexpr double US_PER_SECOND = 1e6;
constexpr int64_t PPM_UNIT = 1000000;
constexpr int RATIO_BITS = micro_resampler::MicroResampler::ASRC_FRACTIONAL_BITS;
constexpr uint32_t UNITY_RATIO = micro_resampler::MicroResampler::UNITY_RATIO;

}  // namespace

// ==============================================================================
// SourceClock
// ==============================================================================

double SourceClock::observe(int64_t capture_us, double position, uint32_t rate, size_t frames) {
    const double played_us = position * US_PER_SECOND / rate;
    if (!this->offset_us_) {
        this->offset_us_ = static_cast<double>(capture_us) - played_us;
        this->last_us_ = 0.0;
        this->centered_ = false;
        this->step_us_.reset();
        this->clear_window();
        return this->ppm_;
    }
    const double error = static_cast<double>(capture_us) - *this->offset_us_ - played_us;
    if (std::abs(error - this->last_us_) >= SOURCE_CLOCK_STEP_US) {
        // Two observations that move as far from the last one taken and agree are a step: the
        // reference absorbs it, and the window, which would average across it, starts again
        if (this->step_us_ && std::abs(error - *this->step_us_) <= SOURCE_CLOCK_STEP_US / 2.0) {
            *this->offset_us_ += ((error + *this->step_us_) / 2.0) - this->last_us_;
            this->step_us_.reset();
            this->clear_window();
        } else {
            this->step_us_ = error;
        }
        return this->ppm_;
    }
    this->step_us_.reset();
    this->last_us_ = error;
    this->window_sum_ += error * static_cast<double>(frames);
    this->window_frames_ += static_cast<int64_t>(frames);
    if (this->window_frames_ * sched::US_PER_SECOND <
        SOURCE_CLOCK_WINDOW_US * static_cast<int64_t>(rate)) {
        return this->ppm_;
    }
    const double mean = this->window_sum_ / static_cast<double>(this->window_frames_);
    const double seconds = static_cast<double>(this->window_frames_) / rate;
    this->clear_window();
    if (!this->centered_) {
        *this->offset_us_ += mean;
        this->last_us_ -= mean;
        this->centered_ = true;
        return this->ppm_;
    }
    if (!this->engaged_) {
        if (std::abs(mean) < SOURCE_CLOCK_ENGAGE_US) {
            return this->ppm_;
        }
        this->engaged_ = true;
    }
    // A late chunk (positive error) wants more output per input: a negative correction
    this->integral_ += mean * seconds;
    double ppm = -((SOURCE_CLOCK_KP * mean) + (SOURCE_CLOCK_KI * this->integral_));
    if (std::abs(ppm) > SOURCE_CLOCK_MAX_PPM) {
        // The integral holds while the correction is clamped
        this->integral_ -= mean * seconds;
        ppm = std::clamp(ppm, -SOURCE_CLOCK_MAX_PPM, SOURCE_CLOCK_MAX_PPM);
    }
    this->ppm_ = ppm;
    return this->ppm_;
}

void SourceClock::skip(int64_t duration_us) {
    if (this->offset_us_) {
        *this->offset_us_ += static_cast<double>(duration_us);
    }
}

void SourceClock::restart() {
    this->offset_us_.reset();
}

void SourceClock::clear_window() {
    this->window_sum_ = 0.0;
    this->window_frames_ = 0;
}

// ==============================================================================
// SampleSlip
// ==============================================================================

size_t SampleSlip::max_output(size_t frames) {
    // At most one slip per 10^6 / SOURCE_CLOCK_MAX_PPM frames, plus one due at once
    return frames + (frames / (PPM_UNIT / static_cast<int64_t>(SOURCE_CLOCK_MAX_PPM))) + 1;
}

size_t SampleSlip::slip(const uint8_t* in, size_t frames, size_t frame_bytes, int32_t ppm,
                        uint8_t* out) {
    const auto bound = static_cast<int32_t>(SOURCE_CLOCK_MAX_PPM);
    const auto step = static_cast<int64_t>(std::clamp(ppm, -bound, bound));
    if (step == 0) {
        std::memcpy(out, in, frames * frame_bytes);
        return frames;
    }
    size_t written = 0;
    size_t taken = 0;
    while (taken < frames) {
        // The frames up to and including the one whose step takes the phase to +-10^6
        const int64_t distance = std::abs((step > 0 ? PPM_UNIT : -PPM_UNIT) - this->phase);
        const int64_t due = (distance + std::abs(step) - 1) / std::abs(step);
        const size_t plain = std::min(static_cast<size_t>(due - 1), frames - taken);
        std::memcpy(out + (written * frame_bytes), in + (taken * frame_bytes), plain * frame_bytes);
        written += plain;
        taken += plain;
        this->phase += static_cast<int64_t>(plain) * step;
        if (taken == frames) {
            break;
        }
        // The frame that slips: dropped, or written twice
        if (step > 0) {
            this->phase += step - PPM_UNIT;
        } else {
            for (int copy = 0; copy < 2; ++copy) {
                std::memcpy(out + (written * frame_bytes), in + (taken * frame_bytes), frame_bytes);
                ++written;
            }
            this->phase += step + PPM_UNIT;
        }
        ++taken;
    }
    return written;
}

// ==============================================================================
// SourceAsrc
// ==============================================================================

std::unique_ptr<SourceAsrc> SourceAsrc::create(uint32_t rate, uint32_t channels,
                                               ResamplerQuality quality, size_t max_block) {
    std::unique_ptr<micro_resampler::MicroResampler> resampler =
        micro_resampler::MicroResampler::create(
            rate, rate, static_cast<int>(channels),
            Resampler::config(quality, false).set_asrc(true).set_max_block_size(max_block));
    if (!resampler || !resampler->initialize() || !resampler->is_asrc_supported() ||
        resampler->get_max_block_size() < max_block) {
        return nullptr;
    }
    return std::unique_ptr<SourceAsrc>(new SourceAsrc(std::move(resampler), channels, max_block));
}

SourceAsrc::SourceAsrc(std::unique_ptr<micro_resampler::MicroResampler> resampler,
                       uint32_t channels, size_t max_block)
    : resampler_(std::move(resampler)),
      channels_(channels),
      max_block_(max_block),
      ratio_(UNITY_RATIO) {
    this->resampler_->set_ratio_fixed(this->ratio_);
}

SourceAsrc::~SourceAsrc() = default;

size_t SourceAsrc::max_output(size_t frames) {
    // The fastest ratio writes fewer than one output frame per input frame past the filter's
    // hold, which was taken earlier: the input plus a margin for what it held
    return frames + (frames / (PPM_UNIT / static_cast<int64_t>(SOURCE_CLOCK_MAX_PPM))) + 2;
}

std::optional<SourceAsrc::Processed> SourceAsrc::process(const float* in, size_t frames, double ppm,
                                                         float* out, size_t capacity) {
    const double clamped = std::clamp(ppm, -SOURCE_CLOCK_MAX_PPM, SOURCE_CLOCK_MAX_PPM);
    // The ratio is set at once: at a correction's size the step in pitch is inaudible, and a
    // steady ratio within a call keeps held_ exact
    this->ratio_ = static_cast<uint32_t>(
        std::llround(static_cast<double>(UNITY_RATIO) * (1.0 + (clamped / PPM_UNIT))));
    this->resampler_->set_ratio_fixed(this->ratio_);
    if (!this->primed_) {
        if (frames == 0 || !this->resampler_->prime_raw(in, frames)) {
            return std::nullopt;
        }
        this->primed_ = true;
    }
    size_t written = 0;
    size_t taken = 0;
    while (frames > 0 && written < capacity) {
        size_t consumed = 0;
        size_t generated = 0;
        if (!this->resampler_->process_block_raw(in, std::min(frames, this->max_block_),
                                                 out + written * this->channels_,
                                                 capacity - written, consumed, generated)) {
            return std::nullopt;
        }
        // A call that takes nothing would never end
        if (consumed == 0 && generated == 0) {
            return std::nullopt;
        }
        this->held_ += (static_cast<int64_t>(consumed) << RATIO_BITS) -
                       (static_cast<int64_t>(generated) * this->ratio_);
        written += generated;
        taken += consumed;
        frames -= consumed;
        in += consumed * this->channels_;
    }
    this->produced_ += written;
    return Processed{taken, written};
}

void SourceAsrc::reset() {
    this->resampler_->reset();
    this->primed_ = false;
    this->produced_ = 0;
    this->held_ = 0;
}

double SourceAsrc::next_position() const {
    return static_cast<double>(this->produced_) +
           (static_cast<double>(this->held_) / static_cast<double>(this->ratio_));
}

}  // namespace sendspin::server
