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

/// @file cpace.h
/// @brief CPace-X25519-SHA512 with explicit mutual confirmation (spec pairing.md "PAKE",
/// draft-irtf-cfrg-cpace-21). The server is role A (INITIATOR), the client role B (RESPONDER);
/// both roles are implemented, so either side can be tested against the other.

#pragma once

#include "crypto/keys.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief draft-irtf-cfrg-cpace-21 domain separation strings for the generator, the ISK and the
/// MAC key
constexpr std::string_view CPACE_DSI{"CPace255"};
constexpr std::string_view CPACE_DSI_ISK{"CPace255_ISK"};
constexpr std::string_view CPACE_MAC_LABEL{"CPaceMac"};

/// @brief The SHA-512 block size, which pads the generator string
constexpr size_t CPACE_SHA512_BLOCK = 128;
constexpr size_t CPACE_FIELD_BYTES = 32;
/// @brief A public share (`Ya`, `Yb`)
constexpr size_t CPACE_SHARE_SIZE = 32;
/// @brief A confirmation tag (`Ta`, `Tb`), HMAC-SHA-512
constexpr size_t CPACE_TAG_SIZE = 64;
/// @brief The intermediate session key (ISK), which also feeds K_wrap (spec pairing.md
/// "Wrapping")
constexpr size_t CPACE_ISK_SIZE = 64;

enum class CPaceRole : uint8_t {
    INITIATOR = 0,  ///< Role A: sends the first share
    RESPONDER = 1,  ///< Role B: sends the second share
};

/// @brief One side of a CPace run: start() samples the scalar and gives public_share(), derive()
/// takes the peer's share, then tag() and verify() exchange confirmation. Wipes its scalar, MAC
/// key and ISK when destroyed.
class CPace {
public:
    CPace() = default;
    ~CPace();
    CPace(const CPace&) = delete;
    CPace& operator=(const CPace&) = delete;

    /// @param prs the password-related string: the pairing code's bytes
    /// @param ci the channel identifier, empty for Sendspin
    /// @param ad this side's associated data (ADa "server" for role A)
    /// @param peer_ad the peer's associated data
    /// @return false when the generator or the share cannot be computed
    bool start(CPaceRole role, const std::vector<uint8_t>& prs, const std::vector<uint8_t>& sid,
               const std::vector<uint8_t>& ci, const std::vector<uint8_t>& ad,
               const std::vector<uint8_t>& peer_ad);

    /// @brief This side's share (`Ya` for A, `Yb` for B), after a successful start()
    [[nodiscard]] const std::array<uint8_t, CPACE_SHARE_SIZE>& public_share() const {
        return this->public_share_;
    }

    /// @brief Takes the peer's share and derives the ISK and the MAC key; once per start()
    /// @return false before start(), on a second call, or for a share of the wrong size or one
    /// that yields the all-zero point (a low-order point)
    bool derive(const uint8_t* peer_share, size_t peer_share_len);

    /// @brief This side's tag (`Ta` for A, `Tb` for B); nullopt before derive()
    [[nodiscard]] std::optional<std::array<uint8_t, CPACE_TAG_SIZE>> tag() const;

    /// @brief Whether peer_tag is the peer's tag, compared in constant time; false before
    /// derive()
    [[nodiscard]] bool verify(const uint8_t* peer_tag, size_t peer_tag_len) const;

    /// @brief The ISK; nullopt before derive(). The caller wipes its copy.
    [[nodiscard]] std::optional<std::array<uint8_t, CPACE_ISK_SIZE>> isk() const;

    /// @brief The sid start() was given, which K_wrap hashes with the ISK
    [[nodiscard]] const std::vector<uint8_t>& sid() const {
        return this->sid_;
    }

private:
    /// @brief The HMAC-SHA-512 tag over one side's (share, ad): own, or the peer's
    std::array<uint8_t, CPACE_TAG_SIZE> compute_mac(bool own) const;

    std::vector<uint8_t> ad_;
    std::vector<uint8_t> peer_ad_;
    std::vector<uint8_t> sid_;
    std::array<uint8_t, CPACE_FIELD_BYTES> scalar_{};
    std::array<uint8_t, CPACE_SHARE_SIZE> public_share_{};
    /// From derive() on
    std::vector<uint8_t> initiator_ad_;
    std::vector<uint8_t> responder_ad_;
    std::array<uint8_t, CPACE_SHARE_SIZE> initiator_share_{};
    std::array<uint8_t, CPACE_SHARE_SIZE> responder_share_{};
    std::array<uint8_t, CPACE_ISK_SIZE> isk_{};
    std::array<uint8_t, CPACE_TAG_SIZE> mac_key_{};
    CPaceRole role_{CPaceRole::RESPONDER};
    bool started_{false};
    bool derived_{false};
};

/// @brief draft-irtf-cfrg-cpace-21 `prepend_len`: a little-endian base-128 length, then the data
std::vector<uint8_t> cpace_prepend_len(const uint8_t* data, size_t len);

/// @brief draft-irtf-cfrg-cpace-21 `lv_cat` over (data, length) parts
std::vector<uint8_t> cpace_lv_cat(std::initializer_list<std::pair<const uint8_t*, size_t>> parts);

/// @brief draft-irtf-cfrg-cpace-21 `generator_string`
std::vector<uint8_t> cpace_generator_string(const uint8_t* prs, size_t prs_len, const uint8_t* ci,
                                            size_t ci_len, const uint8_t* sid, size_t sid_len);

/// @brief The Elligator2 map of r (little-endian, reduced mod p) to an x-coordinate on
/// Curve25519, little-endian
std::array<uint8_t, CPACE_FIELD_BYTES> cpace_elligator2(
    const std::array<uint8_t, CPACE_FIELD_BYTES>& r_le);

/// @brief A 32-byte little-endian value with its top bit cleared (RFC 7748 decoding)
std::array<uint8_t, CPACE_FIELD_BYTES> cpace_decode_u(const uint8_t* value, size_t len);

/// @brief draft-irtf-cfrg-cpace-21 `calculate_generator`
std::array<uint8_t, CPACE_FIELD_BYTES> cpace_calculate_generator(const uint8_t* prs, size_t prs_len,
                                                                 const uint8_t* ci, size_t ci_len,
                                                                 const uint8_t* sid,
                                                                 size_t sid_len);

/// @brief X25519 (RFC 7748: scalar clamped, the point's top bit masked) of scalar and point into
/// out, one Montgomery ladder
/// @return true: kept for the callers' shape; an all-zero result is the caller's to reject
bool x25519_scalar_mult(const uint8_t scalar[X25519_KEY_SIZE], const uint8_t point[X25519_KEY_SIZE],
                        uint8_t out[X25519_KEY_SIZE]);

}  // namespace sendspin::server
