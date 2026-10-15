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

/// @file wire_messages.h
/// @brief The spec's binary message types and fields, as tests decode what the server sends,
/// defined here rather than taken from the server's own constants

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace sendspin::server::test::wire {

/// The binary message types (spec messaging.md "Binary Message ID Structure"): a JSON message
/// sent as binary, a fragment of a larger message, an audio chunk, artwork channel 0 (channel n's
/// is this plus n) and the range the visualization messages take
constexpr uint8_t JSON_ID = 0;
constexpr uint8_t FRAGMENT_ID = 1;
constexpr uint8_t AUDIO_CHUNK_ID = 4;
constexpr uint8_t FIRST_ARTWORK_ID = 8;
constexpr uint8_t FIRST_VISUALIZER_ID = 16;
constexpr uint8_t LAST_VISUALIZER_ID = 23;

/// The visualization messages the server sends, by type (spec roles/visualizer/v1.md
/// "Visualization Data (Binary)")
constexpr uint8_t LOUDNESS_ID = 16;
constexpr uint8_t F_PEAK_ID = 18;
constexpr uint8_t SPECTRUM_ID = 19;
constexpr uint8_t PEAK_ID = 20;

/// Where a message's timestamp starts: after the type byte, and after an artwork message's flags
constexpr size_t TIMESTAMP_AT = 1;
constexpr size_t ARTWORK_TIMESTAMP_AT = 2;
/// Where an audio chunk's send_ahead starts, after its timestamp (spec roles/player/v1.md "Audio
/// Chunks (Binary)")
constexpr size_t SEND_AHEAD_AT = 9;
/// Where an artwork announce's total_size starts, after its timestamp
constexpr size_t ARTWORK_TOTAL_SIZE_AT = 10;

/// An audio chunk's type, timestamp and send_ahead, ahead of its audio
constexpr size_t AUDIO_CHUNK_HEADER = 13;
/// A visualization message's type and timestamp, ahead of its data
constexpr size_t VISUALIZER_HEADER = 9;

/// An artwork message's flags byte, after its type (spec roles/artwork/v1.md "Artwork (Binary)")
constexpr size_t ARTWORK_FLAGS_AT = 1;
constexpr uint8_t ARTWORK_PART = 0x00;
constexpr uint8_t ARTWORK_CANCEL = 0x01;
constexpr uint8_t ARTWORK_ANNOUNCE = 0x02;

constexpr bool is_visualizer(uint8_t id) {
    return id >= FIRST_VISUALIZER_ID && id <= LAST_VISUALIZER_ID;
}

/// The big-endian T starting at bytes[at]
template <typename T>
T field_at(std::span<const uint8_t> bytes, size_t at) {
    std::make_unsigned_t<T> value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value = static_cast<std::make_unsigned_t<T>>((value << 8U) | bytes[at + i]);
    }
    return static_cast<T>(value);
}

/// The timestamp starting at bytes[at]: TIMESTAMP_AT for an audio chunk or a visualization message
inline int64_t timestamp_at(std::span<const uint8_t> bytes, size_t at = TIMESTAMP_AT) {
    return field_at<int64_t>(bytes, at);
}

/// What follows a message's header of header bytes
inline std::span<const uint8_t> payload_after(std::span<const uint8_t> bytes, size_t header) {
    return bytes.subspan(header);
}

}  // namespace sendspin::server::test::wire
