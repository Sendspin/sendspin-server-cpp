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

#include "crypto/keys.h"

#include "crypto/base64url.h"
#include "crypto/crypto.h"

#include <algorithm>
#include <cstdlib>

namespace sendspin::server {

namespace {

constexpr std::string_view PSK_ID_LABEL{"sendspin-psk-id-v1"};
constexpr std::string_view SENTINEL_LABEL{"sendspin-sentinel-psk-v1"};

/// The X25519 points of small order, high bit cleared: a key exchange with one has an output
/// known in advance. The comparison ignores the high bit, as X25519 does.
constexpr std::array<PublicKey, 7> SMALL_ORDER_POINTS{{
    {0x00},
    {0x01},
    {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
     0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
     0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
    {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
     0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
     0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57},
    {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
}};

constexpr uint8_t HIGH_BIT = 0x80;

bool has_small_order(const PublicKey& key) {
    PublicKey masked = key;
    masked.back() &= static_cast<uint8_t>(~HIGH_BIT);
    return std::find(SMALL_ORDER_POINTS.begin(), SMALL_ORDER_POINTS.end(), masked) !=
           SMALL_ORDER_POINTS.end();
}

Psk sha256_or_abort(std::string_view prefix, const uint8_t* data, size_t len) {
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size());
    if (len > 0) {
        hash.update(data, len);
    }
    const Psk digest = hash.finalize();
    // A zero digest would collapse every derived value onto one public constant
    if (!hash.ok()) {
        abort();
    }
    return digest;
}

}  // namespace

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const Psk SENTINEL_PSK = sha256_or_abort(SENTINEL_LABEL, nullptr, 0);

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::string SENTINEL_PSK_ID = psk_id_for(SENTINEL_PSK);

std::string psk_id_for(const Psk& psk) {
    const Psk digest = sha256_or_abort(PSK_ID_LABEL, psk.data(), psk.size());
    return b64url_encode(digest.data(), digest.size());
}

std::optional<PublicKey> public_key_from_peer_id(std::string_view peer_id) {
    if (peer_id.size() != PEER_ID_SIZE) {
        return std::nullopt;
    }
    const std::optional<std::vector<uint8_t>> decoded = b64url_decode(peer_id);
    if (!decoded || decoded->size() != X25519_KEY_SIZE ||
        b64url_encode(decoded->data(), decoded->size()) != peer_id) {
        return std::nullopt;
    }
    PublicKey key{};
    std::copy(decoded->begin(), decoded->end(), key.begin());
    if (has_small_order(key)) {
        return std::nullopt;
    }
    return key;
}

Identity::~Identity() {
    secure_zero(this->private_bytes.data(), this->private_bytes.size());
}

Identity::Identity(Identity&& other) noexcept
    : private_bytes(other.private_bytes), public_bytes(other.public_bytes) {
    secure_zero(other.private_bytes.data(), other.private_bytes.size());
}

Identity& Identity::operator=(Identity&& other) noexcept {
    if (this != &other) {
        this->private_bytes = other.private_bytes;
        this->public_bytes = other.public_bytes;
        secure_zero(other.private_bytes.data(), other.private_bytes.size());
    }
    return *this;
}

std::string Identity::peer_id() const {
    return b64url_encode(this->public_bytes.data(), this->public_bytes.size());
}

std::optional<Identity> Identity::generate() {
    const DhState dh;
    if (!dh.valid() || noise_dhstate_generate_keypair(dh.get()) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    Identity identity{};
    if (noise_dhstate_get_keypair(dh.get(), identity.private_bytes.data(),
                                  identity.private_bytes.size(), identity.public_bytes.data(),
                                  identity.public_bytes.size()) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    return identity;
}

std::optional<Identity> Identity::from_private_bytes(
    const std::array<uint8_t, X25519_KEY_SIZE>& private_bytes) {
    const DhState dh;
    if (!dh.valid() ||
        noise_dhstate_set_keypair_private(dh.get(), private_bytes.data(), private_bytes.size()) !=
            NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    Identity identity{};
    identity.private_bytes = private_bytes;
    if (noise_dhstate_get_public_key(dh.get(), identity.public_bytes.data(),
                                     identity.public_bytes.size()) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    return identity;
}

}  // namespace sendspin::server
