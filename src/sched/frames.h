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

/// @file frames.h
/// @brief The frame grid, frames and a pipeline instance's frame ring keyed by cell index, and a
/// session's cursor over one

#pragma once

#include "frame_payload.h"
#include "ring.h"
#include "sched/constants.h"
#include "sched/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief A format's frame on the cell grid
struct FrameLayout {
    /// `m = 50 / gcd(r, 50)`: the fewest cells holding a whole number of samples; 0 for rate 0
    uint32_t cells{0};
    /// `r * m / 50`, per channel
    uint32_t samples{0};
    /// The frame fits one chunk (`m <= 7`) and, for Opus, the rate is an Opus rate
    bool usable{false};
};

/// @brief Every format at one rate shares its frame boundaries, FLAC and PCM alike; an Opus packet
/// is one cell
FrameLayout frame_layout(uint32_t rate_hz, Codec codec);

/// @brief Whether rate_hz is a sample rate an Opus encoder and decoder take (RFC 6716 section 2)
bool is_opus_rate(uint32_t rate_hz);

/// @brief `ceil(samples * 10^6 / rate)`: exact for a full frame, never shorter than exact for a
/// short last frame
int64_t frame_duration_us(uint32_t samples, uint32_t rate_hz);

/// @brief One encoded unit for `m` consecutive cells, immutable once published
struct Frame {
    EpochId epoch;
    /// `k`, the first cell; a multiple of the format's `m`
    int64_t index{0};
    /// `ts(k)`, fixed when the frame is produced
    int64_t ts_us{0};
    int64_t duration_us{0};
    /// Per channel: `r * m / 50`, fewer only in a drained epoch's last frame
    uint32_t samples{0};
    FramePayload payload;
};

/// @brief What packing reads of a frame
struct FrameView {
    int64_t index{0};
    int64_t ts_us{0};
    int64_t duration_us{0};
    int64_t payload_bytes{0};
};

FrameView view_of(const Frame& frame);

/// @brief The frames of `cells` cells over the staging ring's span (the write-ahead and the lead
/// the profile allows), and one more
constexpr size_t ring_frames(uint32_t cells) {
    return static_cast<size_t>(profile::STAGING_RING_US /
                               (int64_t{cells > 0 ? cells : 1} * CELL_US)) +
           1;
}

/// @brief The frames of `cells` cells alive at once, each in a payload slot: ring_frames(), and
/// those up to profile::PAYLOAD_LAG_US past their play time that core has not pruned
constexpr size_t payload_slots(uint32_t cells) {
    const int64_t frame_us = int64_t{cells > 0 ? cells : 1} * CELL_US;
    return ring_frames(cells) +
           static_cast<size_t>((profile::PAYLOAD_LAG_US + frame_us - 1) / frame_us);
}

/// @brief Core's store of one pipeline instance's frames in one epoch, keyed by cell index. The
/// instance produces every `m`-th index from its start index, so the ring has no holes. Frames are
/// dropped once `ts <= now`, which is late for every player since output delays are not negative.
class FrameRing {
public:
    FrameRing(InstanceId instance, uint32_t cells)
        : frames_(payload_slots(cells), Ring<Frame>::UNLIMITED, MemoryLocation::PREFER_EXTERNAL),
          instance_(instance),
          cells_(cells) {}

    /// @brief Drops every frame and expects the epoch's frames from start_index, a multiple of
    /// the frame length
    void restart(EpochId epoch, int64_t start_index);

    /// @brief Takes the instance's next frame, then prunes at now_us. The first frame since
    /// restart() may come at any multiple of the frame length at or above the base, which moves the
    /// base up: the instance may start later than requested.
    /// @return false, dropping it, for a frame of another epoch or one that is not the next index
    bool append(Frame frame, int64_t now_us);

    /// @brief Drops the frames with `ts <= now`
    void prune(int64_t now_us);

    /// @brief A visualizer analysis restarted past cells late for every cursor: prunes at now_us
    /// and, when no frame is left, expects the next frame at index, a later multiple of the frame
    /// length in the ring's epoch; anything else changes nothing
    void skip_to(EpochId epoch, int64_t index, int64_t now_us);

    /// @brief The frame starting at index in epoch, or null: outside the ring, not a multiple of
    /// the frame length, or another epoch
    const Frame* at(EpochId epoch, int64_t index) const;

    /// @brief The first index held; with no frame, the next index expected, so a cursor clamped to
    /// it never passes an unproduced frame
    int64_t base() const {
        return this->base_;
    }

    /// @brief The index the next appended frame must have
    int64_t next_expected() const {
        return this->base_ + static_cast<int64_t>(this->frames_.size()) * this->cells_;
    }

    /// @brief The instance's final index in a drained epoch (`InstanceFinal`); -1 for an instance
    /// with no frames
    void set_final_index(int64_t index) {
        this->final_index_ = index;
    }

    std::optional<int64_t> final_index() const {
        return this->final_index_;
    }

    InstanceId instance() const {
        return this->instance_;
    }

    EpochId epoch() const {
        return this->epoch_;
    }

    uint32_t cells() const {
        return this->cells_;
    }

    size_t size() const {
        return this->frames_.size();
    }

private:
    /// Bounded by the send path's horizon: a frame is dropped once its `ts` passes, and admission
    /// keeps the input within the group lead and the write-ahead ahead; at most payload_slots(),
    /// since each frame holds one of its instance's payload slots
    Ring<Frame> frames_;
    InstanceId instance_;
    EpochId epoch_;
    int64_t base_{0};
    std::optional<int64_t> final_index_;
    uint32_t cells_;
    /// A frame was taken since restart()
    bool started_{false};
};

/// @brief A session's position in one stream role's ring
struct Cursor {
    /// The ring the next item comes from; its format is the sent format
    InstanceId instance;
    /// The ring of the wanted format; differs from instance while a switch is pending
    InstanceId target;
    /// The index at which the cursor moves to target
    int64_t switch_point{0};
    int64_t next{0};
    /// `min_ts`: the startup bound of the last trigger
    int64_t min_ts{0};
    /// Visualizer: the last timestamp sent
    int64_t last_ts{0};
    /// The instance whose frames the cursor waits for, or none
    InstanceId starved_on;
    /// `next` is past its own instance's final index in a drained epoch
    bool final_done{false};
};

/// @brief The player cursor's step before every selection: moves next up to the ring's base
/// (everything below it was late for every player), skips frames late for this player (`ts - od <=
/// now`) and frames before the trigger's `min_ts` (a shift core has not received cannot start it
/// early), and sets final_done from the ring's final index
void prepare_cursor(Cursor& cursor, const FrameRing& ring, EpochId epoch, int64_t output_delay_us,
                    int64_t now_us);

/// @brief Where a cursor at next moves from an instance of from_cells to one of to_cells: a frame
/// boundary of both, never below next or the target ring's base, nor below a switch point already
/// planned; with no target ring yet, `P_prep` past next. Never below a frame already sent, so
/// nothing is resent and the new format follows the old on the same timeline.
/// @param planned the switch point planned for the same target, if any
/// @param target_base the target ring's base, while it has the cursor's epoch
int64_t plan_switch(int64_t next, std::optional<int64_t> planned,
                    std::optional<int64_t> target_base, uint32_t from_cells, uint32_t to_cells);

}  // namespace sendspin::server::sched
