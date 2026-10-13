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

/// @file test_ws_batch.cpp
/// @brief WsBatch, the frames of one write as the ESP-IDF transport sends them: the bytes on the
/// wire, unmasked and masked, read back with ws_parse_frame_header() on the host.

#include "transport.h"
#include "ws_batch.h"
#include "ws_frame.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace {

using sendspin::server::MessageKind;
using sendspin::server::ws_mask_copy;
using sendspin::server::ws_parse_frame_header;
using sendspin::server::WsBatch;
using sendspin::server::WsHeaderParse;
using sendspin::server::WsHeaderStatus;
using sendspin::server::WsMaskKey;
using sendspin::server::ws_opcode::BINARY;
using sendspin::server::ws_opcode::TEXT;

std::vector<uint8_t> payload_of(size_t size, uint8_t seed) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(seed + (i * 7));
    }
    return out;
}

/// Reads each frame of wire as the peer does, unmasking a masked one
struct ReadFrame {
    uint8_t opcode;
    std::optional<WsMaskKey> mask;
    std::vector<uint8_t> payload;
};

std::vector<ReadFrame> read_frames(std::span<const uint8_t> wire, bool masked) {
    std::vector<ReadFrame> frames;
    while (!wire.empty()) {
        const WsHeaderParse parse = ws_parse_frame_header(wire, masked);
        EXPECT_EQ(parse.status, WsHeaderStatus::COMPLETE);
        if (parse.status != WsHeaderStatus::COMPLETE || !parse.header.fin) {
            break;
        }
        wire = wire.subspan(parse.header.header_bytes);
        const auto size = static_cast<size_t>(parse.header.payload_bytes);
        std::vector<uint8_t> payload(wire.begin(),
                                     wire.begin() + static_cast<std::ptrdiff_t>(size));
        if (parse.header.mask) {
            ws_mask_copy(payload.data(), payload.data(), payload.size(), *parse.header.mask);
        }
        frames.push_back({parse.header.opcode, parse.header.mask, std::move(payload)});
        wire = wire.subspan(size);
    }
    return frames;
}

struct Row {
    MessageKind kind;
    size_t size;
    // The header's first bytes, before any masking key
    std::vector<uint8_t> header;
};

const Row ROWS[] = {
    {MessageKind::TEXT, 0, {0x81, 0x00}},
    {MessageKind::BINARY, 1, {0x82, 1}},
    {MessageKind::BINARY, 125, {0x82, 125}},
    {MessageKind::BINARY, 126, {0x82, 126, 0x00, 126}},
    {MessageKind::TEXT, 303, {0x81, 126, 0x01, 0x2F}},
    {MessageKind::BINARY, sendspin::server::MAX_MESSAGE_BYTES, {0x82, 126, 0xFF, 0xFF}},
};

// RFC 6455 section 5.2: each message a final, unmasked frame with the shortest length encoding,
// back to back, and read back in order as written
TEST(WsBatch, HoldsEachMessageAsAFinalUnmaskedFrame) {
    WsBatch batch;
    std::vector<uint8_t> wire;
    for (size_t i = 0; i < std::size(ROWS); ++i) {
        const std::vector<uint8_t> payload = payload_of(ROWS[i].size, static_cast<uint8_t>(i));
        batch.append(ROWS[i].kind, payload);
        wire.insert(wire.end(), ROWS[i].header.begin(), ROWS[i].header.end());
        wire.insert(wire.end(), payload.begin(), payload.end());
    }
    EXPECT_EQ(batch.frames(), std::size(ROWS));
    EXPECT_EQ(std::vector<uint8_t>(batch.bytes().begin(), batch.bytes().end()), wire);

    const std::vector<ReadFrame> frames = read_frames(batch.bytes(), false);
    ASSERT_EQ(frames.size(), std::size(ROWS));
    for (size_t i = 0; i < std::size(ROWS); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(frames[i].opcode, ROWS[i].kind == MessageKind::TEXT ? TEXT : BINARY);
        EXPECT_EQ(frames[i].payload, payload_of(ROWS[i].size, static_cast<uint8_t>(i)));
    }

    batch.clear();
    EXPECT_EQ(batch.frames(), 0U);
    EXPECT_TRUE(batch.bytes().empty());
}

// RFC 6455 section 5.3: a dialed connection's frames carry the mask bit and the key given for
// each, and the payload XORed with it, each byte with the key byte its index selects; unmasking
// gives the message back. Sizes around a word cover the masking's word and byte steps.
TEST(WsBatch, MasksEachFrameWithItsOwnKey) {
    WsBatch batch;
    std::vector<WsMaskKey> keys;
    for (size_t i = 0; i < std::size(ROWS); ++i) {
        const WsMaskKey key{static_cast<uint8_t>(0x10 + i), static_cast<uint8_t>(0xA5 ^ i),
                            static_cast<uint8_t>(0x3C + (7 * i)), static_cast<uint8_t>(0xF0 - i)};
        keys.push_back(key);
        batch.append(ROWS[i].kind, payload_of(ROWS[i].size, static_cast<uint8_t>(i)), key);
    }
    EXPECT_EQ(batch.frames(), std::size(ROWS));

    // The wire bytes as RFC 6455 section 5.3 defines them, built byte by byte
    std::vector<uint8_t> wire;
    for (size_t i = 0; i < std::size(ROWS); ++i) {
        std::vector<uint8_t> header = ROWS[i].header;
        header[1] |= 0x80;
        wire.insert(wire.end(), header.begin(), header.end());
        wire.insert(wire.end(), keys[i].begin(), keys[i].end());
        const std::vector<uint8_t> payload = payload_of(ROWS[i].size, static_cast<uint8_t>(i));
        for (size_t j = 0; j < payload.size(); ++j) {
            wire.push_back(payload[j] ^ keys[i][j % 4]);
        }
    }
    EXPECT_EQ(std::vector<uint8_t>(batch.bytes().begin(), batch.bytes().end()), wire);

    const std::vector<ReadFrame> frames = read_frames(batch.bytes(), true);
    ASSERT_EQ(frames.size(), std::size(ROWS));
    for (size_t i = 0; i < std::size(ROWS); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(frames[i].opcode, ROWS[i].kind == MessageKind::TEXT ? TEXT : BINARY);
        EXPECT_EQ(frames[i].mask, keys[i]);
        EXPECT_EQ(frames[i].payload, payload_of(ROWS[i].size, static_cast<uint8_t>(i)));
    }
}

}  // namespace
