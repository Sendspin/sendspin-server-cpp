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

#include "crypto/cpace.h"

#include "crypto/crypto.h"
#include "crypto/field25519.h"

#include <cassert>
#include <cstring>

// noise-c's X25519 (src/crypto/x25519/x25519.c), the one its DH state calls: one Montgomery ladder
// with the scalar clamped when clamp is set
extern "C" int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t x1[32], int clamp);

namespace sendspin::server {

namespace {

/// The seven value bits each byte of the base-128 length prefix carries
constexpr size_t LENGTH_PREFIX_DIGIT_MASK = 0x7F;
constexpr size_t LENGTH_PREFIX_CONTINUE = 0x80;
constexpr size_t LENGTH_PREFIX_LIMIT = 128;
/// The unused top bit of a field element's most significant byte
constexpr uint8_t FIELD_TOP_BYTE_MASK = 0x7F;

std::vector<uint8_t> lv_pair(const uint8_t* share, const std::vector<uint8_t>& ad) {
    return cpace_lv_cat({
        {share, CPACE_SHARE_SIZE},
        {ad.data(), ad.size()},
    });
}

}  // namespace

std::vector<uint8_t> cpace_prepend_len(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    size_t length = len;
    do {
        if (length < LENGTH_PREFIX_LIMIT) {
            out.push_back(static_cast<uint8_t>(length));
        } else {
            out.push_back(
                static_cast<uint8_t>((length & LENGTH_PREFIX_DIGIT_MASK) | LENGTH_PREFIX_CONTINUE));
        }
        length >>= 7;
    } while (length > 0);
    if (len > 0) {
        out.insert(out.end(), data, data + len);
    }
    return out;
}

std::vector<uint8_t> cpace_lv_cat(std::initializer_list<std::pair<const uint8_t*, size_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& [data, len] : parts) {
        const std::vector<uint8_t> lv = cpace_prepend_len(data, len);
        out.insert(out.end(), lv.begin(), lv.end());
    }
    return out;
}

std::vector<uint8_t> cpace_generator_string(const uint8_t* prs, size_t prs_len, const uint8_t* ci,
                                            size_t ci_len, const uint8_t* sid, size_t sid_len) {
    const auto* dsi = reinterpret_cast<const uint8_t*>(CPACE_DSI.data());
    const size_t lv_dsi = cpace_prepend_len(dsi, CPACE_DSI.size()).size();
    const size_t lv_prs = cpace_prepend_len(prs, prs_len).size();
    // len_zpad = max(0, s_in_bytes - 1 - len(prepend_len(PRS)) - len(prepend_len(DSI)))
    const size_t used = lv_dsi + lv_prs + 1;
    const size_t len_zpad = CPACE_SHA512_BLOCK > used ? CPACE_SHA512_BLOCK - used : 0;
    const std::vector<uint8_t> zeros(len_zpad, 0);
    return cpace_lv_cat({
        {dsi, CPACE_DSI.size()},
        {prs, prs_len},
        {zeros.data(), zeros.size()},
        {ci, ci_len},
        {sid, sid_len},
    });
}

std::array<uint8_t, CPACE_FIELD_BYTES> cpace_decode_u(const uint8_t* value, size_t len) {
    assert(len == CPACE_FIELD_BYTES);
    (void)len;
    std::array<uint8_t, CPACE_FIELD_BYTES> u{};
    std::memcpy(u.data(), value, CPACE_FIELD_BYTES);
    u[CPACE_FIELD_BYTES - 1] &= FIELD_TOP_BYTE_MASK;
    return u;
}

