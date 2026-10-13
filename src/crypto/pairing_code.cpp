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

#include "crypto/pairing_code.h"

#include "crypto/crypto.h"

#include <cstring>

namespace sendspin::server {

namespace {

constexpr uint64_t DECIMAL_BASE = 10;
constexpr uint64_t BYTE_BASE = 256;

}  // namespace

std::array<uint8_t, PAIRING_NONCE_SIZE> pairing_generate_nonce() {
    std::array<uint8_t, PAIRING_NONCE_SIZE> nonce{};
    random_bytes(nonce.data(), nonce.size());
    return nonce;
}

std::optional<std::array<uint8_t, PAIRING_COMMIT_SIZE>> pairing_code_commit(const uint8_t* nonce,
                                                                            size_t nonce_len) {
    if (nonce_len != PAIRING_NONCE_SIZE) {
        return std::nullopt;
    }
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(PAIRING_COMMIT_LABEL.data()),
                PAIRING_COMMIT_LABEL.size());
    hash.update(nonce, nonce_len);
    const std::array<uint8_t, PAIRING_COMMIT_SIZE> commitment = hash.finalize();
    // A failed hash finalizes to zeros, which would bind nothing
    if (!hash.ok()) {
        return std::nullopt;
    }
    return commitment;
}

bool pairing_code_verify_commit(const uint8_t* nonce, size_t nonce_len, const uint8_t* commitment,
                                size_t commitment_len) {
    if (commitment_len != PAIRING_COMMIT_SIZE) {
        return false;
    }
    const std::optional<std::array<uint8_t, PAIRING_COMMIT_SIZE>> computed =
        pairing_code_commit(nonce, nonce_len);
    return computed && constant_time_equal(computed->data(), commitment, PAIRING_COMMIT_SIZE);
}

std::optional<std::array<uint8_t, PAIRING_DIGEST_SIZE>> pairing_code_digest(
    const uint8_t* handshake_hash, size_t hash_len, const uint8_t* nonce_a, size_t nonce_a_len,
    const uint8_t* nonce_b, size_t nonce_b_len) {
    if (hash_len != SHA256_DIGEST_SIZE || nonce_a_len != PAIRING_NONCE_SIZE ||
        nonce_b_len != PAIRING_NONCE_SIZE) {
        return std::nullopt;
    }
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(PAIRING_CODE_DERIVE_LABEL.data()),
                PAIRING_CODE_DERIVE_LABEL.size());
    hash.update(handshake_hash, hash_len);
    hash.update(nonce_a, nonce_a_len);
    hash.update(nonce_b, nonce_b_len);
    const std::array<uint8_t, PAIRING_DIGEST_SIZE> digest = hash.finalize();
    // A failed hash finalizes to zeros, a predictable code
    if (!hash.ok()) {
        return std::nullopt;
    }
    return digest;
}

std::string pairing_code_digits(const std::array<uint8_t, PAIRING_DIGEST_SIZE>& digest) {
    uint64_t modulus = 1;
    for (size_t i = 0; i < DYNAMIC_PAIRING_CODE_DIGITS; ++i) {
        modulus *= DECIMAL_BASE;
    }
    // Horner's method over the big-endian digest; the accumulator stays below modulus * 256
    uint64_t acc = 0;
    for (const uint8_t byte : digest) {
        acc = (acc * BYTE_BASE + byte) % modulus;
    }
    std::string code(DYNAMIC_PAIRING_CODE_DIGITS, '0');
    for (size_t i = DYNAMIC_PAIRING_CODE_DIGITS; i > 0; --i) {
        code[i - 1] = static_cast<char>('0' + (acc % DECIMAL_BASE));
        acc /= DECIMAL_BASE;
    }
    return code;
}

std::array<uint8_t, QR_PAIRING_CODE_SIZE> pairing_code_qr_bytes(
    const std::array<uint8_t, PAIRING_DIGEST_SIZE>& digest) {
    std::array<uint8_t, QR_PAIRING_CODE_SIZE> code{};
    std::memcpy(code.data(), digest.data(), QR_PAIRING_CODE_SIZE);
    return code;
}

}  // namespace sendspin::server
