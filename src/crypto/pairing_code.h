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

/// @file pairing_code.h
/// @brief Pairing-code derivation and the commitment to nonce_B (spec pairing.md "Dynamic Pairing
/// Code Flow")

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief nonce_A and nonce_B
constexpr size_t PAIRING_NONCE_SIZE = 32;

/// @brief commit_B, a SHA-256 digest
constexpr size_t PAIRING_COMMIT_SIZE = 32;

constexpr size_t PAIRING_DIGEST_SIZE = 32;

constexpr std::string_view PAIRING_CODE_DERIVE_LABEL{"sendspin-pairing-code-derive-v1"};
constexpr std::string_view PAIRING_COMMIT_LABEL{"sendspin-pair-commit-v1"};

/// @brief A dynamic pairing code in the `digits` format
constexpr size_t DYNAMIC_PAIRING_CODE_DIGITS = 6;

/// @brief A dynamic pairing code in the `qr_code` format: the digest's first bytes
constexpr size_t QR_PAIRING_CODE_SIZE = 24;

/// @brief A static pairing code (spec pairing.md "Static Pairing Code Flow")
constexpr size_t STATIC_PAIRING_CODE_DIGITS = 8;

/// @brief 32 bytes from the CSPRNG
std::array<uint8_t, PAIRING_NONCE_SIZE> pairing_generate_nonce();

/// @brief commit_B = SHA-256("sendspin-pair-commit-v1" || nonce)
/// @return nullopt for a nonce of another length, or when SHA-256 fails
std::optional<std::array<uint8_t, PAIRING_COMMIT_SIZE>> pairing_code_commit(const uint8_t* nonce,
                                                                            size_t nonce_len);

/// @brief Whether commitment is commit_B of nonce, compared in constant time
bool pairing_code_verify_commit(const uint8_t* nonce, size_t nonce_len, const uint8_t* commitment,
                                size_t commitment_len);

/// @brief SHA-256("sendspin-pairing-code-derive-v1" || h || nonce_a || nonce_b)
/// @return nullopt when an input has the wrong length or SHA-256 fails
std::optional<std::array<uint8_t, PAIRING_DIGEST_SIZE>> pairing_code_digest(
    const uint8_t* handshake_hash, size_t hash_len, const uint8_t* nonce_a, size_t nonce_a_len,
    const uint8_t* nonce_b, size_t nonce_b_len);

/// @brief The `digits` code: the digest as a big-endian integer modulo 10^6, as six digits
std::string pairing_code_digits(const std::array<uint8_t, PAIRING_DIGEST_SIZE>& digest);

/// @brief The `qr_code` code: the digest's first QR_PAIRING_CODE_SIZE bytes
std::array<uint8_t, QR_PAIRING_CODE_SIZE> pairing_code_qr_bytes(
    const std::array<uint8_t, PAIRING_DIGEST_SIZE>& digest);

/// @brief The CPace PRS of a decimal code, static or dynamic: its ASCII digits (spec pairing.md
/// "PAKE")
inline std::vector<uint8_t> pairing_code_digits_prs(std::string_view code) {
    return {code.begin(), code.end()};
}

}  // namespace sendspin::server
