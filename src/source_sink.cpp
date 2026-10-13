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

#include <algorithm>
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

LiveSourceSink::LiveSourceSink(std::shared_ptr<HostView> view, int64_t jitter_us)
    : view_(std::move(view)), jitter_us_(jitter_us) {}

bool LiveSourceSink::begin(const InputFormat& format) {
    this->sample_rate_ = format.sample_rate;
    this->timing_.baseline_us.reset();
    this->timing_.stale_us = 0;
    return this->view_->format().format == format || this->view_->set_format(format);
}

void LiveSourceSink::write(const uint8_t* pcm, size_t frames, int64_t capture_us, int64_t rx_us) {
    const int64_t end_us =
        capture_us + static_cast<int64_t>(frames) * 1000000 / int64_t{this->sample_rate_};
    switch (judge_source_chunk(this->timing_, capture_us, end_us, rx_us, this->jitter_us_)) {
        case SourceChunkAction::DROP:
            return;
        case SourceChunkAction::RESTART:
            this->stop_run();
            break;
        case SourceChunkAction::WRITE:
            break;
    }
    // A live input's writer drops what write() refuses (GroupInput::write())
    (void)this->view_->write(pcm, frames);
}

void LiveSourceSink::end() {
    this->stop_run();
    this->timing_ = {};
}

void LiveSourceSink::stop_run() {
    const InputState state = this->view_->state();
    if (state != InputState::STOP_LATCHED && state != InputState::CLOSED) {
        this->view_->stop();
    }
}

}  // namespace sendspin::server
