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

/// @file pairing_token.h
/// @brief Pairing tokens (spec pairing.md "Pairing Token"): "SP:", a version character and the
/// base32 payload with its padding stripped and every '2' written as '9'. Version 0 carries a
/// client key and its pairing PSK, version 1 a dynamic pairing code in the `qr_code` format.

#pragma once

#include "crypto/keys.h"
#include "crypto/pairing_code.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin::server {

constexpr char PAIRING_PSK_TOKEN_VERSION = '0';
constexpr char PAIRING_CODE_TOKEN_VERSION = '1';

/// @brief A version-0 token's payload. Wipes its PSK when destroyed.
struct PairingPskToken {
    PublicKey client_key{};
    Psk pairing_psk{};

    PairingPskToken() = default;
    ~PairingPskToken();
    PairingPskToken(const PairingPskToken&) = default;
    PairingPskToken& operator=(const PairingPskToken&) = default;
    PairingPskToken(PairingPskToken&&) noexcept = default;
    PairingPskToken& operator=(PairingPskToken&&) noexcept = default;
};

/// @brief A version-1 token's payload, the `qr_code` pairing code. Wipes itself when destroyed.
struct PairingCodeToken {
    std::array<uint8_t, QR_PAIRING_CODE_SIZE> code{};

    PairingCodeToken() = default;
    ~PairingCodeToken();
    PairingCodeToken(const PairingCodeToken&) = default;
    PairingCodeToken& operator=(const PairingCodeToken&) = default;
    PairingCodeToken(PairingCodeToken&&) noexcept = default;
    PairingCodeToken& operator=(PairingCodeToken&&) noexcept = default;
};

/// @brief The version-0 token of client_key and pairing_psk (107 characters)
std::string format_pairing_token(const PublicKey& client_key, const Psk& pairing_psk);

/// @brief The version-1 token of a `qr_code` pairing code (43 characters)
std::string format_pairing_code_token(const std::array<uint8_t, QR_PAIRING_CODE_SIZE>& code);

/// @brief Decodes operator input as a version-0 token, leniently (spec pairing.md "Pairing
/// Token"): surrounding whitespace trimmed, case ignored, the "SP:" prefix optional, '9' read as
/// '2', padding restored. Payload bytes past the 64 version 0 defines are ignored.
/// @return nullopt for another version, a character outside the alphabet, a length no padding
/// gives, or a payload shorter than 64 bytes
std::optional<PairingPskToken> decode_pairing_psk_token(std::string_view input);

/// @brief decode_pairing_psk_token() for a version-1 token: the first 24 payload bytes
std::optional<PairingCodeToken> decode_pairing_code_token(std::string_view input);

}  // namespace sendspin::server
