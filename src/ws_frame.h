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
/// header written and parsed, client-to-server masking and a close frame's payload checked (RFC
/// 6455 sections 5.2, 5.3 and 5.5.1).
/// Platform-free and host-tested.

#pragma once

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

/// @brief A control frame's largest payload (RFC 6455 section 5.5)
constexpr size_t WS_CONTROL_PAYLOAD_MAX = 125;

/// @brief Whether a received close frame's payload is well formed (RFC 6455 section 5.5.1): empty,
/// or a status code an endpoint may send (section 7.4) followed by a UTF-8 reason
bool ws_close_payload_valid(std::span<const uint8_t> payload);

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

/// @brief A ChaCha20 key's length (RFC 8439 section 2.3)
constexpr size_t CHACHA20_KEY_BYTES = 32;
/// @brief A ChaCha20 block's length (RFC 8439 section 2.3)
constexpr size_t CHACHA20_BLOCK_BYTES = 64;

/// @brief The ChaCha20 block function (RFC 8439 section 2.3): the keystream block for key, the
/// block counter and the nonce's three words, little-endian as the RFC reads its bytes
std::array<uint8_t, CHACHA20_BLOCK_BYTES> chacha20_block(
    const std::array<uint8_t, CHACHA20_KEY_BYTES>& key, uint32_t counter,
    const std::array<uint32_t, 3>& nonce);

/// @brief Masking keys read off a ChaCha20 keystream under a key drawn once from the CSPRNG, so
/// each is unpredictable (RFC 6455 section 5.3) at one block per sixteen keys, with no RNG call
/// per frame. One generator per thread or per lock; generators sharing a seed take different
/// streams.
class WsMaskKeys {
public:
    /// @param stream the nonce's first word: generators sharing seed differ in it
    void seed(const std::array<uint8_t, CHACHA20_KEY_BYTES>& seed, uint32_t stream);

    /// @brief The next four keystream bytes; seed() comes first
    WsMaskKey next();

private:
    std::array<uint8_t, CHACHA20_KEY_BYTES> key_{};
    std::array<uint32_t, 3> nonce_{};
    uint32_t counter_{0};
    std::array<uint8_t, CHACHA20_BLOCK_BYTES> block_{};
    /// The bytes of block_ taken; a full block refills at the next key
    size_t taken_{CHACHA20_BLOCK_BYTES};
};

/// @brief What a data frame does to the message being assembled
enum class WsFrameStep : uint8_t {
    OPENS,      ///< A text or binary frame opens a message
    CONTINUES,  ///< A continuation frame adds to the open message
    /// A continuation with no message open, a text or binary frame inside one (RFC 6455 section
    /// 5.4), or a message that would pass its cap: the connection fails
    VIOLATION,
};

/// @brief One message assembled from its data frames (RFC 6455 section 5.4): which frame may come
/// next, the message's kind and length, and where each frame's payload goes. Control frames do not
/// touch it, so they may come between a message's frames (section 5.5).
class WsAssembly {
public:
    /// @brief A data frame of payload_bytes begins; the message may total message_max bytes
    WsFrameStep begin(uint8_t opcode, size_t payload_bytes, size_t message_max);

    /// @brief The frame begun last has all its payload
    /// @return true when it was the message's final frame: the message is message_bytes() long,
    /// and the next data frame opens another
    bool end(bool fin);

    MessageKind kind() const {
        return this->kind_;
    }

    /// @brief The message's bytes before the frame begun last, or its whole length once complete
    size_t message_bytes() const {
        return this->message_bytes_;
    }

    /// @brief The payload of the frame begun last
    size_t frame_bytes() const {
        return this->frame_bytes_;
    }

private:
    MessageKind kind_{MessageKind::BINARY};
    bool open_{false};
    size_t message_bytes_{0};
    size_t frame_bytes_{0};
};

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
/// 5.5), a length not in its fewest bytes or a 64-bit one with its top bit set (section 5.2) or
/// the wrong masking is INVALID.
WsHeaderParse ws_parse_frame_header(std::span<const uint8_t> bytes, bool peer_masks);

}  // namespace sendspin::server
