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

#include "crypto/base64url.h"

namespace sendspin::server {

namespace {

constexpr std::string_view ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view STANDARD_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr int BITS_PER_CHAR = 6;
constexpr uint32_t CHAR_MASK = 0x3F;
constexpr uint32_t BYTE_MASK = 0xFF;
constexpr size_t BYTES_PER_GROUP = 3;
constexpr size_t CHARS_PER_GROUP = 4;
constexpr uint8_t INVALID = 0xFF;

uint8_t value_of(char c) {
    const size_t index = ALPHABET.find(c);
    return index == std::string_view::npos ? INVALID : static_cast<uint8_t>(index);
}

std::string encode(const uint8_t* data, size_t len, std::string_view alphabet) {
    std::string out;
    out.reserve(((len + BYTES_PER_GROUP - 1) / BYTES_PER_GROUP) * CHARS_PER_GROUP);
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        acc = (acc << 8U) | data[i];
        bits += 8;
        while (bits >= BITS_PER_CHAR) {
            bits -= BITS_PER_CHAR;
            out += alphabet[(acc >> static_cast<uint32_t>(bits)) & CHAR_MASK];
        }
    }
    if (bits > 0) {
        out += alphabet[(acc << static_cast<uint32_t>(BITS_PER_CHAR - bits)) & CHAR_MASK];
    }
    return out;
}

}  // namespace

std::string b64url_encode(const uint8_t* data, size_t len) {
    return encode(data, len, ALPHABET);
}

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out = encode(data, len, STANDARD_ALPHABET);
    while (out.size() % CHARS_PER_GROUP != 0) {
        out += '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> b64url_decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    // One character past a whole group carries only 6 bits, less than a byte
    if (text.size() % CHARS_PER_GROUP == 1) {
        return std::nullopt;
    }
    std::vector<uint8_t> out;
    out.reserve((text.size() * BYTES_PER_GROUP) / CHARS_PER_GROUP);
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const uint8_t value = value_of(c);
        if (value == INVALID) {
            return std::nullopt;
        }
        acc = (acc << static_cast<uint32_t>(BITS_PER_CHAR)) | value;
        bits += BITS_PER_CHAR;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> static_cast<uint32_t>(bits)) & BYTE_MASK));
        }
    }
    return out;
}

}  // namespace sendspin::server
