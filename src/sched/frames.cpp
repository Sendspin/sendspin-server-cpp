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

#include "sched/frames.h"

#include "sched/timeline.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <utility>

namespace sendspin::server::sched {

namespace {

constexpr std::array<uint32_t, 5> OPUS_RATES = {8000, 12000, 16000, 24000, 48000};

}  // namespace

FrameLayout frame_layout(uint32_t rate_hz, Codec codec) {
    if (rate_hz == 0) {
        return {};
    }
    if (codec == Codec::OPUS) {
        return {1, rate_hz / CELLS_PER_SECOND, is_opus_rate(rate_hz)};
    }
    const uint32_t common = std::gcd(rate_hz, CELLS_PER_SECOND);
    const uint32_t cells = CELLS_PER_SECOND / common;
    return {cells, rate_hz / common, cells <= MAX_FRAME_CELLS};
}

bool is_opus_rate(uint32_t rate_hz) {
    return std::find(OPUS_RATES.begin(), OPUS_RATES.end(), rate_hz) != OPUS_RATES.end();
}

int64_t frame_duration_us(uint32_t samples, uint32_t rate_hz) {
    return ceil_div(int64_t{samples} * US_PER_SECOND, rate_hz);
}

FrameView view_of(const Frame& frame) {
    return {frame.index, frame.ts_us, frame.duration_us,
            static_cast<int64_t>(frame.payload.size())};
}

void FrameRing::restart(EpochId epoch, int64_t start_index) {
    this->frames_.clear();
    this->epoch_ = epoch;
    this->base_ = start_index;
    this->final_index_.reset();
    this->started_ = false;
}

bool FrameRing::append(Frame frame, int64_t now_us) {
    if (frame.epoch != this->epoch_) {
        return false;
    }
    // The instance may start later than requested
    if (!this->started_ && frame.index > this->base_ && frame.index % this->cells_ == 0) {
        this->base_ = frame.index;
    }
    if (frame.index != this->next_expected()) {
        return false;
    }
    this->started_ = true;
    this->frames_.push(std::move(frame));
    this->prune(now_us);
    return true;
}

void FrameRing::prune(int64_t now_us) {
    while (!this->frames_.empty() && this->frames_.front().ts_us <= now_us) {
        this->frames_.pop_front();
        this->base_ += this->cells_;
    }
}

void FrameRing::skip_to(EpochId epoch, int64_t index, int64_t now_us) {
    if (epoch != this->epoch_) {
        return;
    }
    this->prune(now_us);
    if (this->frames_.empty() && index > this->base_ && index % this->cells_ == 0) {
        this->base_ = index;
    }
}

const Frame* FrameRing::at(EpochId epoch, int64_t index) const {
    if (epoch != this->epoch_ || index < this->base_ || index >= this->next_expected()) {
        return nullptr;
    }
    // The bounded ring span fits size_t, avoiding software int64_t division on 32-bit targets
    const auto offset = static_cast<size_t>(index - this->base_);
    if (offset % this->cells_ != 0) {
        return nullptr;
    }
    return &this->frames_[offset / this->cells_];
}

void prepare_cursor(Cursor& cursor, const FrameRing& ring, EpochId epoch, int64_t output_delay_us,
                    int64_t now_us) {
    cursor.next = std::max(cursor.next, ring.base());
    for (const Frame* frame = ring.at(epoch, cursor.next);
         frame != nullptr &&
         (frame->ts_us - output_delay_us <= now_us || frame->ts_us < cursor.min_ts);
         frame = ring.at(epoch, cursor.next)) {
        cursor.next += ring.cells();
    }
    const std::optional<int64_t> final_index = ring.final_index();
    cursor.final_done = final_index.has_value() && cursor.next > *final_index;
}

int64_t plan_switch(int64_t next, std::optional<int64_t> planned,
                    std::optional<int64_t> target_base, uint32_t from_cells, uint32_t to_cells) {
    int64_t point = next;
    if (planned.has_value()) {
        point = std::max(point, *planned);
    } else if (!target_base.has_value()) {
        point += SWITCH_PREP_CELLS;
    }
    if (target_base.has_value()) {
        point = std::max(point, *target_base);
    }
    return round_up(point, std::lcm(int64_t{from_cells}, int64_t{to_cells}));
}

}  // namespace sendspin::server::sched
