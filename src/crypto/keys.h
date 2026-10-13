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

/// @file keys.h
/// @brief Static identities, peer ids, the Sentinel PSK and psk_id derivation (spec connection.md
/// "Identities" and "Pre-Shared Key")

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin::server {

constexpr size_t X25519_KEY_SIZE = 32;
constexpr size_t PSK_SIZE = 32;
/// @brief Length of a base64url peer id or psk_id: 32 bytes, unpadded
constexpr size_t PEER_ID_SIZE = 43;

using PublicKey = std::array<uint8_t, X25519_KEY_SIZE>;
using Psk = std::array<uint8_t, PSK_SIZE>;

/// @brief SHA-256("sendspin-sentinel-psk-v1"), a published constant (spec "Pre-Shared Key")
extern const Psk SENTINEL_PSK;

/// @brief psk_id_for(SENTINEL_PSK)
extern const std::string SENTINEL_PSK_ID;

/// @brief base64url(SHA-256("sendspin-psk-id-v1" || psk)). Aborts if SHA-256 fails.
std::string psk_id_for(const Psk& psk);

/// @brief Decodes a client_id or server_id to the X25519 public key it names
/// @return nullopt unless peer_id is exactly the canonical encoding of 32 bytes (43 characters,
/// unused trailing bits zero), so that one key has one id, and nullopt for a key of small order
/// (zero included), whose shared secrets are predictable
std::optional<PublicKey> public_key_from_peer_id(std::string_view peer_id);

/// @brief A static X25519 key pair. Its peer id is the server_id. Not copyable; a move wipes the
/// source's private key, and destruction wipes its own.
struct Identity {
    std::array<uint8_t, X25519_KEY_SIZE> private_bytes{};
    PublicKey public_bytes{};

    Identity() = default;
    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;
    Identity(Identity&& other) noexcept;
    Identity& operator=(Identity&& other) noexcept;

    /// @brief base64url(public_bytes)
    [[nodiscard]] std::string peer_id() const;

    /// @brief A new key pair from the CSPRNG; nullopt when noise-c cannot create the DH state
    static std::optional<Identity> generate();

    /// @brief The key pair of a stored private key; nullopt when noise-c rejects it
    static std::optional<Identity> from_private_bytes(
        const std::array<uint8_t, X25519_KEY_SIZE>& private_bytes);
};

}  // namespace sendspin::server
