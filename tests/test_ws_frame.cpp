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
/// with its rejections, a close payload's check, a message's assembly from its frames, the masking
/// and the masking keys.

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

using sendspin::server::chacha20_block;
using sendspin::server::CHACHA20_BLOCK_BYTES;
using sendspin::server::CHACHA20_KEY_BYTES;
using sendspin::server::MessageKind;
using sendspin::server::ws_close_payload_fault;
using sendspin::server::ws_frame_header;
using sendspin::server::WS_FRAME_HEADER_MAX;
using sendspin::server::ws_mask_copy;
using sendspin::server::ws_message_valid;
using sendspin::server::ws_parse_frame_header;
using sendspin::server::WsAssembly;
using sendspin::server::WsFrameStep;
using sendspin::server::WsHeaderParse;
using sendspin::server::WsHeaderStatus;
using sendspin::server::WsMaskKey;
using sendspin::server::WsMaskKeys;
namespace close_code = sendspin::server::close_code;
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
        {"Control: 16-bit length of 126", {0x82, 126, 0x00, 126}, false, C, true, 2, 126, 4, {}},
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
        {"16-bit length of 125", {0x82, 126, 0x00, 125}, false, I, false, 0, 0, 0, {}},
        {"64-bit length of 65535",
         {0x82, 127, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF},
         false,
         I,
         false,
         0,
         0,
         0,
         {}},
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

// RFC 6455 sections 5.5.1 and 7.4: a close payload is empty or a status code an endpoint may send
// with a UTF-8 reason (RFC 3629 section 4); a reason that is not UTF-8 fails the connection with
// 1007 (section 7.4.1), any other fault with 1002
TEST(WsFrame, ChecksAClosePayload) {
    struct Row {
        std::string name;
        std::vector<uint8_t> payload;
        std::optional<uint16_t> fault;
    };
    // Code 1000 and a reason of n ASCII bytes
    const auto with_reason = [](size_t n) {
        std::vector<uint8_t> payload{0x03, 0xE8};
        payload.resize(payload.size() + n, 'r');
        return payload;
    };
    constexpr std::optional<uint16_t> VALID;
    constexpr std::optional<uint16_t> PROTOCOL = close_code::PROTOCOL_ERROR;
    constexpr std::optional<uint16_t> PAYLOAD = close_code::INVALID_PAYLOAD;
    const Row rows[] = {
        {"Control: empty", {}, VALID},
        {"Control: 1000", {0x03, 0xE8}, VALID},
        {"Control: 1000 with a multibyte reason, U+00E9 U+20AC U+1F600",
         {0x03, 0xE8, 'o', 'k', 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80},
         VALID},
        {"Control: 1000 with a 123-byte reason", with_reason(123), VALID},
        {"Control: 1003", {0x03, 0xEB}, VALID},
        {"Control: 1007", {0x03, 0xEF}, VALID},
        {"Control: 1014", {0x03, 0xF6}, VALID},
        {"Control: 3000", {0x0B, 0xB8}, VALID},
        {"Control: 4999", {0x13, 0x87}, VALID},
        {"Control: U+D7FF and U+10FFFF",
         {0x03, 0xE8, 0xED, 0x9F, 0xBF, 0xF4, 0x8F, 0xBF, 0xBF},
         VALID},
        {"Control: U+E000 and U+40000",
         {0x03, 0xE8, 0xEE, 0x80, 0x80, 0xF1, 0x80, 0x80, 0x80},
         VALID},
        {"a 1-byte body", {0x03}, PROTOCOL},
        {"999", {0x03, 0xE7}, PROTOCOL},
        {"1004", {0x03, 0xEC}, PROTOCOL},
        {"1005", {0x03, 0xED}, PROTOCOL},
        {"1006", {0x03, 0xEE}, PROTOCOL},
        {"1015", {0x03, 0xF7}, PROTOCOL},
        {"1016", {0x03, 0xF8}, PROTOCOL},
        {"2999", {0x0B, 0xB7}, PROTOCOL},
        {"5000", {0x13, 0x88}, PROTOCOL},
        {"overlong NUL", {0x03, 0xE8, 0xC0, 0x80}, PAYLOAD},
        {"overlong three-byte form", {0x03, 0xE8, 0xE0, 0x9F, 0xBF}, PAYLOAD},
        {"overlong four-byte form", {0x03, 0xE8, 0xF0, 0x8F, 0xBF, 0xBF}, PAYLOAD},
        {"surrogate U+D800", {0x03, 0xE8, 0xED, 0xA0, 0x80}, PAYLOAD},
        {"past U+10FFFF", {0x03, 0xE8, 0xF4, 0x90, 0x80, 0x80}, PAYLOAD},
        {"lead byte past F4", {0x03, 0xE8, 0xF5, 0x80, 0x80, 0x80}, PAYLOAD},
        {"truncated sequence", {0x03, 0xE8, 0xE2, 0x82}, PAYLOAD},
        {"lone continuation", {0x03, 0xE8, 0x80}, PAYLOAD},
        {"continuation byte out of range", {0x03, 0xE8, 0xC3, 0xC3}, PAYLOAD},
        {"continuation byte below range", {0x03, 0xE8, 0xC3, 0x41}, PAYLOAD},
        {"third byte out of range", {0x03, 0xE8, 0xE2, 0x82, 0x41}, PAYLOAD},
        {"fourth byte out of range", {0x03, 0xE8, 0xF0, 0x9F, 0x98, 0xC0}, PAYLOAD},
        {"a 124-byte reason", with_reason(124), PROTOCOL},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(ws_close_payload_fault(row.payload), row.fault);
    }
}

