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

#include "ws_frame.h"

#include <cstring>

namespace sendspin::server {

namespace {

constexpr uint8_t FIN = 0x80;
constexpr uint8_t RSV_MASK = 0x70;
constexpr uint8_t OPCODE_MASK = 0x0F;
constexpr uint8_t CONTROL_BIT = 0x08;
constexpr uint8_t MASKED = 0x80;
constexpr uint8_t LEN_MASK = 0x7F;
constexpr size_t LEN_7_MAX = 125;
constexpr size_t LEN_16_MAX = 65535;
constexpr uint8_t LEN_16 = 126;
constexpr uint8_t LEN_64 = 127;
constexpr size_t LEN_64_BYTES = 8;
constexpr uint64_t LEN_64_TOP_BIT = uint64_t{1} << 63U;

bool known_opcode(uint8_t opcode) {
    switch (opcode) {
        case ws_opcode::CONTINUATION:
        case ws_opcode::TEXT:
        case ws_opcode::BINARY:
        case ws_opcode::CLOSE:
        case ws_opcode::PING:
        case ws_opcode::PONG:
            return true;
        default:
            return false;
    }
}

}  // namespace

size_t ws_frame_header(uint8_t opcode, size_t len, std::array<uint8_t, WS_FRAME_HEADER_MAX>& header,
                       const std::optional<WsMaskKey>& mask) {
    header[0] = FIN | opcode;
    const uint8_t mask_bit = mask ? MASKED : 0;
    size_t at = 2;
    if (len <= LEN_7_MAX) {
        header[1] = mask_bit | static_cast<uint8_t>(len);
    } else if (len <= LEN_16_MAX) {
        header[1] = mask_bit | LEN_16;
        header[2] = static_cast<uint8_t>(len >> 8U);
        header[3] = static_cast<uint8_t>(len);
        at = 4;
    } else {
        header[1] = mask_bit | LEN_64;
        const auto len64 = static_cast<uint64_t>(len);
        for (size_t i = 0; i < LEN_64_BYTES; ++i) {
            header[2 + i] = static_cast<uint8_t>(len64 >> (8U * (LEN_64_BYTES - 1 - i)));
        }
        at = 2 + LEN_64_BYTES;
    }
    if (mask) {
        std::memcpy(header.data() + at, mask->data(), mask->size());
        at += mask->size();
    }
    return at;
}

void ws_mask_copy(uint8_t* dst, const uint8_t* src, size_t len, const WsMaskKey& key) {
    // memcpy() handles any alignment at full speed; the XOR then runs in place over aligned words,
    // where a target without fast unaligned access (RISC-V) would otherwise move bytes one by one
    if (dst != src) {
        std::memcpy(dst, src, len);
    }
    size_t i = 0;
    for (; i < len && (reinterpret_cast<uintptr_t>(dst + i) % sizeof(uint32_t)) != 0; ++i) {
        dst[i] ^= key[i % key.size()];
    }
    if (len - i >= sizeof(uint32_t)) {
        // The key turned to start at byte i, as a word in memory order
        WsMaskKey turned{};
        for (size_t j = 0; j < turned.size(); ++j) {
            turned[j] = key[(i + j) % key.size()];
        }
        uint32_t key_word = 0;
        std::memcpy(&key_word, turned.data(), sizeof(key_word));
        auto* words = static_cast<uint8_t*>(__builtin_assume_aligned(dst + i, sizeof(uint32_t)));
        const auto mask_word = [words, key_word](size_t at) {
            uint32_t word = 0;
            std::memcpy(&word, words + at, sizeof(word));
            word ^= key_word;
            std::memcpy(words + at, &word, sizeof(word));
        };
        const size_t word_bytes = (len - i) & ~(sizeof(uint32_t) - 1);
        constexpr size_t UNROLLED = 4 * sizeof(uint32_t);
        size_t w = 0;
        for (; w + UNROLLED <= word_bytes; w += UNROLLED) {
            mask_word(w);
            mask_word(w + sizeof(uint32_t));
            mask_word(w + (2 * sizeof(uint32_t)));
            mask_word(w + (3 * sizeof(uint32_t)));
        }
        for (; w < word_bytes; w += sizeof(uint32_t)) {
            mask_word(w);
        }
        i += word_bytes;
    }
    for (; i < len; ++i) {
        dst[i] ^= key[i % key.size()];
    }
}

WsHeaderParse ws_parse_frame_header(std::span<const uint8_t> bytes, bool peer_masks) {
    WsHeaderParse parse;
    if (bytes.size() < 2) {
        return parse;
    }
    const auto invalid = [&parse] {
        parse.status = WsHeaderStatus::INVALID;
        return parse;
    };
    WsFrameHeader& header = parse.header;
    header.fin = (bytes[0] & FIN) != 0;
    header.opcode = bytes[0] & OPCODE_MASK;
    const bool masked = (bytes[1] & MASKED) != 0;
    if ((bytes[0] & RSV_MASK) != 0 || !known_opcode(header.opcode) || masked != peer_masks) {
        return invalid();
    }
    const uint8_t len7 = bytes[1] & LEN_MASK;
    const bool control = (header.opcode & CONTROL_BIT) != 0;
    if (control && (!header.fin || len7 > LEN_7_MAX)) {
        return invalid();
    }
    size_t at = 2;
    if (len7 == LEN_16) {
        at += 2;
    } else if (len7 == LEN_64) {
        at += LEN_64_BYTES;
    }
    const size_t mask_at = at;
    if (masked) {
        at += WsMaskKey{}.size();
    }
    if (bytes.size() < at) {
        return parse;
    }
    if (len7 == LEN_16) {
        header.payload_bytes = (uint64_t{bytes[2]} << 8U) | bytes[3];
    } else if (len7 == LEN_64) {
        for (size_t i = 0; i < LEN_64_BYTES; ++i) {
            header.payload_bytes = (header.payload_bytes << 8U) | bytes[2 + i];
        }
        if ((header.payload_bytes & LEN_64_TOP_BIT) != 0) {
            return invalid();
        }
    } else {
        header.payload_bytes = len7;
    }
    if (masked) {
        WsMaskKey key{};
        std::memcpy(key.data(), bytes.data() + mask_at, key.size());
        header.mask = key;
    }
    header.header_bytes = at;
    parse.status = WsHeaderStatus::COMPLETE;
    return parse;
}

}  // namespace sendspin::server
