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

#include "byte_order.h"
#include "transport.h"

#include <algorithm>
#include <bit>
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
constexpr size_t CLOSE_CODE_BYTES = 2;

struct CloseCodes {
    uint16_t low;
    uint16_t high;
};
/// The status codes a close frame may carry (RFC 6455 sections 7.4.1 and 7.4.2, and the IANA
/// registry of section 11.7): not 1004 (reserved), 1005, 1006 or 1015 (never sent), nor the
/// unassigned 1016 to 2999
constexpr std::array<CloseCodes, 3> SENDABLE_CLOSE_CODES{
    {{1000, 1003}, {1007, 1014}, {3000, 4999}}};

/// A UTF-8 form by its lead byte: how many continuation bytes follow and the range of the first,
/// which rules out overlong forms, surrogates and code points past U+10FFFF (RFC 3629 section 4)
struct Utf8Form {
    uint8_t lead_low;
    uint8_t lead_high;
    uint8_t continuations;
    uint8_t second_low;
    uint8_t second_high;
};
constexpr uint8_t CONTINUATION_LOW = 0x80;
constexpr uint8_t CONTINUATION_HIGH = 0xBF;
constexpr std::array<Utf8Form, 9> UTF8_FORMS{{
    {0x00, 0x7F, 0, 0, 0},
    {0xC2, 0xDF, 1, CONTINUATION_LOW, CONTINUATION_HIGH},
    {0xE0, 0xE0, 2, 0xA0, CONTINUATION_HIGH},
    {0xE1, 0xEC, 2, CONTINUATION_LOW, CONTINUATION_HIGH},
    {0xED, 0xED, 2, CONTINUATION_LOW, 0x9F},
    {0xEE, 0xEF, 2, CONTINUATION_LOW, CONTINUATION_HIGH},
    {0xF0, 0xF0, 3, 0x90, CONTINUATION_HIGH},
    {0xF1, 0xF3, 3, CONTINUATION_LOW, CONTINUATION_HIGH},
    {0xF4, 0xF4, 3, CONTINUATION_LOW, 0x8F},
}};

uint32_t load_le32(const uint8_t* bytes) {
    return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8U) | (uint32_t{bytes[2]} << 16U) |
           (uint32_t{bytes[3]} << 24U);
}

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

bool ws_utf8_valid(std::span<const uint8_t> bytes) {
    size_t i = 0;
    while (i < bytes.size()) {
        const uint8_t lead = bytes[i++];
        const auto* form = std::find_if(
            UTF8_FORMS.begin(), UTF8_FORMS.end(),
            [lead](const Utf8Form& f) { return lead >= f.lead_low && lead <= f.lead_high; });
        if (form == UTF8_FORMS.end() || bytes.size() - i < form->continuations) {
            return false;
        }
        for (size_t k = 0; k < form->continuations; ++k, ++i) {
            const uint8_t low = k == 0 ? form->second_low : CONTINUATION_LOW;
            const uint8_t high = k == 0 ? form->second_high : CONTINUATION_HIGH;
            if (bytes[i] < low || bytes[i] > high) {
                return false;
            }
        }
    }
    return true;
}

std::optional<uint16_t> ws_close_payload_fault(std::span<const uint8_t> payload) {
    if (payload.empty()) {
        return std::nullopt;
    }
    if (payload.size() < CLOSE_CODE_BYTES || payload.size() > WS_CONTROL_PAYLOAD_MAX) {
        return close_code::PROTOCOL_ERROR;
    }
    const auto code = read_be<uint16_t>(payload.data());
    const bool sendable =
        std::any_of(SENDABLE_CLOSE_CODES.begin(), SENDABLE_CLOSE_CODES.end(),
                    [code](const CloseCodes& c) { return code >= c.low && code <= c.high; });
    if (!sendable) {
        return close_code::PROTOCOL_ERROR;
    }
    if (!ws_utf8_valid(payload.subspan(CLOSE_CODE_BYTES))) {
        return close_code::INVALID_PAYLOAD;
    }
    return std::nullopt;
}

bool ws_message_valid(MessageKind kind, std::span<const uint8_t> payload) {
    return kind != MessageKind::TEXT || ws_utf8_valid(payload);
}