// draft-irtf-cfrg-cpace-21 Elligator2 for Curve25519 (A = 486662, Z = 2):
//   v = -A / (1 + Z * r^2), eps = legendre(v^3 + A * v^2 + v), x = eps * v - (1 - eps) * A / 2
std::array<uint8_t, CPACE_FIELD_BYTES> cpace_elligator2(
    const std::array<uint8_t, CPACE_FIELD_BYTES>& r_le) {
    using namespace field25519;
    constexpr uint64_t A_CONST = 486662;
    constexpr uint64_t Z_CONST = 2;
    // A is even, so A / 2 needs no inverse
    constexpr Fp A_HALF = {{A_CONST / 2, 0, 0, 0}};
    const Fp one = {{1, 0, 0, 0}};
    const Fp zero = {{0, 0, 0, 0}};

    const Fp r = fp_reduce_full(fp_from_le(r_le.data()));
    const Fp denom = fp_add(one, fp_scale(fp_mul(r, r), Z_CONST));
    const Fp v = fp_sub(zero, fp_scale(fp_inv(denom), A_CONST));
    const Fp v2 = fp_mul(v, v);
    const Fp poly = fp_add(fp_add(fp_mul(v2, v), fp_scale(v2, A_CONST)), v);
    // 0, 1 or p - 1
    const Fp eps = fp_legendre_pow(poly);
    const Fp x = fp_sub(fp_mul(eps, v), fp_mul(fp_sub(one, eps), A_HALF));
    return fp_to_le(x);
}

std::array<uint8_t, CPACE_FIELD_BYTES> cpace_calculate_generator(const uint8_t* prs, size_t prs_len,
                                                                 const uint8_t* ci, size_t ci_len,
                                                                 const uint8_t* sid,
                                                                 size_t sid_len) {
    std::vector<uint8_t> generator = cpace_generator_string(prs, prs_len, ci, ci_len, sid, sid_len);
    std::array<uint8_t, SHA512_DIGEST_SIZE> hash =
        sha512_oneshot(generator.data(), generator.size());
    // The generator string and what follows from it are derived from the code
    std::array<uint8_t, CPACE_FIELD_BYTES> u = cpace_decode_u(hash.data(), CPACE_FIELD_BYTES);
    secure_zero(generator.data(), generator.size());
    secure_zero(hash.data(), hash.size());
    const std::array<uint8_t, CPACE_FIELD_BYTES> point = cpace_elligator2(u);
    secure_zero(u.data(), u.size());
    return point;
}

bool x25519_scalar_mult(const uint8_t scalar[X25519_KEY_SIZE], const uint8_t point[X25519_KEY_SIZE],
                        uint8_t out[X25519_KEY_SIZE]) {
    // One ladder, as noise_dhstate_calculate() runs it; setting a DH state's private key would
    // first compute its public key, a second ladder CPace never uses. Its return value is not an
    // error: noise-c ignores it too, and CPace rejects an all-zero result itself.
    // RFC 7748 decodeUCoordinate: the top bit of the u-coordinate is masked
    std::array<uint8_t, X25519_KEY_SIZE> u{};
    std::memcpy(u.data(), point, X25519_KEY_SIZE);
    u[X25519_KEY_SIZE - 1] &= FIELD_TOP_BYTE_MASK;
    x25519(out, scalar, u.data(), 1);
    return true;
}

CPace::~CPace() {
    secure_zero(this->scalar_.data(), this->scalar_.size());
    secure_zero(this->mac_key_.data(), this->mac_key_.size());
    secure_zero(this->isk_.data(), this->isk_.size());
}

bool CPace::start(CPaceRole role, const std::vector<uint8_t>& prs, const std::vector<uint8_t>& sid,
                  const std::vector<uint8_t>& ci, const std::vector<uint8_t>& ad,
                  const std::vector<uint8_t>& peer_ad) {
    this->role_ = role;
    this->sid_ = sid;
    this->ad_ = ad;
    this->peer_ad_ = peer_ad;
    this->started_ = false;
    this->derived_ = false;
    random_bytes(this->scalar_.data(), this->scalar_.size());
    const std::array<uint8_t, CPACE_FIELD_BYTES> generator = cpace_calculate_generator(
        prs.data(), prs.size(), ci.data(), ci.size(), sid.data(), sid.size());
    if (!x25519_scalar_mult(this->scalar_.data(), generator.data(), this->public_share_.data())) {
        return false;
    }
    this->started_ = true;
    return true;
}

