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

/// @file ws_batch.h
/// @brief WsBatch: the messages of one write (Connection's write invariant) as unmasked WebSocket
/// frames back to back, ready for one send, and read back frame by frame. Platform-free; the
/// ESP-IDF transport (src/esp/) writes from it.

#pragma once

#include "memory.h"
#include "transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sendspin::server {

/// @brief WebSocket opcodes (RFC 6455 section 5.2)
namespace ws_opcode {
constexpr uint8_t CONTINUATION = 0x0;
constexpr uint8_t TEXT = 0x1;
constexpr uint8_t BINARY = 0x2;
constexpr uint8_t CLOSE = 0x8;
constexpr uint8_t PING = 0x9;
constexpr uint8_t PONG = 0xA;
}  // namespace ws_opcode

/// @brief An unmasked frame header: at most a 64-bit length (RFC 6455 section 5.2)
constexpr size_t WS_FRAME_HEADER_MAX = 10;

/// @brief The header of a final, unmasked frame of len payload bytes
/// @return the header's length
size_t ws_frame_header(uint8_t opcode, size_t len,
                       std::array<uint8_t, WS_FRAME_HEADER_MAX>& header);

/// @brief One write's frames, in PSRAM where there is one. Written by core while no write is in
/// flight, read by the writer while one is.
class WsBatch {
public:
    struct Frame {
        MessageKind kind;
        std::span<const uint8_t> payload;
    };

    void clear() {
        this->bytes_.clear();
        this->frames_ = 0;
    }

    /// @brief Appends payload as one final text or binary frame
    void append(MessageKind kind, std::span<const uint8_t> payload);

    /// @brief Every frame, headers included, as one send writes them
    std::span<const uint8_t> bytes() const {
        return this->bytes_;
    }

    size_t frames() const {
        return this->frames_;
    }

    /// @brief The frame at offset, a frame's start or bytes().size(), and offset moved past it;
    /// nullopt at the end
    std::optional<Frame> next(size_t& offset) const;

private:
    ExternalBytes bytes_;
    size_t frames_{0};
};

}  // namespace sendspin::server
