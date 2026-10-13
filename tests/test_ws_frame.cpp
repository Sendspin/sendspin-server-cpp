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

/// @file test_ws_frame.cpp
/// @brief WebSocket framing for the ESP-IDF transport, on the host: a received header's parse
/// with its rejections, and the masking.

#include "ws_frame.h"
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using sendspin::server::ws_frame_header;
using sendspin::server::WS_FRAME_HEADER_MAX;
using sendspin::server::ws_mask_copy;
using sendspin::server::ws_parse_frame_header;
using sendspin::server::WsHeaderParse;
using sendspin::server::WsHeaderStatus;
using sendspin::server::WsMaskKey;
namespace ws_opcode = sendspin::server::ws_opcode;

// RFC 6455 sections 5.2 and 5.5: what a client (peer_masks false) and a server (true) accept of
// a received header, and the bytes it must wait for
TEST(WsFrame, ParsesAReceivedHeader) {
    struct Row {
        std::string name;
        std::vector<uint8_t> bytes;
        bool peer_masks;
        WsHeaderStatus status;
        // For COMPLETE
        bool fin;
        uint8_t opcode;
        uint64_t payload_bytes;
        size_t header_bytes;
        std::optional<WsMaskKey> mask;
    };
    constexpr auto C = WsHeaderStatus::COMPLETE;
    constexpr auto I = WsHeaderStatus::INVALID;
    constexpr auto N = WsHeaderStatus::INCOMPLETE;
    const Row rows[] = {
        {"Control: final binary, 7-bit length", {0x82, 0x05}, false, C, true, 2, 5, 2, {}},
        {"Control: text continued, not final", {0x01, 0x00}, false, C, false, 1, 0, 2, {}},
        {"Control: continuation", {0x80, 0x03}, false, C, true, 0, 3, 2, {}},
        {"Control: 16-bit length", {0x82, 126, 0x01, 0x00}, false, C, true, 2, 256, 4, {}},
        {"Control: 64-bit length",
         {0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0},
         false,
         C,
         true,
         2,
         65536,
         10,
         {}},
        {"Control: ping of 125 bytes", {0x89, 125}, false, C, true, 9, 125, 2, {}},
        {"Control: masked, as a server receives it",
         {0x82, 0x83, 1, 2, 3, 4},
         true,
         C,
         true,
         2,
         3,
         6,
         WsMaskKey{1, 2, 3, 4}},
        {"Control: masked close",
         {0x88, 0x82, 9, 8, 7, 6},
         true,
         C,
         true,
         8,
         2,
         6,
         WsMaskKey{9, 8, 7, 6}},
        {"empty", {}, false, N, false, 0, 0, 0, {}},
        {"one byte", {0x82}, false, N, false, 0, 0, 0, {}},
        {"16-bit length cut", {0x82, 126, 0x01}, false, N, false, 0, 0, 0, {}},
        {"64-bit length cut", {0x82, 127, 0, 0, 0, 0, 0, 1, 0}, false, N, false, 0, 0, 0, {}},
        {"masking key cut", {0x82, 0x83, 1, 2, 3}, true, N, false, 0, 0, 0, {}},
        {"RSV1", {0xC2, 0x00}, false, I, false, 0, 0, 0, {}},
        {"RSV2", {0xA2, 0x00}, false, I, false, 0, 0, 0, {}},
        {"RSV3", {0x92, 0x00}, false, I, false, 0, 0, 0, {}},
        {"reserved data opcode", {0x83, 0x00}, false, I, false, 0, 0, 0, {}},
        {"reserved control opcode", {0x8B, 0x00}, false, I, false, 0, 0, 0, {}},
        {"fragmented ping", {0x09, 0x00}, false, I, false, 0, 0, 0, {}},
        {"close over 125 bytes", {0x88, 126, 0x00, 126}, false, I, false, 0, 0, 0, {}},
        {"masked frame to a client", {0x82, 0x80, 1, 2, 3, 4}, false, I, false, 0, 0, 0, {}},
        {"unmasked frame to a server", {0x82, 0x00}, true, I, false, 0, 0, 0, {}},
        {"64-bit length with its top bit",
         {0x82, 127, 0x80, 0, 0, 0, 0, 0, 0, 0},
         false,
         I,
         false,
         0,
         0,
         0,
         {}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const WsHeaderParse parse = ws_parse_frame_header(row.bytes, row.peer_masks);
        EXPECT_EQ(parse.status, row.status);
        if (row.status != C || parse.status != C) {
            continue;
        }
        EXPECT_EQ(parse.header.fin, row.fin);
        EXPECT_EQ(parse.header.opcode, row.opcode);
        EXPECT_EQ(parse.header.payload_bytes, row.payload_bytes);
        EXPECT_EQ(parse.header.header_bytes, row.header_bytes);
        EXPECT_EQ(parse.header.mask, row.mask);
    }
}

// The header written for each length encoding, masked or not, parses back to what was written
TEST(WsFrame, WrittenHeaderParsesBack) {
    const WsMaskKey key{0xDE, 0xAD, 0xBE, 0xEF};
    for (const size_t len : {size_t{0}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}}) {
        for (const bool masked : {false, true}) {
            SCOPED_TRACE(std::to_string(len) + (masked ? " masked" : ""));
            std::array<uint8_t, WS_FRAME_HEADER_MAX> header{};
            const size_t bytes = ws_frame_header(ws_opcode::BINARY, len, header,
                                                 masked ? std::optional(key) : std::nullopt);
            const WsHeaderParse parse =
                ws_parse_frame_header(std::span<const uint8_t>(header.data(), bytes), masked);
            ASSERT_EQ(parse.status, WsHeaderStatus::COMPLETE);
            EXPECT_TRUE(parse.header.fin);
            EXPECT_EQ(parse.header.opcode, ws_opcode::BINARY);
            EXPECT_EQ(parse.header.payload_bytes, len);
            EXPECT_EQ(parse.header.header_bytes, bytes);
            EXPECT_EQ(parse.header.mask, masked ? std::optional(key) : std::nullopt);
        }
    }
}

// RFC 6455 section 5.3: byte i XORed with key byte i mod 4, for every length through the
// unaligned head, the unrolled and single word steps and the tail, at every alignment, in place
// as well as copied
TEST(WsFrame, MaskCopyXorsEachByteWithItsKeyByte) {
    const WsMaskKey key{0x37, 0xFA, 0x21, 0x3D};
    std::vector<uint8_t> source(64);
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>((i * 29) + 3);
    }
    for (size_t start = 0; start < 4; ++start) {
        for (size_t len = 0; len + start <= 45; ++len) {
            SCOPED_TRACE("start " + std::to_string(start) + " len " + std::to_string(len));
            std::vector<uint8_t> expected(len);
            for (size_t i = 0; i < len; ++i) {
                expected[i] = source[start + i] ^ key[i % 4];
            }
            std::vector<uint8_t> copied(len + 1, 0xEE);
            ws_mask_copy(copied.data() + 1, source.data() + start, len, key);
            EXPECT_EQ(std::vector<uint8_t>(copied.begin() + 1, copied.end()), expected);
            EXPECT_EQ(copied[0], 0xEE);

            std::vector<uint8_t> in_place(
                source.begin(), source.begin() + static_cast<std::ptrdiff_t>(start + len));
            ws_mask_copy(in_place.data() + start, in_place.data() + start, len, key);
            EXPECT_EQ(std::vector<uint8_t>(in_place.begin() + static_cast<std::ptrdiff_t>(start),
                                           in_place.end()),
                      expected);
        }
    }
}

}  // namespace
