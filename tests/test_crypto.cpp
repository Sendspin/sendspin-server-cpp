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

/// @file test_crypto.cpp
/// @brief Base64url, peer ids, the server identity and the Sentinel constants (spec connection.md
/// "Identities" and "Pre-Shared Key"), and the wipe of a byte vector's storage as it grows.

#include "crypto/base64url.h"
#include "crypto/crypto.h"
#include "crypto/keys.h"
#include "freed_blocks.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sendspin::server::b64url_decode;
using sendspin::server::b64url_encode;
using sendspin::server::Identity;
using sendspin::server::PEER_ID_SIZE;
using sendspin::server::public_key_from_peer_id;
using sendspin::server::PublicKey;
using sendspin::server::secure_reserve;

std::string hex(const uint8_t* data, size_t size) {
    std::string out;
    for (size_t i = 0; i < size; ++i) {
        char byte[3];
        std::snprintf(byte, sizeof(byte), "%02x", data[i]);
        out += byte;
    }
    return out;
}

constexpr std::string_view SECRET = "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVphYmNkZWY";

TEST(Base64Url, EncodesAndDecodesRfc4648Vectors) {
    struct Row {
        std::vector<uint8_t> bytes;
        const char* text;
    };
    const Row rows[] = {
        {{}, ""},
        {{'f'}, "Zg"},
        {{'f', 'o'}, "Zm8"},
        {{'f', 'o', 'o'}, "Zm9v"},
        {{'f', 'o', 'o', 'b'}, "Zm9vYg"},
        {{'f', 'o', 'o', 'b', 'a', 'r'}, "Zm9vYmFy"},
        // The URL-safe characters for values 62 and 63
        {{0xFB, 0xFF}, "-_8"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        EXPECT_EQ(b64url_encode(row.bytes.data(), row.bytes.size()), row.text);
        EXPECT_EQ(b64url_decode(row.text), row.bytes);
    }
}

// spec player/v1.md "Codec framing": codec_header is standard Base64, padding included
TEST(Base64, EncodesRfc4648Vectors) {
    struct Row {
        std::vector<uint8_t> bytes;
        const char* text;
    };
    const Row rows[] = {
        {{}, ""},
        {{'f'}, "Zg=="},
        {{'f', 'o'}, "Zm8="},
        {{'f', 'o', 'o'}, "Zm9v"},
        {{'f', 'o', 'o', 'b'}, "Zm9vYg=="},
        {{'f', 'o', 'o', 'b', 'a', 'r'}, "Zm9vYmFy"},
        // The standard characters for values 62 and 63
        {{0xFB, 0xFF}, "+/8="},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        EXPECT_EQ(sendspin::server::base64_encode(row.bytes.data(), row.bytes.size()), row.text);
    }
}

TEST(Base64Url, DecodeRejectsCharactersOutsideTheAlphabetAndImpossibleLengths) {
    struct Row {
        const char* text;
        bool valid;
    };
    const Row rows[] = {
        {"Zm8", true},  // Control:
        {"Zm8=", true},  {"Zm9vY", false}, {"Zm+v", false},
        {"Zm/v", false}, {"Zm v", false},  {"Zm8\n", false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        EXPECT_EQ(b64url_decode(row.text).has_value(), row.valid);
    }
}

TEST(PeerId, DecodesOnlyTheCanonicalEncodingOfAKey) {
    const std::optional<Identity> identity = Identity::generate();
    ASSERT_TRUE(identity);
    const std::string id = identity->peer_id();
    ASSERT_EQ(id.size(), PEER_ID_SIZE);

    // The last character carries two unused bits; flipping one names the same key differently
    std::string non_canonical = id;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    non_canonical.back() = alphabet[alphabet.find(id.back()) ^ 1U];

    // The small-order points, and two with the high bit set that X25519 reads the same way
    auto id_of = [](PublicKey key) { return b64url_encode(key.data(), key.size()); };
    PublicKey p_minus_one{};
    p_minus_one.fill(0xFF);
    p_minus_one.front() = 0xEC;
    p_minus_one.back() = 0x7F;
    PublicKey p_plus_one = p_minus_one;
    p_plus_one.front() = 0xEE;
    PublicKey one{0x01};
    PublicKey one_high = one;
    one_high.back() = 0x80;
    const PublicKey order_8a{0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3,
                             0xfa, 0xf1, 0x9f, 0xc4, 0x6a, 0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32,
                             0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00};
    const PublicKey order_8b{0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1,
                             0x55, 0x9c, 0x83, 0xef, 0x5b, 0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c,
                             0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57};
    PublicKey order_8b_high = order_8b;
    order_8b_high.back() |= 0x80;
    PublicKey p = p_minus_one;
    p.front() = 0xED;
    PublicKey two{0x02};

    struct Row {
        std::string id;
        bool valid;
    };
    const Row rows[] = {
        {id, true},  // Control:
        {id_of(two), true},
        {id_of(PublicKey{}), false},
        {id_of(one), false},
        {id_of(one_high), false},
        {id_of(order_8a), false},
        {id_of(order_8b), false},
        {id_of(order_8b_high), false},
        {id_of(p_minus_one), false},
        {id_of(p), false},
        {id_of(p_plus_one), false},
        {id.substr(0, PEER_ID_SIZE - 1), false},
        {id + "A", false},
        {id + "=", false},
        {non_canonical, false},
        {std::string(PEER_ID_SIZE, '+'), false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.id);
        const std::optional<PublicKey> key = public_key_from_peer_id(row.id);
        ASSERT_EQ(key.has_value(), row.valid);
        if (row.valid) {
            EXPECT_EQ(id_of(*key), row.id);
        }
    }
}

TEST(ServerIdentity, IsReconstructedFromItsPrivateKey) {
    const std::optional<Identity> first = Identity::generate();
    const std::optional<Identity> second = Identity::generate();
    ASSERT_TRUE(first && second);
    EXPECT_NE(first->private_bytes, second->private_bytes);

    const std::optional<Identity> restored = Identity::from_private_bytes(first->private_bytes);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->public_bytes, first->public_bytes);
    EXPECT_EQ(restored->peer_id(), first->peer_id());
}

TEST(ServerIdentity, MovesWithoutLeavingThePrivateKeyBehind) {
    std::optional<Identity> source = Identity::generate();
    ASSERT_TRUE(source);
    const auto private_bytes = source->private_bytes;
    const Identity moved(std::move(*source));
    EXPECT_EQ(moved.private_bytes, private_bytes);
    EXPECT_EQ(source->private_bytes, decltype(private_bytes){});

    std::optional<Identity> other = Identity::from_private_bytes(private_bytes);
    ASSERT_TRUE(other);
    Identity assigned;
    assigned = std::move(*other);
    EXPECT_EQ(assigned.private_bytes, private_bytes);
    EXPECT_EQ(assigned.public_bytes, moved.public_bytes);
    EXPECT_EQ(other->private_bytes, decltype(private_bytes){});
}

TEST(SentinelPsk, MatchesThePublishedConstants) {
    EXPECT_EQ(hex(sendspin::server::SENTINEL_PSK.data(), sendspin::server::SENTINEL_PSK.size()),
              "1b5e24dbc1aed95fc2a5a338a90c05df44bd10f5ec1f4cd66cbf86272767b9d3");
    EXPECT_EQ(sendspin::server::SENTINEL_PSK_ID, "GFsV9tLaSQm9HcFWpKsgYQOr7wFTvNUtkmFwuVz3zoo");
}

// A vector grown by secure_reserve() frees its old block wiped as far as it was written, past its
// size too, or to its size, and keeps its contents; one grown by its own insert() frees the block
// holding them: the Control. Room it has already moves nothing.
TEST(SecureReserve, WipesTheBlockItLeaves) {
    if (!FreedBlockScan::available()) {
        GTEST_SKIP() << "needs the sanitizer's free hook";
    }
    {
        std::vector<uint8_t> control(SECRET.begin(), SECRET.end());
        control.shrink_to_fit();
        const FreedBlockScan scan(SECRET);
        control.insert(control.end(), SECRET.size(), 'x');
        EXPECT_EQ(scan.holding(), 1);
    }

    std::vector<uint8_t> bytes{'x'};
    bytes.reserve(1 + SECRET.size());
    bytes.insert(bytes.end(), SECRET.begin(), SECRET.end());
    bytes.resize(1);
    const size_t capacity = bytes.capacity();
    {
        const FreedBlockScan scan(SECRET);
        secure_reserve(bytes, capacity + 1, 1 + SECRET.size());
        EXPECT_EQ(scan.holding(), 0);
    }
    EXPECT_EQ(bytes, std::vector<uint8_t>{'x'});
    EXPECT_EQ(bytes.capacity(), capacity + 1);

    const uint8_t* data = bytes.data();
    secure_reserve(bytes, bytes.capacity(), bytes.size());
    EXPECT_EQ(bytes.data(), data);

    // Its contents are wiped though none was counted as written
    std::vector<uint8_t> unwritten(SECRET.begin(), SECRET.end());
    const FreedBlockScan scan(SECRET);
    secure_reserve(unwritten, unwritten.capacity() + 1, 0);
    EXPECT_EQ(scan.holding(), 0);
}

}  // namespace
