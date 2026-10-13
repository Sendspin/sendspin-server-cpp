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

/// @file test_noise_initiator.cpp
/// @brief NoiseInitiator against the cacophony KKpsk2 vectors for both suites (noise-c's
/// tests/vector/cacophony.txt), the lazy replay of the Sentinel Fallback against the same vectors,
/// and the suite names of spec connection.md "Cipher Suites".
///
/// White-box: the vectors fix the initiator's ephemeral key, which only the handshake state
/// (NoiseInitiator::state_) can take, and the replay is shown refusing a changed message 1 by
/// flipping NoiseInitiator::payload_1_ or message_1_.

#include "crypto/keys.h"
#include "noise.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::CipherSuite;
using sendspin::server::Identity;
using sendspin::server::NoiseInitiator;
using sendspin::server::Psk;
using sendspin::server::TransportCiphers;

std::vector<uint8_t> from_hex(const std::string& text) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

template <size_t N>
std::array<uint8_t, N> array_from_hex(const std::string& text) {
    const std::vector<uint8_t> bytes = from_hex(text);
    std::array<uint8_t, N> out{};
    std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

struct Vector {
    CipherSuite suite;
    const char* name;
    const char* handshake_hash;
    // Ciphertexts of the six messages; the payloads are the same for both suites
    const char* ciphertexts[6];
};

constexpr const char* PROLOGUE = "4a6f686e2047616c74";
constexpr const char* PSK = "54686973206973206d7920417573747269616e20706572737065637469766521";
constexpr const char* INIT_STATIC =
    "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1";
constexpr const char* INIT_EPHEMERAL =
    "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a";
constexpr const char* RESP_STATIC =
    "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893";
constexpr const char* PAYLOADS[6] = {
    "4c756477696720766f6e204d69736573",
    "4d757272617920526f746862617264",
    "462e20412e20486179656b",
    "4361726c204d656e676572",
    "4a65616e2d426170746973746520536179",
    "457567656e2042f6686d20766f6e2042617765726b",
};

const Vector VECTORS[] = {
    {CipherSuite::CHACHAPOLY_SHA256,
     "ChaChaPoly",
     "7f3c5fdcdd3767e2835473a2683971490339f5bbeee82c3690bc606e14db70ed",
     {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944babf6443250c604872e33233c3b9"
      "a29df5c6d334ae2d53f1bd7f0b265a716b37",
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f14480884366a1f5f0d79fe93ae476bd1897a7"
      "a8ae92764898aa5d49e07b5849f35865ba",
      "2eb2686b8814a7c0178fe18bfeeafe3e07312d69486d45e6572546",
      "eea5791a890cd573a5c2e2345a8f98b0d1f0727acd24584fcddde5",
      "ab2e1a411abaaa3df9cb497dffe4cfb70af6c71f0815b3c33b35e22329dee72f3e",
      "ed22a0392c6afbfd6a6adea92b1faf13c4df24072f7060a20b1500609621c6957ac86d82f9"}},
    {CipherSuite::AESGCM_SHA256,
     "AESGCM",
     "1d2be5b73c43962fa87db816e6b5a73a0d67053e29b3d89fcee6ae299b5a5c1f",
     {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c794483cf298f1bfed916ea20d8f528bd"
      "30670670c7245e236a68f021b5a521d8bf31",
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f14480884332b32e0f483ae58a9c36e93390a9"
      "c2238918bda6ae0ebfe48a400b8f565f5f",
      "f7cf1f47885fa5ff7cefecf3db3066f89b996834069b8f1517c55a",
      "ce7de28a6b54b559d3db2f7482f1be3264a62165f12a73e377ebde",
      "da4173c5aa84756d0eab64adfbe81f10538b099966897771a071502ee2899e2fbd",
      "1a1e3408acea689f5cced09b246e13e68e1ce146e8f1b3478482593b988587be99ba7595d2"}},
};

class NoiseInitiatorVector : public testing::TestWithParam<Vector> {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    }

    // The initiator with the vector's keys, prologue and fixed ephemeral key, and psk
    static std::optional<NoiseInitiator> make_initiator(CipherSuite suite,
                                                        const Psk& psk = array_from_hex<32>(PSK)) {
        static const std::optional<Identity> local =
            Identity::from_private_bytes(array_from_hex<32>(INIT_STATIC));
        const std::optional<Identity> remote =
            Identity::from_private_bytes(array_from_hex<32>(RESP_STATIC));
        EXPECT_TRUE(local && remote);
        std::optional<NoiseInitiator> initiator =
            NoiseInitiator::create(suite, *local, remote->public_bytes, from_hex(PROLOGUE), psk);
        EXPECT_TRUE(initiator);
        const std::vector<uint8_t> ephemeral = from_hex(INIT_EPHEMERAL);
        EXPECT_EQ(noise_dhstate_set_keypair_private(
                      noise_handshakestate_get_fixed_ephemeral_dh(initiator->state_.get()),
                      ephemeral.data(), ephemeral.size()),
                  NOISE_ERROR_NONE);
        return initiator;
    }
};

