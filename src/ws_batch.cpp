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

#include "ws_batch.h"

namespace sendspin::server {

namespace {

constexpr uint8_t FIN = 0x80;
constexpr uint8_t OPCODE_MASK = 0x0F;
constexpr size_t LEN_7_MAX = 125;
constexpr size_t LEN_16_MAX = 65535;
constexpr uint8_t LEN_16 = 126;
constexpr uint8_t LEN_64 = 127;

}  // namespace

size_t ws_frame_header(uint8_t opcode, size_t len,
                       std::array<uint8_t, WS_FRAME_HEADER_MAX>& header) {
    header[0] = FIN | opcode;
    if (len <= LEN_7_MAX) {
        header[1] = static_cast<uint8_t>(len);
        return 2;
    }
    if (len <= LEN_16_MAX) {
        header[1] = LEN_16;
        header[2] = static_cast<uint8_t>(len >> 8U);
        header[3] = static_cast<uint8_t>(len);
        return 4;
    }
    header[1] = LEN_64;
    const auto len64 = static_cast<uint64_t>(len);
    for (size_t i = 0; i < 8; ++i) {
        header[2 + i] = static_cast<uint8_t>(len64 >> (8U * (7 - i)));
    }
    return WS_FRAME_HEADER_MAX;
}

void WsBatch::append(MessageKind kind, std::span<const uint8_t> payload) {
    std::array<uint8_t, WS_FRAME_HEADER_MAX> header{};
    const size_t header_bytes = ws_frame_header(
        kind == MessageKind::TEXT ? ws_opcode::TEXT : ws_opcode::BINARY, payload.size(), header);
    this->bytes_.insert(this->bytes_.end(), header.begin(), header.begin() + header_bytes);
    this->bytes_.insert(this->bytes_.end(), payload.begin(), payload.end());
    ++this->frames_;
}

std::optional<WsBatch::Frame> WsBatch::next(size_t& offset) const {
    if (offset >= this->bytes_.size()) {
        return std::nullopt;
    }
    const uint8_t* at = this->bytes_.data() + offset;
    const uint8_t opcode = at[0] & OPCODE_MASK;
    size_t header_bytes = 2;
    uint64_t len = at[1];
    if (len == LEN_16) {
        header_bytes = 4;
        len = (uint64_t{at[2]} << 8U) | at[3];
    } else if (len == LEN_64) {
        header_bytes = WS_FRAME_HEADER_MAX;
        len = 0;
        for (size_t i = 0; i < 8; ++i) {
            len = (len << 8U) | at[2 + i];
        }
    }
    const Frame frame{opcode == ws_opcode::TEXT ? MessageKind::TEXT : MessageKind::BINARY,
                      std::span<const uint8_t>(at + header_bytes, static_cast<size_t>(len))};
    offset += header_bytes + static_cast<size_t>(len);
    return frame;
}

}  // namespace sendspin::server
