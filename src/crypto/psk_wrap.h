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

/// @file psk_wrap.h
/// @brief Wrapping (spec pairing.md "Wrapping"): a 32-byte value sealed under
/// K_wrap = SHA-256(label || sid || ISK) with the AEAD of the connection's cipher suite, a
/// 12-byte all-zero nonce and empty associated data. A code-based pairing carries the new
/// long-term PSK as `wrapped_psk` and the opening of commit_B as `wrapped_nonce_B` this way.

#pragma once

#include "crypto/cpace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sendspin::server {

constexpr size_t WRAPPED_PLAINTEXT_SIZE = 32;
/// @brief The value and the 16-byte AEAD tag
constexpr size_t WRAPPED_VALUE_SIZE = WRAPPED_PLAINTEXT_SIZE + 16;

using WrappedValue = std::array<uint8_t, WRAPPED_VALUE_SIZE>;
using WrapKey = std::array<uint8_t, 32>;

/// @brief The `wrapped_psk` label; distinct from the `wrapped_nonce_B` one, so the two fields of
/// a run never share a key under the all-zero nonce
constexpr std::string_view PSK_WRAP_LABEL{"sendspin-pair-psk-wrap-v1"};
constexpr std::string_view NONCE_WRAP_LABEL{"sendspin-pair-nonce-wrap-v1"};

/// @brief noise-c's names of the suites' AEADs
constexpr const char* AEAD_CHACHAPOLY = "ChaChaPoly";
constexpr const char* AEAD_AESGCM = "AESGCM";

/// @brief K_wrap = SHA-256(label || sid || isk); nullopt when SHA-256 fails, never a zero key.
/// The caller wipes it.
std::optional<WrapKey> derive_wrap_key(std::string_view label, const std::vector<uint8_t>& sid,
                                       const std::array<uint8_t, CPACE_ISK_SIZE>& isk);

/// @brief Seals value under K_wrap with the AEAD noise-c names cipher_name
/// @return nullopt when the key or the cipher fails
std::optional<WrappedValue> wrap_value(std::string_view label, const char* cipher_name,
                                       const std::vector<uint8_t>& sid,
                                       const std::array<uint8_t, CPACE_ISK_SIZE>& isk,
                                       const std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>& value);

/// @brief The value wrap_value() sealed; the caller wipes it
/// @return nullopt when the field fails to authenticate, or the key or the cipher fails
std::optional<std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>> unwrap_value(
    std::string_view label, const char* cipher_name, const std::vector<uint8_t>& sid,
    const std::array<uint8_t, CPACE_ISK_SIZE>& isk, const WrappedValue& wrapped);

}  // namespace sendspin::server
