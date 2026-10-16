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

/// @file frame_payloads.h
/// @brief Frame payloads a test owns: bytes in a slot of their own, so a frame a test holds never
/// keeps a pipeline instance's slot

#pragma once

#include "frame_encoder.h"
#include "payload_slots.h"
#include "sched/frames.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server::test {

/// bytes as a payload in a one-slot store of its own
inline FramePayload payload_of(std::span<const uint8_t> bytes) {
    const SharedPtr<PayloadSlots> slots =
        PayloadSlots::create(std::max<size_t>(1, bytes.size()), 1);
    EXPECT_TRUE(slots);
    const std::span<uint8_t> room = slots->reserve();
    std::copy(bytes.begin(), bytes.end(), room.begin());
    return slots->commit(bytes.size());
}

/// bytes zero bytes as a payload in a store of its own
inline FramePayload zero_payload(size_t bytes) {
    return payload_of(std::vector<uint8_t>(bytes, 0));
}

/// frame with its payload copied into a store of its own
inline sched::Frame copy_of(const sched::Frame& frame) {
    sched::Frame copy;
    copy.epoch = frame.epoch;
    copy.index = frame.index;
    copy.ts_us = frame.ts_us;
    copy.duration_us = frame.duration_us;
    copy.samples = frame.samples;
    copy.payload = payload_of(frame.payload.bytes());
    return copy;
}

/// Bytes copied out, to compare
inline std::vector<uint8_t> bytes_of(std::span<const uint8_t> bytes) {
    return {bytes.begin(), bytes.end()};
}

inline std::vector<uint8_t> bytes_of(const FramePayload& payload) {
    return bytes_of(payload.bytes());
}

/// FrameEncoder::encode() into out, sized to what it wrote
inline bool encode_into(FrameEncoder& encoder, std::span<const uint8_t> input,
                        SampleFormat in_format, std::vector<uint8_t>& out) {
    out.resize(encoder.max_frame_bytes());
    const std::optional<size_t> written = encoder.encode(input, in_format, out);
    out.resize(written.value_or(0));
    return written.has_value();
}

}  // namespace sendspin::server::test