// RFC 6455 section 8.1: a complete text message is UTF-8 over its frames' payloads joined, as
// WsLink::end_frame() checks it, so a form split across two frames is whole; a binary message is
// any bytes
TEST(WsFrame, ChecksATextMessage) {
    struct Row {
        std::string name;
        std::vector<std::vector<uint8_t>> frames;
        MessageKind kind;
        bool valid;
    };
    const Row rows[] = {
        {"Control: empty text", {{}}, MessageKind::TEXT, true},
        {"Control: U+20AC split across two frames",
         {{'a', 0xE2}, {0x82, 0xAC}},
         MessageKind::TEXT,
         true},
        {"Control: binary that is not UTF-8", {{0xFF}, {0xC0, 0x80}}, MessageKind::BINARY, true},
        {"a form whose last byte never came", {{'a', 0xE2}, {0x82}}, MessageKind::TEXT, false},
        {"surrogate U+D800 split across two frames",
         {{0xED}, {0xA0, 0x80}},
         MessageKind::TEXT,
         false},
        {"a lone continuation", {{'a'}, {0x80}}, MessageKind::TEXT, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        std::vector<uint8_t> message;
        for (const std::vector<uint8_t>& frame : row.frames) {
            message.insert(message.end(), frame.begin(), frame.end());
        }
        EXPECT_EQ(ws_message_valid(row.kind, message), row.valid);
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
            std::vector<uint8_t> copied(len + 2, 0xEE);
            ws_mask_copy(copied.data() + 1, source.data() + start, len, key);
            EXPECT_EQ(std::vector<uint8_t>(copied.begin() + 1, copied.end() - 1), expected);
            EXPECT_EQ(copied.front(), 0xEE);
            EXPECT_EQ(copied.back(), 0xEE);

            std::vector<uint8_t> in_place(
                source.begin(), source.begin() + static_cast<std::ptrdiff_t>(start + len));
            ws_mask_copy(in_place.data() + start, in_place.data() + start, len, key);
            EXPECT_EQ(std::vector<uint8_t>(in_place.begin() + static_cast<std::ptrdiff_t>(start),
                                           in_place.end()),
                      expected);
        }
    }
}

std::array<uint8_t, CHACHA20_KEY_BYTES> counting_key() {
    std::array<uint8_t, CHACHA20_KEY_BYTES> key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    return key;
}

// RFC 8439 section 2.3.2's test vector
TEST(WsMaskKeys, BlockFunctionMatchesTheRfcVector) {
    const std::array<uint8_t, CHACHA20_BLOCK_BYTES> expected{
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3,
        0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22,
        0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa,
        0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1,
        0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};
    // The nonce 00:00:00:09:00:00:00:4a:00:00:00:00, as little-endian words
    EXPECT_EQ(chacha20_block(counting_key(), 1, {0x09000000, 0x4a000000, 0}), expected);
}

// Each key is the next four bytes of the stream's keystream, across block refills, and two
// streams under one seed differ
TEST(WsMaskKeys, ReadsTheKeystreamFourBytesAtATime) {
    constexpr uint32_t STREAM = 7;
    constexpr size_t BLOCKS = 3;
    std::vector<uint8_t> keystream;
    for (uint32_t block = 0; block < BLOCKS; ++block) {
        const std::array<uint8_t, CHACHA20_BLOCK_BYTES> bytes =
            chacha20_block(counting_key(), block, {STREAM, 0, 0});
        keystream.insert(keystream.end(), bytes.begin(), bytes.end());
    }
    WsMaskKeys keys;
    keys.seed(counting_key(), STREAM);
    for (size_t at = 0; at < keystream.size(); at += 4) {
        SCOPED_TRACE("byte " + std::to_string(at));
        const WsMaskKey key = keys.next();
        EXPECT_EQ(std::vector<uint8_t>(key.begin(), key.end()),
                  std::vector<uint8_t>(keystream.begin() + static_cast<std::ptrdiff_t>(at),
                                       keystream.begin() + static_cast<std::ptrdiff_t>(at + 4)));
    }
    WsMaskKeys other;
    other.seed(counting_key(), STREAM + 1);
    WsMaskKeys same;
    same.seed(counting_key(), STREAM);
    EXPECT_NE(other.next(), same.next());
}