bool CPace::derive(const uint8_t* peer_share, size_t peer_share_len) {
    // Once per start(): a second derive would key the run again
    if (!this->started_ || this->derived_ || peer_share_len != CPACE_SHARE_SIZE) {
        return false;
    }
    std::array<uint8_t, CPACE_SHARE_SIZE> shared{};
    if (!x25519_scalar_mult(this->scalar_.data(), peer_share, shared.data())) {
        return false;
    }
    // draft-irtf-cfrg-cpace-21 `scalar_mult_vfy`: a low-order peer share yields the zero point
    const std::array<uint8_t, CPACE_SHARE_SIZE> zero{};
    if (constant_time_equal(shared.data(), zero.data(), CPACE_SHARE_SIZE)) {
        return false;
    }

    if (this->role_ == CPaceRole::INITIATOR) {
        this->initiator_share_ = this->public_share_;
        this->initiator_ad_ = this->ad_;
        std::memcpy(this->responder_share_.data(), peer_share, CPACE_SHARE_SIZE);
        this->responder_ad_ = this->peer_ad_;
    } else {
        std::memcpy(this->initiator_share_.data(), peer_share, CPACE_SHARE_SIZE);
        this->initiator_ad_ = this->peer_ad_;
        this->responder_share_ = this->public_share_;
        this->responder_ad_ = this->ad_;
    }

    // ISK = SHA-512(lv_cat(DSI_ISK, sid, K) || lv_cat(Ya, ADa) || lv_cat(Yb, ADb))
    const auto* dsi_isk = reinterpret_cast<const uint8_t*>(CPACE_DSI_ISK.data());
    std::vector<uint8_t> prefix = cpace_lv_cat({
        {dsi_isk, CPACE_DSI_ISK.size()},
        {this->sid_.data(), this->sid_.size()},
        {shared.data(), shared.size()},
    });
    const std::vector<uint8_t> initiator =
        lv_pair(this->initiator_share_.data(), this->initiator_ad_);
    const std::vector<uint8_t> responder =
        lv_pair(this->responder_share_.data(), this->responder_ad_);
    Sha512 isk_hash;
    isk_hash.update(prefix.data(), prefix.size());
    isk_hash.update(initiator.data(), initiator.size());
    isk_hash.update(responder.data(), responder.size());
    this->isk_ = isk_hash.finalize();
    secure_zero(prefix.data(), prefix.size());
    secure_zero(shared.data(), shared.size());

    // mac_key = SHA-512("CPaceMac" || sid || ISK)
    const auto* mac_label = reinterpret_cast<const uint8_t*>(CPACE_MAC_LABEL.data());
    Sha512 mac_hash;
    mac_hash.update(mac_label, CPACE_MAC_LABEL.size());
    mac_hash.update(this->sid_.data(), this->sid_.size());
    mac_hash.update(this->isk_.data(), this->isk_.size());
    this->mac_key_ = mac_hash.finalize();
    this->derived_ = true;
    return true;
}

// Ta authenticates (Ya, ADa), Tb (Yb, ADb)
std::array<uint8_t, CPACE_TAG_SIZE> CPace::compute_mac(bool own) const {
    const bool initiator = own == (this->role_ == CPaceRole::INITIATOR);
    const uint8_t* share =
        initiator ? this->initiator_share_.data() : this->responder_share_.data();
    const std::vector<uint8_t>& ad = initiator ? this->initiator_ad_ : this->responder_ad_;
    const std::vector<uint8_t> lv = lv_pair(share, ad);
    return hmac_sha512(this->mac_key_.data(), this->mac_key_.size(), lv.data(), lv.size());
}

std::optional<std::array<uint8_t, CPACE_TAG_SIZE>> CPace::tag() const {
    if (!this->derived_) {
        return std::nullopt;
    }
    return this->compute_mac(true);
}

bool CPace::verify(const uint8_t* peer_tag, size_t peer_tag_len) const {
    if (!this->derived_ || peer_tag_len != CPACE_TAG_SIZE) {
        return false;
    }
    const std::array<uint8_t, CPACE_TAG_SIZE> expected = this->compute_mac(false);
    return constant_time_equal(expected.data(), peer_tag, CPACE_TAG_SIZE);
}

std::optional<std::array<uint8_t, CPACE_ISK_SIZE>> CPace::isk() const {
    if (!this->derived_) {
        return std::nullopt;
    }
    return this->isk_;
}

}  // namespace sendspin::server