size_t ws_frame_header(uint8_t opcode, size_t len, std::array<uint8_t, WS_FRAME_HEADER_MAX>& header,
                       const std::optional<WsMaskKey>& mask) {
    header[0] = FIN | opcode;
    const uint8_t mask_bit = mask ? MASKED : 0;
    size_t at = 2;
    if (len <= LEN_7_MAX) {
        header[1] = mask_bit | static_cast<uint8_t>(len);
    } else if (len <= LEN_16_MAX) {
        header[1] = mask_bit | LEN_16;
        put_be(header.data() + 2, static_cast<uint16_t>(len));
        at = 4;
    } else {
        header[1] = mask_bit | LEN_64;
        put_be(header.data() + 2, static_cast<uint64_t>(len));
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

std::array<uint8_t, CHACHA20_BLOCK_BYTES> chacha20_block(
    const std::array<uint8_t, CHACHA20_KEY_BYTES>& key, uint32_t counter,
    const std::array<uint32_t, 3>& nonce) {
    constexpr size_t WORDS = CHACHA20_BLOCK_BYTES / sizeof(uint32_t);
    // RFC 8439 section 2.3: the constants, then the key, the counter and the nonce
    constexpr std::array<uint32_t, 4> CONSTANTS{0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    constexpr size_t KEY_AT = CONSTANTS.size();
    constexpr size_t COUNTER_AT = KEY_AT + (CHACHA20_KEY_BYTES / sizeof(uint32_t));
    constexpr size_t DOUBLE_ROUNDS = 10;
    // Section 2.3: a column round, then a diagonal round, each four quarter rounds
    constexpr std::array<std::array<uint8_t, 4>, 8> QUARTER_ROUNDS{{{0, 4, 8, 12},
                                                                    {1, 5, 9, 13},
                                                                    {2, 6, 10, 14},
                                                                    {3, 7, 11, 15},
                                                                    {0, 5, 10, 15},
                                                                    {1, 6, 11, 12},
                                                                    {2, 7, 8, 13},
                                                                    {3, 4, 9, 14}}};
    // Section 2.1: the quarter round's rotations
    constexpr std::array<int, 4> ROTATIONS{16, 12, 8, 7};
    std::array<uint32_t, WORDS> state{};
    std::copy(CONSTANTS.begin(), CONSTANTS.end(), state.begin());
    for (size_t i = 0; i < CHACHA20_KEY_BYTES / sizeof(uint32_t); ++i) {
        state[KEY_AT + i] = load_le32(key.data() + (i * sizeof(uint32_t)));
    }
    state[COUNTER_AT] = counter;
    std::copy(nonce.begin(), nonce.end(), state.begin() + COUNTER_AT + 1);
    std::array<uint32_t, WORDS> x = state;
    for (size_t round = 0; round < DOUBLE_ROUNDS; ++round) {
        for (const std::array<uint8_t, 4>& q : QUARTER_ROUNDS) {
            uint32_t& a = x[q[0]];
            uint32_t& b = x[q[1]];
            uint32_t& c = x[q[2]];
            uint32_t& d = x[q[3]];
            a += b;
            d = std::rotl(d ^ a, ROTATIONS[0]);
            c += d;
            b = std::rotl(b ^ c, ROTATIONS[1]);
            a += b;
            d = std::rotl(d ^ a, ROTATIONS[2]);
            c += d;
            b = std::rotl(b ^ c, ROTATIONS[3]);
        }
    }
    std::array<uint8_t, CHACHA20_BLOCK_BYTES> block{};
    for (size_t i = 0; i < WORDS; ++i) {
        const uint32_t word = x[i] + state[i];
        for (size_t b = 0; b < sizeof(uint32_t); ++b) {
            block[(i * sizeof(uint32_t)) + b] = static_cast<uint8_t>(word >> (8U * b));
        }
    }
    return block;
}

void WsMaskKeys::seed(const std::array<uint8_t, CHACHA20_KEY_BYTES>& seed, uint32_t stream) {
    this->key_ = seed;
    this->nonce_ = {stream, 0, 0};
    this->counter_ = 0;
    this->taken_ = CHACHA20_BLOCK_BYTES;
}

WsMaskKey WsMaskKeys::next() {
    if (this->taken_ == CHACHA20_BLOCK_BYTES) {
        this->block_ = chacha20_block(this->key_, this->counter_, this->nonce_);
        // Past 2^32 blocks the nonce moves on, so the keystream never repeats
        if (++this->counter_ == 0) {
            ++this->nonce_[1];
        }
        this->taken_ = 0;
    }
    WsMaskKey key{};
    std::memcpy(key.data(), this->block_.data() + this->taken_, key.size());
    this->taken_ += key.size();
    return key;
}

WsFrameStep WsAssembly::begin(uint8_t opcode, size_t payload_bytes, size_t message_max) {
    WsFrameStep step = WsFrameStep::CONTINUES;
    if (opcode == ws_opcode::CONTINUATION) {
        if (!this->open_) {
            return WsFrameStep::VIOLATION;
        }
    } else {
        if (this->open_) {
            return WsFrameStep::VIOLATION;
        }
        this->open_ = true;
        this->kind_ = opcode == ws_opcode::TEXT ? MessageKind::TEXT : MessageKind::BINARY;
        this->message_bytes_ = 0;
        step = WsFrameStep::OPENS;
    }
    if (payload_bytes > message_max - this->message_bytes_) {
        return WsFrameStep::VIOLATION;
    }
    this->frame_bytes_ = payload_bytes;
    return step;
}

bool WsAssembly::end(bool fin) {
    if (!this->open_) {
        return false;
    }
    this->message_bytes_ += this->frame_bytes_;
    this->frame_bytes_ = 0;
    if (!fin) {
        return false;
    }
    this->open_ = false;
    return true;
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
    // Section 5.2: a length in the minimal number of bytes
    if (len7 == LEN_16) {
        header.payload_bytes = read_be<uint16_t>(bytes.data() + 2);
        if (header.payload_bytes <= LEN_7_MAX) {
            return invalid();
        }
    } else if (len7 == LEN_64) {
        header.payload_bytes = read_be<uint64_t>(bytes.data() + 2);
        if ((header.payload_bytes & LEN_64_TOP_BIT) != 0 || header.payload_bytes <= LEN_16_MAX) {
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
