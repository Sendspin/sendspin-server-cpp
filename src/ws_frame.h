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

/// @file ws_frame.h
/// @brief WebSocket framing for a transport that frames itself (the ESP-IDF one): a frame's
/// header written and parsed, and client-to-server masking (RFC 6455 sections 5.2 and 5.3).
/// Platform-free and host-tested.

#pragma once

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

/// @brief A control frame's largest payload (RFC 6455 section 5.5)
constexpr size_t WS_CONTROL_PAYLOAD_MAX = 125;

/// @brief A frame's masking key (RFC 6455 section 5.3)
using WsMaskKey = std::array<uint8_t, 4>;

/// @brief The longest frame header: a 64-bit length and a masking key (RFC 6455 section 5.2)
constexpr size_t WS_FRAME_HEADER_MAX = 14;

/// @brief The header of a final frame of len payload bytes, masked with mask when set
/// @return the header's length
size_t ws_frame_header(uint8_t opcode, size_t len, std::array<uint8_t, WS_FRAME_HEADER_MAX>& header,
                       const std::optional<WsMaskKey>& mask = std::nullopt);

/// @brief Writes len bytes of src to dst, each XORed with the key byte its index selects
/// (RFC 6455 section 5.3); dst may be src, the cheaper call: the XOR runs in place by aligned
/// words after a memcpy()
void ws_mask_copy(uint8_t* dst, const uint8_t* src, size_t len, const WsMaskKey& key);

/// @brief A frame header as received
struct WsFrameHeader {
    bool fin{false};
    uint8_t opcode{0};
    /// The masking key, when the frame is masked
    std::optional<WsMaskKey> mask;
    uint64_t payload_bytes{0};
    /// The header's own length
    size_t header_bytes{0};
};

enum class WsHeaderStatus : uint8_t {
    INCOMPLETE,  ///< bytes ends inside the header
    COMPLETE,
    INVALID,  ///< The frame breaks the rules: the connection fails (RFC 6455 section 7.1.7)
};

struct WsHeaderParse {
    WsHeaderStatus status{WsHeaderStatus::INCOMPLETE};
    WsFrameHeader header;
};

/// @brief Parses the header at the start of bytes, as the endpoint receiving it: a server's peer
/// masks every frame and a client's never does (RFC 6455 section 5.1). A reserved bit (no
/// extension is negotiated), an unknown opcode, a fragmented or longer control frame (section
/// 5.5), a 64-bit length with its top bit set or the wrong masking is INVALID.
WsHeaderParse ws_parse_frame_header(std::span<const uint8_t> bytes, bool peer_masks);

}  // namespace sendspin::server
