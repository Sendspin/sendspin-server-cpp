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

/// @file live_slew.h
/// @brief A live input played fast while its latency comes down (sched::live_slew_ppm()): through
/// micro-resampler's ASRC, or by dropping single frames spread out (SampleSlip). The host view
/// runs it on its writes in their own format.

#pragma once

#include "memory.h"
#include "sendspin/server/config.h"
#include "sendspin/server/types.h"
#include "source_clock.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sendspin::server {

/// @brief The most frames LiveSlew::process() takes at once, and the history the ASRC starts from:
/// more than its filter spans at any quality
constexpr size_t LIVE_SLEW_BLOCK_FRAMES = 512;

/// @brief One live input's slew. The ASRC, once started, runs on at unity once nothing is owed, so
/// its output stays continuous until restart(); the few frames its filter holds when its epoch or
/// segment ends are not played. Dropped frames need no filter and stop with the slew.
class LiveSlew {
public:
    /// @brief Readies the slew for format, ending any in progress: with asrc, micro-resampler's
    /// ASRC at quality, or frame drops where it cannot be made. Allocates all process() uses.
    /// @return false when an allocation fails; the slew then takes nothing out
    bool prepare(const InputFormat& format, bool asrc, ResamplerQuality quality);

    /// @brief prepare() succeeded
    bool ready() const {
        return this->bytes_ != nullptr;
    }

    /// @brief Ends the slew in progress: the next starts again from frames written as they came
    void restart();

    /// @brief The ASRC runs: every frame goes through it, at unity once nothing is owed
    bool engaged() const {
        return this->engaged_;
    }

    /// @brief The ASRC waits for start() before it slews
    bool awaits_start() const {
        return this->asrc_ != nullptr && !this->engaged_;
    }

    /// @brief Room for LIVE_SLEW_BLOCK_FRAMES frames of history in the format, for start()
    std::span<uint8_t> history() const;

    /// @brief Starts the ASRC after the frames written as they came, the last frames of which
    /// (up to LIVE_SLEW_BLOCK_FRAMES) are staged in history(): it takes them first, so its output
    /// continues them
    void start(size_t frames);

    struct Block {
        size_t consumed{0};
        size_t written{0};
    };

    /// @brief Plays up to LIVE_SLEW_BLOCK_FRAMES frames of in ppm fast into output(), writing at
    /// most capacity frames; a positive ppm at most LOWERING_SLEW_PPM. The ASRC keeps back what
    /// it wrote past the capacity for the next call.
    Block process(const uint8_t* in, size_t frames, int32_t ppm, size_t capacity);

    /// @brief What the last process() wrote, in the format
    const uint8_t* output() const {
        return this->bytes_.get();
    }

    /// @brief The time the slew took out of the timeline since the last call, in whole
    /// microseconds; never negative, so a fraction the ASRC gives back is not counted twice
    int64_t take_slewed_us();

private:
    /// The ASRC refused: the slew drops frames from now on
    void fall_back();

    InputFormat format_;
    size_t frame_bytes_{0};
    std::unique_ptr<SourceAsrc> asrc_;
    SampleSlip slip_;
    bool engaged_{false};
    /// Staging in PSRAM where there is one: the format's bytes (history, then output), and with
    /// the ASRC its float input and output
    PlacedArray<uint8_t> bytes_;
    PlacedArray<float> floats_in_;
    PlacedArray<float> floats_out_;
    /// The ASRC's input since start(), history included, and its output start() left to discard
    uint64_t taken_{0};
    size_t skip_{0};
    /// The frames bytes_ holds: the ASRC's output a call's capacity kept back, after those the
    /// call wrote
    size_t output_frames_{0};
    size_t pending_{0};
    size_t emitted_{0};
    /// The input frames since restart() less the positions they landed at
    double slewed_frames_{0.0};
    int64_t reported_us_{0};
};

}  // namespace sendspin::server
