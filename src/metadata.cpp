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

#include "metadata.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace sendspin::server {

namespace {

constexpr int64_t US_PER_MS = 1000;
constexpr int64_t SPEED_UNIT = NORMAL_PLAYBACK_SPEED;

}  // namespace

MetadataState metadata_at(const MetadataState& state, int64_t at_us) {
    MetadataState moved = state;
    moved.timestamp_us = at_us;
    if (state.metadata.progress && at_us > state.timestamp_us) {
        MetadataProgress& progress = *moved.metadata.progress;
        // In floating point, so no elapsed time or speed overflows; clamped to the field's range
        const double advanced_ms = static_cast<double>(at_us - state.timestamp_us) *
                                   progress.playback_speed / (SPEED_UNIT * US_PER_MS);
        double position = progress.track_progress_ms + advanced_ms;
        if (progress.track_duration_ms != 0) {
            position = std::min<double>(position, progress.track_duration_ms);
        }
        progress.track_progress_ms = static_cast<uint32_t>(std::min<double>(position, UINT32_MAX));
    }
    return moved;
}

MetadataState MetadataTimeline::current(int64_t now_us) const {
    return this->updates_.current(now_us);
}

std::optional<MetadataState> MetadataTimeline::sent_pending() const {
    return this->updates_.sent_pending();
}

MetadataSends MetadataTimeline::set(const Metadata& metadata, int64_t at_us, int64_t now_us) {
    MetadataState state{at_us, metadata};
    if (state.metadata.progress) {
        if (state.metadata.progress->playback_speed != 0) {
            this->speed_ = state.metadata.progress->playback_speed;
        }
        // metadata/v1.md: while paused a seek or a new track is reported at speed 0
        if (this->paused_) {
            state.metadata.progress->playback_speed = 0;
        }
    }
    return {this->updates_.set(std::move(state), now_us)};
}

MetadataSends MetadataTimeline::cancel(int64_t now_us) {
    return {this->updates_.cancel(now_us)};
}

MetadataSends MetadataTimeline::stop(int64_t now_us, bool paused) {
    MetadataSends sends{this->updates_.advance(now_us)};
    this->paused_ = paused;
    MetadataState state = metadata_at(this->updates_.current(now_us), now_us);
    if (state.metadata.progress) {
        state.metadata.progress->playback_speed = 0;
    }
    // The scheduled update is dropped with no cancel: the state sent here replaces it
    this->updates_.replace(state);
    sends.states.push_back(std::move(state));
    return sends;
}

MetadataSends MetadataTimeline::resume(int64_t start_us, int64_t now_us) {
    MetadataSends sends{this->updates_.advance(now_us)};
    if (!this->paused_) {
        return sends;
    }
    this->paused_ = false;
    const std::optional<MetadataState>& current = this->updates_.current_set();
    if (!this->speed_ || !current || !current->metadata.progress) {
        return sends;
    }
    Metadata metadata = current->metadata;
    metadata.progress->playback_speed = *this->speed_;
    MetadataSends resumed = this->set(metadata, std::max(start_us, now_us), now_us);
    sends.states.insert(sends.states.end(), resumed.states.begin(), resumed.states.end());
    return sends;
}

MetadataSends MetadataTimeline::advance(int64_t now_us) {
    return {this->updates_.advance(now_us)};
}

std::optional<int64_t> MetadataTimeline::next_due_us() const {
    return this->updates_.next_due_us();
}

}  // namespace sendspin::server
