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
/// wire and the frames read back, on the host.

#include "transport.h"
#include "ws_batch.h"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace {

using sendspin::server::MessageKind;
using sendspin::server::WsBatch;

std::vector<uint8_t> payload_of(size_t size, uint8_t seed) {
    std::vector<uint8_t> out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(seed + (i * 7));
    }
    return out;
}

// RFC 6455 section 5.2: each message a final, unmasked frame with the shortest length encoding,
// back to back, and read back in order as written
TEST(WsBatch, HoldsEachMessageAsAFinalUnmaskedFrame) {
    struct Row {
        MessageKind kind;
        size_t size;
        // The header the frame must start with
        std::vector<uint8_t> header;
    };
    const Row rows[] = {
        {MessageKind::TEXT, 0, {0x81, 0x00}},
        {MessageKind::BINARY, 125, {0x82, 125}},
        {MessageKind::BINARY, 126, {0x82, 126, 0x00, 126}},
        {MessageKind::TEXT, 300, {0x81, 126, 0x01, 0x2C}},
        {MessageKind::BINARY, sendspin::server::MAX_MESSAGE_BYTES, {0x82, 126, 0xFF, 0xFF}},
    };
    WsBatch batch;
    std::vector<uint8_t> wire;
    for (size_t i = 0; i < std::size(rows); ++i) {
        const std::vector<uint8_t> payload = payload_of(rows[i].size, static_cast<uint8_t>(i));
        batch.append(rows[i].kind, payload);
        wire.insert(wire.end(), rows[i].header.begin(), rows[i].header.end());
        wire.insert(wire.end(), payload.begin(), payload.end());
    }
    EXPECT_EQ(batch.frames(), std::size(rows));
    EXPECT_EQ(std::vector<uint8_t>(batch.bytes().begin(), batch.bytes().end()), wire);

    size_t offset = 0;
    for (size_t i = 0; i < std::size(rows); ++i) {
        SCOPED_TRACE(i);
        const std::optional<WsBatch::Frame> frame = batch.next(offset);
        ASSERT_TRUE(frame);
        EXPECT_EQ(frame->kind, rows[i].kind);
        EXPECT_EQ(std::vector<uint8_t>(frame->payload.begin(), frame->payload.end()),
                  payload_of(rows[i].size, static_cast<uint8_t>(i)));
    }
    EXPECT_FALSE(batch.next(offset));
    EXPECT_EQ(offset, wire.size());

    batch.clear();
    EXPECT_EQ(batch.frames(), 0U);
    EXPECT_TRUE(batch.bytes().empty());
    offset = 0;
    EXPECT_FALSE(batch.next(offset));
}

}  // namespace
