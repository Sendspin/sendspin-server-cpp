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

#include "crypto/pairing_token.h"

#include "crypto/crypto.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace sendspin::server {

namespace {

/// RFC 4648 section 6
constexpr std::string_view BASE32_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
constexpr int BASE32_BITS_PER_CHAR = 5;
constexpr uint64_t BASE32_CHAR_MASK = 0x1F;
constexpr size_t BASE32_GROUP_BYTES = 5;
constexpr size_t BASE32_GROUP_CHARS = 8;
/// Meaningful characters of a final group of 1 to 5 bytes (RFC 4648 section 6)
constexpr std::array<size_t, BASE32_GROUP_BYTES + 1> CHARS_FOR_BYTES = {0, 2, 4, 5, 7, 8};
constexpr std::string_view TOKEN_PREFIX = "SP:";
constexpr size_t PSK_PAYLOAD_SIZE = X25519_KEY_SIZE + PSK_SIZE;

std::string base32_encode(const uint8_t* data, size_t len) {
    std::string out;
    for (size_t i = 0; i < len; i += BASE32_GROUP_BYTES) {
        const size_t group = std::min(BASE32_GROUP_BYTES, len - i);
        uint64_t bits = 0;
        for (size_t j = 0; j < BASE32_GROUP_BYTES; ++j) {
            bits = (bits << 8) | (j < group ? data[i + j] : 0);
        }
        for (size_t k = 0; k < CHARS_FOR_BYTES[group]; ++k) {
            const auto shift =
                static_cast<unsigned>((BASE32_GROUP_CHARS - 1 - k) * BASE32_BITS_PER_CHAR);
            out.push_back(BASE32_ALPHABET[(bits >> shift) & BASE32_CHAR_MASK]);
        }
    }
    return out;
}

std::string format_token(char version, const uint8_t* payload, size_t len) {
    std::string body = base32_encode(payload, len);
    std::replace(body.begin(), body.end(), '2', '9');
    return std::string(TOKEN_PREFIX) + version + body;
}

/// The payload of a token of version, or nullopt; the caller wipes it
std::optional<std::vector<uint8_t>> decode_token(std::string_view input, char version,
                                                 size_t min_payload) {
    constexpr std::string_view WHITESPACE = " \t\r\n\f\v";
    const size_t first = input.find_first_not_of(WHITESPACE);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    input = input.substr(first, input.find_last_not_of(WHITESPACE) - first + 1);
    std::string text(input);
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    std::string_view body = text;
    if (body.starts_with(TOKEN_PREFIX)) {
        body.remove_prefix(TOKEN_PREFIX.size());
    }
    if (body.empty() || body.front() != version) {
        return std::nullopt;
    }
    body.remove_prefix(1);
    // A final group of 1, 3 or 6 characters is no padded length
    const size_t remainder = body.size() % BASE32_GROUP_CHARS;
    if (remainder == 1 || remainder == 3 || remainder == 6) {
        return std::nullopt;
    }
    // Reserved whole, so no reallocation leaves a copy of the payload behind
    std::vector<uint8_t> payload;
    payload.reserve(body.size() * BASE32_BITS_PER_CHAR / 8);
    uint64_t bits = 0;
    int pending = 0;
    for (char c : body) {
        if (c == '9') {
            c = '2';
        }
        const size_t value = BASE32_ALPHABET.find(c);
        if (value == std::string_view::npos) {
            secure_zero(payload.data(), payload.size());
            secure_zero(text.data(), text.size());
            return std::nullopt;
        }
        bits = (bits << BASE32_BITS_PER_CHAR) | value;
        pending += BASE32_BITS_PER_CHAR;
        if (pending >= 8) {
            pending -= 8;
            payload.push_back(static_cast<uint8_t>((bits >> pending) & 0xFF));
        }
    }
    secure_zero(&bits, sizeof(bits));
    secure_zero(text.data(), text.size());
    if (payload.size() < min_payload) {
        secure_zero(payload.data(), payload.size());
        return std::nullopt;
    }
    return payload;
}

}  // namespace

PairingPskToken::~PairingPskToken() {
    secure_zero(this->pairing_psk.data(), this->pairing_psk.size());
}

PairingCodeToken::~PairingCodeToken() {
    secure_zero(this->code.data(), this->code.size());
}

std::string format_pairing_token(const PublicKey& client_key, const Psk& pairing_psk) {
    std::array<uint8_t, PSK_PAYLOAD_SIZE> payload{};
    std::copy(client_key.begin(), client_key.end(), payload.begin());
    std::copy(pairing_psk.begin(), pairing_psk.end(), payload.begin() + X25519_KEY_SIZE);
    std::string token = format_token(PAIRING_PSK_TOKEN_VERSION, payload.data(), payload.size());
    secure_zero(payload.data(), payload.size());
    return token;
}

std::string format_pairing_code_token(const std::array<uint8_t, QR_PAIRING_CODE_SIZE>& code) {
    return format_token(PAIRING_CODE_TOKEN_VERSION, code.data(), code.size());
}

std::optional<PairingPskToken> decode_pairing_psk_token(std::string_view input) {
    std::optional<std::vector<uint8_t>> payload =
        decode_token(input, PAIRING_PSK_TOKEN_VERSION, PSK_PAYLOAD_SIZE);
    if (!payload) {
        return std::nullopt;
    }
    PairingPskToken token;
    std::copy_n(payload->begin(), X25519_KEY_SIZE, token.client_key.begin());
    std::copy_n(payload->begin() + X25519_KEY_SIZE, PSK_SIZE, token.pairing_psk.begin());
    secure_zero(payload->data(), payload->size());
    return token;
}

std::optional<PairingCodeToken> decode_pairing_code_token(std::string_view input) {
    std::optional<std::vector<uint8_t>> payload =
        decode_token(input, PAIRING_CODE_TOKEN_VERSION, QR_PAIRING_CODE_SIZE);
    if (!payload) {
        return std::nullopt;
    }
    PairingCodeToken token;
    std::copy_n(payload->begin(), QR_PAIRING_CODE_SIZE, token.code.begin());
    secure_zero(payload->data(), payload->size());
    return token;
}

}  // namespace sendspin::server
