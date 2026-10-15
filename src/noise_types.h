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

/// @file noise_types.h
/// @brief The names a Noise handshake is described by: its cipher suites, its PSK categories, the
/// PSK it references and its handshake hash

#pragma once

#include "crypto/keys.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace sendspin::server {

/// @brief The cipher suites of spec connection.md "Cipher Suites"
enum class CipherSuite : uint8_t {
    CHACHAPOLY_SHA256,
    AESGCM_SHA256,
};

/// @brief The suite named by a client/init `suite` value, or nullopt for an unknown name
std::optional<CipherSuite> cipher_suite_from_wire(std::string_view name);

/// @brief The suite's `client/init` name, such as "25519_ChaChaPoly_SHA256"
std::string_view to_wire(CipherSuite suite);

/// @brief The categories a msg-1 payload's `psk_category` declares (messaging.md
/// "noise/handshake")
enum class PskCategory : uint8_t {
    LONG_TERM,
    PAIRING,
    SENTINEL,
};

/// @brief The `psk_category` code: "lt", "pr" or "sn"
std::string_view to_wire(PskCategory category);

/// @brief A PSK a handshake references and the category message 1 declares it under (spec
/// connection.md "Pre-Shared Key"). Every copy wipes its PSK when destroyed.
struct HandshakePsk {
    PskCategory category{PskCategory::SENTINEL};
    Psk psk{SENTINEL_PSK};

    HandshakePsk() = default;
    HandshakePsk(PskCategory category_in, const Psk& psk_in) : category(category_in), psk(psk_in) {}
    ~HandshakePsk();
    HandshakePsk(const HandshakePsk&) = default;
    HandshakePsk& operator=(const HandshakePsk&) = default;
    HandshakePsk(HandshakePsk&&) noexcept = default;
    HandshakePsk& operator=(HandshakePsk&&) noexcept = default;
};

/// @brief Size of the handshake hash `h` for the SHA-256 suites
constexpr size_t HANDSHAKE_HASH_SIZE = 32;

using HandshakeHash = std::array<uint8_t, HANDSHAKE_HASH_SIZE>;

}  // namespace sendspin::server
