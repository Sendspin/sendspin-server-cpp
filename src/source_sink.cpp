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

#include "source_sink.h"

#include "pcm_convert.h"
#include "profile.h"
#include "sched/timeline.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sendspin::server {

SourceChunkAction judge_source_chunk(SourceTiming& timing, int64_t capture_us, int64_t end_us,
                                     int64_t rx_us, int64_t jitter_us) {
    const int64_t lateness = rx_us - end_us;
    if (!timing.baseline_us || lateness < *timing.baseline_us) {
        timing.baseline_us = lateness;
    }
    if (lateness > *timing.baseline_us + jitter_us) {
        timing.stale_us += end_us - capture_us;
        if (timing.stale_us <= SOURCE_RESTART_GAP_US) {
            return SourceChunkAction::DROP;
        }
        timing.baseline_us = lateness;
    }
    timing.stale_us = 0;
    const bool gap =
        timing.written_end_us && capture_us - *timing.written_end_us >= SOURCE_RESTART_GAP_US;
    timing.written_end_us = end_us;
    return gap ? SourceChunkAction::RESTART : SourceChunkAction::WRITE;
}

LiveSourceSink::LiveSourceSink(std::shared_ptr<HostView> view, int64_t jitter_us,
                               ResamplerQuality quality)
    : view_(std::move(view)), jitter_us_(jitter_us), quality_(quality) {}

bool LiveSourceSink::begin(const InputFormat& format) {
    this->format_ = format;
    this->timing_.baseline_us.reset();
    this->timing_.stale_us = 0;
    this->clock_ = {};
    this->slip_ = {};
    this->asrc_.reset();
    this->input_.reset();
    this->output_.reset();
    const size_t max_frames = static_cast<size_t>(
        sched::ceil_div(int64_t{format.sample_rate} * MAX_SOURCE_CHUNK_US, sched::US_PER_SECOND));
    InputFormat staged = format;
    size_t input_bytes = 0;
    size_t output_bytes = 0;
    if (profile::SOURCE_ASRC) {
        staged.sample_format = SampleFormat::F32;
        input_bytes = max_frames * format.channels * sizeof(float);
        output_bytes = SourceAsrc::max_output(max_frames) * format.channels * sizeof(float);
        this->asrc_ =
            SourceAsrc::create(format.sample_rate, format.channels, this->quality_, max_frames);
        if (!this->asrc_) {
            return false;
        }
    } else {
        output_bytes = SampleSlip::max_output(max_frames) * format.channels *
                       sample_bytes(format.sample_format);
    }
    // A size the heap cannot give refuses the stream rather than ending the process
    for (auto [buffer, bytes] :
         {std::pair{&this->input_, input_bytes}, std::pair{&this->output_, output_bytes}}) {
        if (bytes > 0) {
            buffer->reset(static_cast<uint8_t*>(
                placed_malloc(bytes, new_alignment<float>(), MemoryLocation::PREFER_EXTERNAL)));
            if (!*buffer) {
                return false;
            }
        }
    }
    this->restart_correction();
    return this->view_->format().format == staged || this->view_->set_format(staged);
}

void LiveSourceSink::write(const uint8_t* pcm, size_t frames, int64_t capture_us, int64_t rx_us) {
    const int64_t end_us =
        capture_us + static_cast<int64_t>(frames) * 1000000 / int64_t{this->format_.sample_rate};
    switch (judge_source_chunk(this->timing_, capture_us, end_us, rx_us, this->jitter_us_)) {
        case SourceChunkAction::DROP:
            this->clock_.skip(end_us - capture_us);
            return;
        case SourceChunkAction::RESTART:
            this->stop_run();
            this->restart_correction();
            break;
        case SourceChunkAction::WRITE:
            break;
    }
    this->correct(pcm, frames, capture_us);
}

void LiveSourceSink::end() {
    this->stop_run();
    this->timing_ = {};
    this->restart_correction();
}

void LiveSourceSink::stop_run() {
    const InputState state = this->view_->state();
    if (state != InputState::STOP_LATCHED && state != InputState::CLOSED) {
        this->view_->stop();
    }
}

void LiveSourceSink::restart_correction() {
    this->clock_.restart();
    this->written_ = 0;
    if (this->asrc_) {
        this->asrc_->reset();
    }
}

void LiveSourceSink::correct(const uint8_t* pcm, size_t frames, int64_t capture_us) {
    const uint32_t rate = this->format_.sample_rate;
    const uint32_t channels = this->format_.channels;
    // A live input's writer drops what write() refuses (GroupInput::write())
    if (!this->asrc_) {
        const double ppm =
            this->clock_.observe(capture_us, static_cast<double>(this->written_), rate, frames);
        const uint8_t* out = pcm;
        size_t count = frames;
        if (this->clock_.engaged()) {
            count =
                this->slip_.slip(pcm, frames, channels * sample_bytes(this->format_.sample_format),
                                 static_cast<int32_t>(std::lround(ppm)), this->output_.get());
            out = this->output_.get();
        }
        (void)this->view_->write(out, count);
        this->written_ += count;
        return;
    }
    auto* floats = reinterpret_cast<float*>(this->input_.get());
    const size_t samples = frames * channels;
    to_float({pcm, samples * sample_bytes(this->format_.sample_format)},
             this->format_.sample_format, {floats, samples});
    // Before the correction starts the ASRC runs at unity and its output, which lines up with its
    // input, is left unwritten; its position is then the unaltered frames written
    const double ppm = this->clock_.observe(capture_us, this->asrc_->next_position(), rate, frames);
    const bool engaged = this->clock_.engaged();
    auto* corrected = reinterpret_cast<float*>(this->output_.get());
    const uint64_t first = this->asrc_->produced();
    const std::optional<SourceAsrc::Processed> processed = this->asrc_->process(
        floats, frames, engaged ? ppm : 0.0, corrected, SourceAsrc::max_output(frames));
    if (!engaged || !processed) {
        (void)this->view_->write(floats, frames);
        this->written_ += frames;
        return;
    }
    // From the position the unaltered frames reached on, which the ASRC's output overlaps at the
    // correction's start
    const uint64_t end = first + processed->written;
    if (end > this->written_) {
        const uint64_t from = std::max(first, this->written_);
        (void)this->view_->write(corrected + ((from - first) * channels),
                                 static_cast<size_t>(end - from));
        this->written_ = end;
    }
}

}  // namespace sendspin::server
