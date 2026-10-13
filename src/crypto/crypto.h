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

/// @file crypto.h
/// @brief SHA-256, SHA-512 and HMAC-SHA-512, an X25519 DH-state wrapper, the CSPRNG, a
/// constant-time comparison and secret erasure

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <noise/protocol/constants.h>
#include <noise/protocol/dhstate.h>
#include <noise/protocol/hashstate.h>
#include <noise/protocol/randstate.h>
}

namespace sendspin::server {

/// @brief Overwrites a buffer with zeroes through a volatile pointer, so dead-store elimination
/// cannot remove the write
inline void secure_zero(void* p, size_t n) {
    volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        vp[i] = 0;
    }
}

constexpr size_t SHA256_DIGEST_SIZE = 32;

/// @brief Streaming SHA-256
class Sha256 {
public:
    Sha256() {
        if (noise_hashstate_new_by_name(&this->state_, "SHA256") != NOISE_ERROR_NONE) {
            this->state_ = nullptr;
        } else {
            noise_hashstate_reset(this->state_);
        }
    }

    ~Sha256() {
        if (this->state_ != nullptr) {
            noise_hashstate_free(this->state_);
        }
    }

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const uint8_t* data, size_t len) {
        if (this->state_ != nullptr &&
            noise_hashstate_update(this->state_, data, len) != NOISE_ERROR_NONE) {
            this->failed_ = true;
        }
    }

    /// @brief The digest; all zero when ok() is false afterwards
    std::array<uint8_t, SHA256_DIGEST_SIZE> finalize() {
        std::array<uint8_t, SHA256_DIGEST_SIZE> digest{};
        if (this->state_ != nullptr && !this->failed_ &&
            noise_hashstate_finalize(this->state_, digest.data(), digest.size()) !=
                NOISE_ERROR_NONE) {
            this->failed_ = true;
            digest.fill(0);
        }
        return digest;
    }

    /// @brief False when the state could not be created or an update or finalize failed
    [[nodiscard]] bool ok() const {
        return this->state_ != nullptr && !this->failed_;
    }

private:
    NoiseHashState* state_{nullptr};
    bool failed_{false};
};

/// @brief Owns a noise-c NoiseDHState
class DhState {
public:
    explicit DhState(const char* dh_name = "25519") {
        if (noise_dhstate_new_by_name(&this->state_, dh_name) != NOISE_ERROR_NONE) {
            this->state_ = nullptr;
        }
    }

    ~DhState() {
        if (this->state_ != nullptr) {
            noise_dhstate_free(this->state_);
        }
    }

    DhState(const DhState&) = delete;
    DhState& operator=(const DhState&) = delete;

    [[nodiscard]] bool valid() const {
        return this->state_ != nullptr;
    }

    [[nodiscard]] NoiseDHState* get() const {
        return this->state_;
    }

private:
    NoiseDHState* state_{nullptr};
};

constexpr size_t SHA512_DIGEST_SIZE = 64;

/// @brief Streaming SHA-512 (FIPS 180-4), self-contained since the noise-c build has no SHA-512.
/// Wipes its state when destroyed.
class Sha512 {
public:
    Sha512() {
        this->reset();
    }

    ~Sha512() {
        secure_zero(this->buf_, sizeof(this->buf_));
        secure_zero(this->h_, sizeof(this->h_));
    }

    Sha512(const Sha512&) = delete;
    Sha512& operator=(const Sha512&) = delete;

    void update(const uint8_t* data, size_t len) {
        this->total_len_ += len;
        while (len > 0) {
            size_t take = BLOCK - this->buf_len_;
            if (take > len) {
                take = len;
            }
            std::memcpy(this->buf_ + this->buf_len_, data, take);
            this->buf_len_ += take;
            data += take;
            len -= take;
            if (this->buf_len_ == BLOCK) {
                this->process_block(this->buf_);
                this->buf_len_ = 0;
            }
        }
    }

    /// @brief The digest; once per hash
    std::array<uint8_t, SHA512_DIGEST_SIZE> finalize() {
        // The 128-bit big-endian bit length, whose high 64 bits are zero for any input in memory
        const uint64_t bit_len = this->total_len_ * 8ULL;

        // 0x80, then zeros until the buffer is 112 mod 128 bytes
        this->buf_[this->buf_len_++] = 0x80;
        if (this->buf_len_ > BLOCK - 16) {
            std::memset(this->buf_ + this->buf_len_, 0, BLOCK - this->buf_len_);
            this->process_block(this->buf_);
            this->buf_len_ = 0;
        }
        std::memset(this->buf_ + this->buf_len_, 0, (BLOCK - 16) - this->buf_len_);
        this->buf_len_ = BLOCK - 16;

        uint8_t len_block[16] = {0};
        for (size_t i = 0; i < 8; ++i) {
            len_block[sizeof(len_block) - 1 - i] =
                static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF);
        }
        this->update(len_block, sizeof(len_block));

        std::array<uint8_t, SHA512_DIGEST_SIZE> digest{};
        for (int i = 0; i < 8; ++i) {
            for (int j = 0; j < 8; ++j) {
                digest[i * 8 + j] = static_cast<uint8_t>((this->h_[i] >> ((7 - j) * 8)) & 0xFF);
            }
        }
        return digest;
    }

