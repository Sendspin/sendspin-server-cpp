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

#include "live_slew.h"

#include "pcm_convert.h"
#include "sched/constants.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sendspin::server {

static_assert(sched::LOWERING_SLEW_PPM <= static_cast<int64_t>(SOURCE_CLOCK_MAX_PPM),
              "SampleSlip and SourceAsrc clamp a faster slew");

void LiveSlew::prepare(const InputFormat& format, bool asrc, ResamplerQuality quality) {
    this->format_ = format;
    this->frame_bytes_ = sample_bytes(format.sample_format) * format.channels;
    this->asrc_.reset();
    this->restart();
    if (asrc) {
        this->asrc_ = SourceAsrc::create(format.sample_rate, format.channels, quality,
                                         LIVE_SLEW_BLOCK_FRAMES);
    }
    const size_t output = SourceAsrc::max_output(LIVE_SLEW_BLOCK_FRAMES);
    if (this->asrc_) {
        this->floats_in_ = make_placed_array<float>(LIVE_SLEW_BLOCK_FRAMES * format.channels,
                                                    MemoryLocation::PREFER_EXTERNAL);
        this->floats_out_ =
            make_placed_array<float>(output * format.channels, MemoryLocation::PREFER_EXTERNAL);
    } else {
        this->floats_in_.reset();
        this->floats_out_.reset();
    }
    // Dropped frames write no more than they take; the ASRC's output keeps back what a call's
    // capacity does not take
    this->output_frames_ = this->asrc_ ? 2 * output : LIVE_SLEW_BLOCK_FRAMES;
    this->bytes_ = make_placed_array<uint8_t>(this->output_frames_ * this->frame_bytes_,
                                              MemoryLocation::PREFER_EXTERNAL);
}

void LiveSlew::restart() {
    this->slip_ = {};
    this->engaged_ = false;
    this->taken_ = 0;
    this->skip_ = 0;
    this->pending_ = 0;
    this->emitted_ = 0;
    this->slewed_frames_ = 0.0;
    this->reported_us_ = 0;
}

std::span<uint8_t> LiveSlew::history() const {
    return {this->bytes_.get(), LIVE_SLEW_BLOCK_FRAMES * this->frame_bytes_};
}

void LiveSlew::start(size_t frames) {
    this->asrc_->reset();
    this->engaged_ = true;
    this->taken_ = 0;
    this->skip_ = 0;
    if (frames == 0) {
        // Primed by the first block it slews instead
        return;
    }
    const size_t samples = frames * this->format_.channels;
    to_float({this->bytes_.get(), frames * this->frame_bytes_}, this->format_.sample_format,
             {this->floats_in_.get(), samples});
    // At unity its output lines up with its input, the history already written: it is discarded
    const std::optional<SourceAsrc::Processed> processed =
        this->asrc_->process(this->floats_in_.get(), frames, 0.0, this->floats_out_.get(),
                             SourceAsrc::max_output(LIVE_SLEW_BLOCK_FRAMES));
    if (!processed) {
        this->fall_back();
        return;
    }
    this->taken_ = processed->consumed;
    this->skip_ = frames > processed->written ? frames - processed->written : 0;
}

LiveSlew::Block LiveSlew::process(const uint8_t* in, size_t frames, int32_t ppm, size_t capacity) {
    frames = std::min(frames, LIVE_SLEW_BLOCK_FRAMES);
    if (!this->engaged_) {
        // Dropped frames: never more out than in
        const size_t taken = std::min(frames, capacity);
        const size_t written = this->slip_.slip(in, taken, this->frame_bytes_,
                                                std::max<int32_t>(ppm, 0), this->bytes_.get());
        this->slewed_frames_ += static_cast<double>(taken - written);
        return {taken, written};
    }
    // What the last call kept back moves to the front
    std::memmove(this->bytes_.get(), this->bytes_.get() + (this->emitted_ * this->frame_bytes_),
                 this->pending_ * this->frame_bytes_);
    this->emitted_ = 0;
    const uint32_t channels = this->format_.channels;
    to_float({in, frames * this->frame_bytes_}, this->format_.sample_format,
             {this->floats_in_.get(), frames * channels});
    const double before = static_cast<double>(this->taken_) - this->asrc_->next_position();
    const size_t room = std::min(SourceAsrc::max_output(LIVE_SLEW_BLOCK_FRAMES),
                                 this->output_frames_ - this->pending_ + this->skip_);
    const std::optional<SourceAsrc::Processed> processed = this->asrc_->process(
        this->floats_in_.get(), frames, std::max<int32_t>(ppm, 0), this->floats_out_.get(), room);
    if (!processed) {
        // This block as it came, what was kept back dropped; a step of what the filter held, once
        this->fall_back();
        const size_t taken = std::min(frames, capacity);
        std::memcpy(this->bytes_.get(), in, taken * this->frame_bytes_);
        return {taken, taken};
    }
    this->taken_ += processed->consumed;
    this->slewed_frames_ +=
        static_cast<double>(this->taken_) - this->asrc_->next_position() - before;
    const size_t discarded = std::min(this->skip_, processed->written);
    this->skip_ -= discarded;
    const size_t fresh = processed->written - discarded;
    const std::span<const uint8_t> floats{
        reinterpret_cast<const uint8_t*>(this->floats_out_.get() + (discarded * channels)),
        fresh * channels * sizeof(float)};
    uint8_t* to = this->bytes_.get() + (this->pending_ * this->frame_bytes_);
    if (this->format_.sample_format == SampleFormat::F32) {
        std::memcpy(to, floats.data(), floats.size());
    } else {
        convert_pcm(floats, SampleFormat::F32, 8 * sample_bytes(this->format_.sample_format),
                    {to, fresh * this->frame_bytes_});
    }
    // A call may write a frame more than it took as the filter's phase moves; the frame waits for
    // the next, so no call passes its capacity
    this->pending_ += fresh;
    this->emitted_ = std::min(this->pending_, capacity);
    this->pending_ -= this->emitted_;
    return {processed->consumed, this->emitted_};
}

int64_t LiveSlew::take_slewed_us() {
    const auto total = static_cast<int64_t>(
        std::floor(this->slewed_frames_ * 1e6 / static_cast<double>(this->format_.sample_rate)));
    if (total <= this->reported_us_) {
        return 0;
    }
    const int64_t slewed = total - this->reported_us_;
    this->reported_us_ = total;
    return slewed;
}

void LiveSlew::fall_back() {
    this->asrc_.reset();
    this->engaged_ = false;
}

}  // namespace sendspin::server
