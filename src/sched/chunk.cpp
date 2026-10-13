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

#include "sched/chunk.h"

#include "fragmentation.h"

#include <algorithm>
#include <limits>

namespace sendspin::server::sched {

namespace {

constexpr int64_t MAX_SEND_AHEAD = std::numeric_limits<uint32_t>::max();
constexpr uint8_t BITS_PER_BYTE = 8;
constexpr uint8_t MAX_PCM_BIT_DEPTH = 32;

void put_be(uint8_t* out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<uint8_t>(value >> (BITS_PER_BYTE * (bytes - 1 - i)));
    }
}

}  // namespace

uint32_t send_ahead_field(int64_t ts_us, int64_t stamp_us) {
    return static_cast<uint32_t>(std::clamp<int64_t>(ts_us - stamp_us, 0, MAX_SEND_AHEAD));
}

void write_chunk_header(std::span<uint8_t, CHUNK_HEADER_BYTES> out, int64_t ts_us,
                        uint32_t send_ahead) {
    out[0] = MESSAGE_ID_AUDIO_CHUNK;
    put_be(&out[1], static_cast<uint64_t>(ts_us), sizeof(int64_t));
    put_be(&out[1 + sizeof(int64_t)], send_ahead, sizeof(uint32_t));
}

FormatShape pcm_shape(const AudioFormat& format) {
    FormatShape shape;
    shape.format = format;
    shape.layout = frame_layout(format.sample_rate, Codec::PCM);
    const bool valid = format.codec == Codec::PCM && format.channels > 0 && format.bit_depth > 0 &&
                       format.bit_depth <= MAX_PCM_BIT_DEPTH;
    const int64_t sample_bytes = (int64_t{format.bit_depth} + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    shape.max_frame_bytes = int64_t{shape.layout.samples} * format.channels * sample_bytes;
    shape.encoder_available = valid;
    return shape;
}

bool producible(const FormatShape& shape, int64_t capacity) {
    if (!shape.encoder_available || !shape.layout.usable) {
        return false;
    }
    if (shape.format.codec == Codec::FLAC && shape.format.bit_depth > MAX_FLAC_BIT_DEPTH) {
        return false;
    }
    const int64_t message = shape.max_frame_bytes + CHUNK_HEADER_BYTES;
    return message <= static_cast<int64_t>(MAX_TRANSPORT_PLAINTEXT) && message <= capacity;
}

int64_t message_cap(const FormatShape& shape, size_t max_message_bytes) {
    return std::max(static_cast<int64_t>(max_message_bytes),
                    shape.max_frame_bytes + CHUNK_HEADER_BYTES);
}

std::optional<AudioFormat> select_format(std::span<const FormatShape> supported,
                                         const std::optional<AudioFormat>& preferred,
                                         int64_t capacity, std::optional<uint32_t> native_rate) {
    const auto first = [&](const auto& wanted) -> std::optional<AudioFormat> {
        for (const FormatShape& shape : supported) {
            if (wanted(shape.format) && producible(shape, capacity)) {
                return shape.format;
            }
        }
        return std::nullopt;
    };
    if (preferred.has_value()) {
        if (std::optional<AudioFormat> format =
                first([&](const AudioFormat& f) { return f == *preferred; })) {
            return format;
        }
    }
    // spec player/v1.md "client/hello player@v1 support object": a server MAY select another
    // entry to match the track's native sample rate and avoid resampling
    if (native_rate.has_value()) {
        if (std::optional<AudioFormat> format =
                first([&](const AudioFormat& f) { return f.sample_rate == *native_rate; })) {
            return format;
        }
    }
    return first([](const AudioFormat&) { return true; });
}

PackPlan plan_chunk(std::span<const FrameView> frames, const PackLimits& limits) {
    PackPlan plan;
    if (frames.empty()) {
        return plan;
    }
    size_t n_max =
        limits.codec == Codec::OPUS ? 1 : std::max<size_t>(1, limits.target_cells / limits.cells);
    // A player drops a chunk whose start is late, so a nearly late frame goes alone
    if (frames[0].ts_us - limits.output_delay_us - limits.now_us < limits.solo_slack_us) {
        n_max = 1;
    }
    const int64_t first = frames[0].index;
    size_t& n = plan.frames;
    int64_t bytes = CHUNK_HEADER_BYTES;
    int64_t duration = 0;
    // Ran out of frames before n_max: the next is not produced yet
    plan.grows = true;
    for (const FrameView& frame : frames) {
        if (n == n_max) {
            plan.grows = false;
            break;
        }
        const int64_t index = first + static_cast<int64_t>(n) * limits.cells;
        if (frame.index != index) {
            break;
        }
        // A chunk plays its frames back to back from its timestamp, so a timeline shift between two
        // frames ends it
        if ((n > 0 && frame.ts_us != frames[0].ts_us + duration) ||
            (limits.final_index.has_value() && index > *limits.final_index) ||
            (limits.switch_point.has_value() && index == *limits.switch_point) ||
            duration + frame.duration_us > MAX_CHUNK_US ||
            bytes + frame.payload_bytes > limits.message_cap) {
            plan.grows = false;
            break;
        }
        if (bytes + frame.payload_bytes > limits.room_bytes) {
            plan.bytes_with_next = bytes + frame.payload_bytes;
            break;
        }
        bytes += frame.payload_bytes;
        duration += frame.duration_us;
        ++n;
    }
    // A chunk that reached n_max, the final index or the switch point is whole
    const int64_t next = first + static_cast<int64_t>(n) * limits.cells;
    if (n == n_max || (limits.final_index.has_value() && next > *limits.final_index) ||
        (limits.switch_point.has_value() && next == *limits.switch_point)) {
        plan.grows = false;
    }
    return plan;
}

size_t pack_chunk(std::span<const FrameView> frames, const PackLimits& limits) {
    return plan_chunk(frames, limits).frames;
}

int64_t pack_hold_slack_us(int64_t min_buffer_us, int64_t pump_margin_us, InputKind kind) {
    const int64_t player =
        std::max(SOLO_SLACK_US, min_buffer_us) + (TARGET_CHUNK_CELLS * CELL_US) + pump_margin_us;
    return kind == InputKind::LIVE ? player : std::max(player, BUFFERED_HOLD_SLACK_US);
}

int64_t pack_hold_until(const PackPlan& plan, int64_t first_ts_us, const PackLimits& limits) {
    if (plan.frames == 0 || !plan.grows) {
        return NEVER;
    }
    const int64_t until = first_ts_us - limits.output_delay_us - limits.hold_slack_us;
    return until > limits.now_us ? until : NEVER;
}

}  // namespace sendspin::server::sched