private:
    static constexpr size_t BLOCK = 128;
    /// FIPS 180-4 section 6.4.2: 80 rounds over a message schedule kept as a rolling 16-word
    /// window
    static constexpr int ROUNDS = 80;
    static constexpr int SCHEDULE_WINDOW = 16;
    static constexpr int SCHEDULE_MASK = SCHEDULE_WINDOW - 1;

    void reset() {
        static const uint64_t IV[8] = {
            0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
            0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
            0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
        };
        for (int i = 0; i < 8; ++i) {
            this->h_[i] = IV[i];
        }
        this->buf_len_ = 0;
        this->total_len_ = 0;
    }

    static uint64_t rotr(uint64_t x, unsigned n) {
        return (x >> n) | (x << (64 - n));
    }

    void process_block(const uint8_t* p) {
        static const uint64_t K[ROUNDS] = {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
            0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
            0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
            0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
            0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
            0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
            0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
            0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
            0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
            0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
            0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
            0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
            0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
            0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
            0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
            0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
            0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
            0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
            0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
            0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
            0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
        };

        // w[t] depends only on w[t-2], w[t-7], w[t-15] and w[t-16], so a 16-word window
        // updated in place stands in for the 80-word schedule
        uint64_t w[SCHEDULE_WINDOW];
        for (int t = 0; t < SCHEDULE_WINDOW; ++t) {
            uint64_t v = 0;
            for (int j = 0; j < 8; ++j) {
                v = (v << 8) | p[t * 8 + j];
            }
            w[t] = v;
        }

        uint64_t a = this->h_[0], b = this->h_[1], c = this->h_[2], d = this->h_[3];
        uint64_t e = this->h_[4], f = this->h_[5], g = this->h_[6], h = this->h_[7];
        for (int t = 0; t < ROUNDS; ++t) {
            if (t >= SCHEDULE_WINDOW) {
                const uint64_t wm15 = w[(t - 15) & SCHEDULE_MASK];
                const uint64_t wm2 = w[(t - 2) & SCHEDULE_MASK];
                const uint64_t s0 = rotr(wm15, 1) ^ rotr(wm15, 8) ^ (wm15 >> 7);
                const uint64_t s1 = rotr(wm2, 19) ^ rotr(wm2, 61) ^ (wm2 >> 6);
                w[t & SCHEDULE_MASK] += s0 + w[(t - 7) & SCHEDULE_MASK] + s1;
            }
            const uint64_t big_s1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
            const uint64_t ch = (e & f) ^ (~e & g);
            const uint64_t t1 = h + big_s1 + ch + K[t] + w[t & SCHEDULE_MASK];
            const uint64_t big_s0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = big_s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        this->h_[0] += a;
        this->h_[1] += b;
        this->h_[2] += c;
        this->h_[3] += d;
        this->h_[4] += e;
        this->h_[5] += f;
        this->h_[6] += g;
        this->h_[7] += h;
    }

    uint64_t h_[8]{};
    /// Bytes hashed so far
    uint64_t total_len_{0};
    /// Bytes in buf_, a partial block
    size_t buf_len_{0};
    uint8_t buf_[BLOCK]{};
};

inline std::array<uint8_t, SHA512_DIGEST_SIZE> sha512_oneshot(const uint8_t* data, size_t len) {
    Sha512 h;
    h.update(data, len);
    return h.finalize();
}

/// @brief HMAC-SHA-512 (RFC 2104); a key longer than the 128-byte block is hashed first
inline std::array<uint8_t, SHA512_DIGEST_SIZE> hmac_sha512(const uint8_t* key, size_t key_len,
                                                           const uint8_t* data, size_t data_len) {
    constexpr size_t BLOCK = 128;
    constexpr uint8_t IPAD = 0x36;
    constexpr uint8_t OPAD = 0x5C;
    std::array<uint8_t, BLOCK> k{};
    if (key_len > BLOCK) {
        std::array<uint8_t, SHA512_DIGEST_SIZE> hashed = sha512_oneshot(key, key_len);
        std::copy(hashed.begin(), hashed.end(), k.begin());
        secure_zero(hashed.data(), hashed.size());
    } else {
        std::copy(key, key + key_len, k.begin());
    }
    std::array<uint8_t, BLOCK> pad{};
    std::array<uint8_t, SHA512_DIGEST_SIZE> inner_hash{};
    {
        for (size_t i = 0; i < BLOCK; ++i) {
            pad[i] = k[i] ^ IPAD;
        }
        Sha512 inner;
        inner.update(pad.data(), BLOCK);
        inner.update(data, data_len);
        inner_hash = inner.finalize();
    }
    for (size_t i = 0; i < BLOCK; ++i) {
        pad[i] = k[i] ^ OPAD;
    }
    Sha512 outer;
    outer.update(pad.data(), BLOCK);
    outer.update(inner_hash.data(), SHA512_DIGEST_SIZE);
    secure_zero(k.data(), k.size());
    secure_zero(pad.data(), pad.size());
    secure_zero(inner_hash.data(), inner_hash.size());
    return outer.finalize();
}

/// @brief Fills out from the CSPRNG (noise-c's randstate, the operating system's source).
/// Aborts when it cannot be created: a predictable scalar or nonce is never used.
inline void random_bytes(uint8_t* out, size_t len) {
    NoiseRandState* rng = nullptr;
    if (noise_randstate_new(&rng) != NOISE_ERROR_NONE || rng == nullptr) {
        abort();
    }
    noise_randstate_generate(rng, out, len);
    noise_randstate_free(rng);
}

/// @brief Compares len bytes of a and b with no early exit, for tags and commitments
inline bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace sendspin::server