TEST_P(NoiseInitiatorVector, ProducesTheVectorsHandshakeAndTransportMessages) {
    const Vector& vector = GetParam();
    std::optional<NoiseInitiator> initiator = make_initiator(vector.suite);
    ASSERT_TRUE(initiator);

    EXPECT_EQ(initiator->write_message_1(from_hex(PAYLOADS[0])), from_hex(vector.ciphertexts[0]));
    EXPECT_EQ(initiator->read_message_2(from_hex(vector.ciphertexts[1])), from_hex(PAYLOADS[1]));
    std::optional<TransportCiphers> ciphers = initiator->split();
    ASSERT_TRUE(ciphers);
    EXPECT_EQ(
        std::vector<uint8_t>(ciphers->handshake_hash().begin(), ciphers->handshake_hash().end()),
        from_hex(vector.handshake_hash));

    // Transport messages alternate, initiator first
    for (size_t i = 2; i < 6; ++i) {
        SCOPED_TRACE(i);
        std::vector<uint8_t> message;
        if (i % 2 == 0) {
            message = from_hex(PAYLOADS[i]);
            ASSERT_TRUE(ciphers->encrypt(message));
            EXPECT_EQ(message, from_hex(vector.ciphertexts[i]));
        } else {
            message = from_hex(vector.ciphertexts[i]);
            ASSERT_TRUE(ciphers->decrypt(message));
            EXPECT_EQ(message, from_hex(PAYLOADS[i]));
        }
    }
}

// connection.md "Sentinel Fallback", by lazy replay: an initiator bound to another PSK writes the
// vector's message 1 all the same, since KKpsk2 mixes the PSK in only at message 2; its read of the
// vector's message 2 fails, and the replay under the vector's PSK, with the same ephemeral key,
// reads it and splits into the vector's handshake hash and transport messages
TEST_P(NoiseInitiatorVector, ReplaysMessage1UnderAnotherPsk) {
    const Vector& vector = GetParam();
    Psk other{};
    other.fill(0x42);
    std::optional<NoiseInitiator> initiator = make_initiator(vector.suite, other);
    ASSERT_TRUE(initiator);
    EXPECT_EQ(initiator->write_message_1(from_hex(PAYLOADS[0])), from_hex(vector.ciphertexts[0]));
    EXPECT_FALSE(initiator->read_message_2(from_hex(vector.ciphertexts[1])));
    // Control: the replay under the PSK that failed fails again
    EXPECT_FALSE(initiator->replay_message_2(other, from_hex(vector.ciphertexts[1])));
    EXPECT_EQ(initiator->replay_message_2(array_from_hex<32>(PSK), from_hex(vector.ciphertexts[1])),
              from_hex(PAYLOADS[1]));
    std::optional<TransportCiphers> ciphers = initiator->split();
    ASSERT_TRUE(ciphers);
    EXPECT_EQ(
        std::vector<uint8_t>(ciphers->handshake_hash().begin(), ciphers->handshake_hash().end()),
        from_hex(vector.handshake_hash));
    std::vector<uint8_t> sent = from_hex(PAYLOADS[2]);
    ASSERT_TRUE(ciphers->encrypt(sent));
    EXPECT_EQ(sent, from_hex(vector.ciphertexts[2]));
    std::vector<uint8_t> received = from_hex(vector.ciphertexts[3]);
    ASSERT_TRUE(ciphers->decrypt(received));
    EXPECT_EQ(received, from_hex(PAYLOADS[3]));
}