// RFC 6455 section 5.4: a message is a text or binary frame and the continuations up to its final
// frame, within its cap; each row's frames go through one assembly in order, and the last frame's
// step and the message completed are checked
TEST(WsAssembly, AssemblesAMessageFromItsFrames) {
    struct Frame {
        uint8_t opcode;
        size_t bytes;
        bool fin;
    };
    struct Row {
        std::string name;
        std::vector<Frame> frames;
        WsFrameStep last_step;
        // For a complete message
        std::optional<MessageKind> kind;
        size_t message_bytes;
    };
    constexpr size_t CAP = 64;
    constexpr uint8_t TEXT = ws_opcode::TEXT;
    constexpr uint8_t BINARY = ws_opcode::BINARY;
    constexpr uint8_t CONT = ws_opcode::CONTINUATION;
    constexpr auto OPENS = WsFrameStep::OPENS;
    constexpr auto CONTINUES = WsFrameStep::CONTINUES;
    constexpr auto VIOLATION = WsFrameStep::VIOLATION;
    const Row rows[] = {
        {"Control: one text frame", {{TEXT, 5, true}}, OPENS, MessageKind::TEXT, 5},
        {"Control: binary in three frames, an empty one among them",
         {{BINARY, 10, false}, {CONT, 0, false}, {CONT, 7, true}},
         CONTINUES,
         MessageKind::BINARY,
         17},
        {"Control: a message of the cap in two frames",
         {{BINARY, CAP - 1, false}, {CONT, 1, true}},
         CONTINUES,
         MessageKind::BINARY,
         CAP},
        {"Control: a message after a complete one starts from nothing",
         {{TEXT, 30, true}, {BINARY, 40, true}},
         OPENS,
         MessageKind::BINARY,
         40},
        {"Control: a fragment pending", {{TEXT, 3, false}}, OPENS, std::nullopt, 0},
        {"a continuation with no message open", {{CONT, 1, true}}, VIOLATION, std::nullopt, 0},
        {"a continuation after a complete message",
         {{TEXT, 1, true}, {CONT, 1, true}},
         VIOLATION,
         std::nullopt,
         0},
        {"a text frame inside a message",
         {{BINARY, 1, false}, {TEXT, 1, true}},
         VIOLATION,
         std::nullopt,
         0},
        {"a binary frame inside a message",
         {{TEXT, 1, false}, {BINARY, 1, true}},
         VIOLATION,
         std::nullopt,
         0},
        {"one frame over the cap", {{BINARY, CAP + 1, true}}, VIOLATION, std::nullopt, 0},
        {"fragments over the cap",
         {{BINARY, CAP - 1, false}, {CONT, 2, true}},
         VIOLATION,
         std::nullopt,
         0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        WsAssembly assembly;
        WsFrameStep step = WsFrameStep::OPENS;
        bool complete = false;
        size_t frame_at = 0;
        for (const Frame& frame : row.frames) {
            const size_t before = assembly.message_bytes();
            step = assembly.begin(frame.opcode, frame.bytes, CAP);
            if (step == WsFrameStep::VIOLATION) {
                break;
            }
            // A continuation's payload goes after the bytes before it
            EXPECT_EQ(assembly.message_bytes(), step == WsFrameStep::OPENS ? 0 : before);
            EXPECT_EQ(assembly.frame_bytes(), frame.bytes);
            frame_at = assembly.message_bytes();
            complete = assembly.end(frame.fin);
        }
        EXPECT_EQ(step, row.last_step);
        if (row.last_step == WsFrameStep::VIOLATION) {
            continue;
        }
        EXPECT_EQ(complete, row.kind.has_value());
        if (row.kind) {
            EXPECT_EQ(assembly.kind(), *row.kind);
            EXPECT_EQ(assembly.message_bytes(), row.message_bytes);
            EXPECT_EQ(frame_at + row.frames.back().bytes, row.message_bytes);
        }
    }
}

}  // namespace
