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

/// @file test_noise.cpp
/// @brief noise-c builds with both cipher suites the server must support (spec "Cipher Suites"):
/// a KKpsk2 handshake completes under each and the split cipher states carry a message.

#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr size_t KEY_BYTES = 32;
constexpr size_t MAX_MESSAGE_BYTES = 256;

struct HandshakeDeleter {
    void operator()(NoiseHandshakeState* state) const {
        noise_handshakestate_free(state);
    }
};
struct CipherDeleter {
    void operator()(NoiseCipherState* state) const {
        noise_cipherstate_free(state);
    }
};
using HandshakePtr = std::unique_ptr<NoiseHandshakeState, HandshakeDeleter>;
using CipherPtr = std::unique_ptr<NoiseCipherState, CipherDeleter>;

HandshakePtr new_handshake(const char* protocol, int role) {
    NoiseHandshakeState* state = nullptr;
    EXPECT_EQ(noise_handshakestate_new_by_name(&state, protocol, role), NOISE_ERROR_NONE);
    return HandshakePtr(state);
}

std::array<uint8_t, KEY_BYTES> generate_local_key(NoiseHandshakeState* state) {
    NoiseDHState* dh = noise_handshakestate_get_local_keypair_dh(state);
    EXPECT_EQ(noise_dhstate_generate_keypair(dh), NOISE_ERROR_NONE);
    std::array<uint8_t, KEY_BYTES> public_key{};
    EXPECT_EQ(noise_dhstate_get_public_key(dh, public_key.data(), public_key.size()),
              NOISE_ERROR_NONE);
    return public_key;
}

// Writes the next handshake message on `writer` and reads it on `reader`
void transfer(NoiseHandshakeState* writer, NoiseHandshakeState* reader) {
    ASSERT_EQ(noise_handshakestate_get_action(writer), NOISE_ACTION_WRITE_MESSAGE);
    ASSERT_EQ(noise_handshakestate_get_action(reader), NOISE_ACTION_READ_MESSAGE);
    std::array<uint8_t, MAX_MESSAGE_BYTES> bytes{};
    NoiseBuffer message;
    noise_buffer_set_output(message, bytes.data(), bytes.size());
    ASSERT_EQ(noise_handshakestate_write_message(writer, &message, nullptr), NOISE_ERROR_NONE);
    noise_buffer_set_input(message, bytes.data(), message.size);
    ASSERT_EQ(noise_handshakestate_read_message(reader, &message, nullptr), NOISE_ERROR_NONE);
}

class NoiseSuite : public testing::TestWithParam<std::pair<const char*, int>> {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    }
};

TEST_P(NoiseSuite, KKpsk2HandshakeCarriesATransportMessage) {
    const auto [protocol, cipher_id] = GetParam();
    HandshakePtr initiator = new_handshake(protocol, NOISE_ROLE_INITIATOR);
    HandshakePtr responder = new_handshake(protocol, NOISE_ROLE_RESPONDER);
    ASSERT_TRUE(initiator && responder);

    const auto initiator_key = generate_local_key(initiator.get());
    const auto responder_key = generate_local_key(responder.get());
    ASSERT_EQ(
        noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(initiator.get()),
                                     responder_key.data(), responder_key.size()),
        NOISE_ERROR_NONE);
    ASSERT_EQ(
        noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(responder.get()),
                                     initiator_key.data(), initiator_key.size()),
        NOISE_ERROR_NONE);

    std::array<uint8_t, KEY_BYTES> psk{};
    psk.fill(0x5A);
    for (NoiseHandshakeState* state : {initiator.get(), responder.get()}) {
        ASSERT_EQ(noise_handshakestate_set_pre_shared_key(state, psk.data(), psk.size()),
                  NOISE_ERROR_NONE);
        ASSERT_EQ(noise_handshakestate_start(state), NOISE_ERROR_NONE);
    }

    transfer(initiator.get(), responder.get());
    transfer(responder.get(), initiator.get());
    ASSERT_EQ(noise_handshakestate_get_action(initiator.get()), NOISE_ACTION_SPLIT);
    ASSERT_EQ(noise_handshakestate_get_action(responder.get()), NOISE_ACTION_SPLIT);

    NoiseCipherState* raw_send = nullptr;
    NoiseCipherState* raw_unused = nullptr;
    ASSERT_EQ(noise_handshakestate_split(initiator.get(), &raw_send, &raw_unused),
              NOISE_ERROR_NONE);
    CipherPtr initiator_send(raw_send);
    CipherPtr initiator_receive(raw_unused);
    NoiseCipherState* raw_receive = nullptr;
    ASSERT_EQ(noise_handshakestate_split(responder.get(), &raw_unused, &raw_receive),
              NOISE_ERROR_NONE);
    CipherPtr responder_send(raw_unused);
    CipherPtr responder_receive(raw_receive);
    EXPECT_EQ(noise_cipherstate_get_cipher_id(initiator_send.get()), cipher_id);

    const std::string plaintext = R"({"type":"server/time"})";
    std::array<uint8_t, MAX_MESSAGE_BYTES> bytes{};
    std::memcpy(bytes.data(), plaintext.data(), plaintext.size());
    NoiseBuffer message;
    noise_buffer_set_inout(message, bytes.data(), plaintext.size(), bytes.size());
    ASSERT_EQ(noise_cipherstate_encrypt(initiator_send.get(), &message), NOISE_ERROR_NONE);
    ASSERT_EQ(message.size,
              plaintext.size() + noise_cipherstate_get_mac_length(initiator_send.get()));
    EXPECT_NE(std::memcmp(bytes.data(), plaintext.data(), plaintext.size()), 0);

    // A flipped ciphertext bit fails authentication without consuming the nonce, so the original
    // then decrypts.
    std::array<uint8_t, MAX_MESSAGE_BYTES> tampered = bytes;
    tampered[0] ^= 0x01;
    NoiseBuffer tampered_message;
    noise_buffer_set_input(tampered_message, tampered.data(), message.size);
    EXPECT_EQ(
        noise_cipherstate_decrypt_with_ad(responder_receive.get(), nullptr, 0, &tampered_message),
        NOISE_ERROR_MAC_FAILURE);

    noise_buffer_set_input(message, bytes.data(), message.size);
    ASSERT_EQ(noise_cipherstate_decrypt(responder_receive.get(), &message), NOISE_ERROR_NONE);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(message.data), message.size), plaintext);
}

INSTANTIATE_TEST_SUITE_P(
    Suites, NoiseSuite,
    testing::Values(std::make_pair("Noise_KKpsk2_25519_ChaChaPoly_SHA256", NOISE_CIPHER_CHACHAPOLY),
                    std::make_pair("Noise_KKpsk2_25519_AESGCM_SHA256", NOISE_CIPHER_AESGCM)),
    [](const testing::TestParamInfo<std::pair<const char*, int>>& info) {
        return info.param.second == NOISE_CIPHER_AESGCM ? std::string("AESGCM")
                                                        : std::string("ChaChaPoly");
    });

}  // namespace