// The replay must write the message 1 the client read, or the transcripts differ: one whose kept
// payload or message 1 changed is refused. White-box: flips a byte of NoiseInitiator::payload_1_
// or message_1_. Only the message 1 row is refused by the comparison alone, since the replay of
// the original payload reads message 2. Control: nothing flipped.
TEST_P(NoiseInitiatorVector, ReplaysOnlyTheMessage1TheClientRead) {
    const Vector& vector = GetParam();
    struct Row {
        const char* name;
        std::vector<uint8_t> NoiseInitiator::*kept;
        bool replayed;
    };
    const Row rows[] = {
        {"Control: nothing flipped", nullptr, true},
        {"the payload flipped", &NoiseInitiator::payload_1_, false},
        {"message 1 flipped", &NoiseInitiator::message_1_, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Psk other{};
        other.fill(0x42);
        std::optional<NoiseInitiator> initiator = make_initiator(vector.suite, other);
        ASSERT_TRUE(initiator);
        ASSERT_TRUE(initiator->write_message_1(from_hex(PAYLOADS[0])));
        if (row.kept != nullptr) {
            ((*initiator).*row.kept).back() ^= 0x01;
        }
        EXPECT_EQ(
            initiator->replay_message_2(array_from_hex<32>(PSK), from_hex(vector.ciphertexts[1]))
                .has_value(),
            row.replayed);
    }
}

// The replay rebuilds message 1 from the ephemeral key write_message_1() kept: without a message
// 1 there is nothing to replay
TEST_P(NoiseInitiatorVector, ReplaysOnlyAfterMessage1) {
    const Vector& vector = GetParam();
    std::optional<NoiseInitiator> initiator = make_initiator(vector.suite);
    ASSERT_TRUE(initiator);
    EXPECT_FALSE(
        initiator->replay_message_2(array_from_hex<32>(PSK), from_hex(vector.ciphertexts[1])));
}

TEST_P(NoiseInitiatorVector, RejectsATamperedMessage2) {
    const Vector& vector = GetParam();
    std::optional<NoiseInitiator> initiator = make_initiator(vector.suite);
    ASSERT_TRUE(initiator);
    ASSERT_TRUE(initiator->write_message_1(from_hex(PAYLOADS[0])));
    std::vector<uint8_t> message_2 = from_hex(vector.ciphertexts[1]);
    message_2.back() ^= 0x01;
    EXPECT_FALSE(initiator->read_message_2(message_2));
    EXPECT_FALSE(initiator->split());
}

TEST_P(NoiseInitiatorVector, RejectsATamperedTransportMessage) {
    const Vector& vector = GetParam();
    std::optional<NoiseInitiator> initiator = make_initiator(vector.suite);
    ASSERT_TRUE(initiator);
    ASSERT_TRUE(initiator->write_message_1(from_hex(PAYLOADS[0])));
    ASSERT_TRUE(initiator->read_message_2(from_hex(vector.ciphertexts[1])));
    std::optional<TransportCiphers> ciphers = initiator->split();
    ASSERT_TRUE(ciphers);
    // The responder's first transport message, with one bit flipped
    std::vector<uint8_t> message = from_hex(vector.ciphertexts[3]);
    message.front() ^= 0x01;
    EXPECT_FALSE(ciphers->decrypt(message));
}

INSTANTIATE_TEST_SUITE_P(Suites, NoiseInitiatorVector, testing::ValuesIn(VECTORS),
                         [](const testing::TestParamInfo<Vector>& info) {
                             return std::string(info.param.name);
                         });

TEST(CipherSuiteNames, MapTheClientInitSuiteNames) {
    struct Row {
        const char* name;
        std::optional<CipherSuite> suite;
    };
    const Row rows[] = {
        {"25519_ChaChaPoly_SHA256", CipherSuite::CHACHAPOLY_SHA256},  // Control:
        {"25519_AESGCM_SHA256", CipherSuite::AESGCM_SHA256},
        {"Noise_KKpsk2_25519_ChaChaPoly_SHA256", std::nullopt},
        {"25519_chachapoly_sha256", std::nullopt},
        {"", std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<CipherSuite> suite = sendspin::server::cipher_suite_from_wire(row.name);
        EXPECT_EQ(suite, row.suite);
        if (suite) {
            EXPECT_EQ(sendspin::server::to_wire(*suite), row.name);
        }
    }
}

}  // namespace
