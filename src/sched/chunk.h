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

/// @file chunk.h
/// @brief Player chunks: the header and `send_ahead`, producibility and format selection, and
/// packing frames into a chunk. Pure functions.

#pragma once

#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sendspin::server::sched {

/// @brief `sat(ts - stamp)`: 0 at or after ts, 0xFFFFFFFF when the lead does not fit the field
/// (spec player/v1.md "Audio Chunks")
uint32_t send_ahead_field(int64_t ts_us, int64_t stamp_us);

/// @brief Writes the 13-byte header: type 4, the big-endian timestamp and send_ahead
void write_chunk_header(std::span<uint8_t, CHUNK_HEADER_BYTES> out, int64_t ts_us,
                        uint32_t send_ahead);

/// @brief What producibility and packing know of one output format
struct FormatShape {
    AudioFormat format;
    FrameLayout layout;
    /// `B_f`: the largest frame's encoded size
    int64_t max_frame_bytes{0};
    /// The server has an encoder for the codec, and the format's parameters are ones it takes
    bool encoder_available{false};
};

/// @brief A PCM format's shape: frames of exact size, little-endian, 24-bit packed (spec
/// player/v1.md "PCM Encoding Convention")
FormatShape pcm_shape(const AudioFormat& format);

/// @brief Whether a format can be streamed to a player with this `buffer_capacity`: its frame fits
/// one chunk (FrameLayout::usable), a FLAC sample is at most 24 bits, and one frame plus the chunk
/// header fits one Noise transport message and the capacity
bool producible(const FormatShape& shape, int64_t capacity);

/// @brief `cap_f = max(max_message_bytes, B_f + 13)`: a chunk's size limit, whole message
/// counted
int64_t message_cap(const FormatShape& shape, size_t max_message_bytes);

/// @brief The format of a player's next stream: its preferred format when producible, otherwise
/// the highest-priority producible entry, one at the native rate first when one is given; nullopt
/// when none is (spec player/v1.md "client/hello player@v1 support object")
/// @param supported the player's `supported_formats`, in priority order
/// @param native_rate the input's rate, which needs no resampling
std::optional<AudioFormat> select_format(std::span<const FormatShape> supported,
                                         const std::optional<AudioFormat>& preferred,
                                         int64_t capacity, std::optional<uint32_t> native_rate);

/// @brief What bounds one chunk
struct PackLimits {
    Codec codec{Codec::PCM};
    /// `m`
    uint32_t cells{1};
    int64_t now_us{0};
    int64_t output_delay_us{0};
    /// `cap_f`
    int64_t message_cap{0};
    /// `capacity - count(now)`
    int64_t room_bytes{0};
    /// The instance's final index in a drained epoch
    std::optional<int64_t> final_index;
    /// The index at which the cursor switches instance; no frame at or after it is packed
    std::optional<int64_t> switch_point;
    uint32_t target_cells{TARGET_CHUNK_CELLS};
    int64_t solo_slack_us{SOLO_SLACK_US};
    /// The player's `S_pack` (pack_hold_slack_us())
    int64_t hold_slack_us{PACK_HOLD_SLACK_US};
};

/// @brief The next chunk as packing finds it now
struct PackPlan {
    size_t frames{0};
    /// It stops short of `n_max` at a frame not produced yet or at the room, so it could carry
    /// more frames later
    bool grows{false};
    /// With a stop at the room: the message's bytes with the next frame, for the ledger's room
    /// time; 0 otherwise
    int64_t bytes_with_next{0};
};

/// @brief How many of frames, consecutive from the cursor's next index, the next chunk carries: at
/// most `max(1, n_target / m)` (one for Opus, one when the first frame's slack is under `S_solo`),
/// within 150 ms, the message cap and the room, and stopping at a missing frame, a timeline shift,
/// the final index or the switch point. 0 frames when the first frame cannot go.
PackPlan plan_chunk(std::span<const FrameView> frames, const PackLimits& limits);

/// @brief plan_chunk()'s frames: take() packs greedily, whatever pack_hold_until() decided
size_t pack_chunk(std::span<const FrameView> frames, const PackLimits& limits);

/// @brief A player's `S_pack = max(S_solo, mb) + n_target * F + M_pump`: a held chunk still has a
/// full chunk's slack past `S_solo` and the player's `min_buffer_ms` when it goes (spec
/// player/v1.md "Server Audio Send Constraints"), with room for a pump that runs late. A live
/// cursor, whose frames come with about `mb + M_pump + J_in` of slack, is never held. A buffered
/// input's player waits to at least `S_buf` (BUFFERED_HOLD_SLACK_US).
/// @param min_buffer_us `mb`, `min_buffer_ms` clamped by `T_clamp`
int64_t pack_hold_slack_us(int64_t min_buffer_us, int64_t pump_margin_us, InputKind kind);

/// @brief Whether a chunk that could grow waits for its frames or room: while its first frame's
/// slack `ts - od - now` exceeds the player's `S_pack`, it does
/// @return the instant it goes anyway, `ts - od - S_pack`; NEVER when it goes now
int64_t pack_hold_until(const PackPlan& plan, int64_t first_ts_us, const PackLimits& limits);

}  // namespace sendspin::server::sched
