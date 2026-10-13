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

/// @file noise.h
/// @brief The server's side of the Noise KKpsk2 handshake (the server is always the initiator,
/// spec connection.md "Pattern") and the transport cipher states it splits into

#pragma once

#include "crypto/keys.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct NoiseHandshakeState_s;
struct NoiseCipherState_s;

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

namespace detail {
struct HandshakeStateDeleter {
    void operator()(NoiseHandshakeState_s* state) const;
};
struct CipherStateDeleter {
    void operator()(NoiseCipherState_s* state) const;
};
}  // namespace detail

/// @brief The transport-mode cipher states of one connection, one per direction
class TransportCiphers {
public:
    /// @brief Encrypts message in place and appends the AEAD tag, without reallocating when its
    /// capacity has room for the tag (reserve_plaintext())
    /// @return false when the send cipher refuses (nonce exhausted); the message is then unusable
    bool encrypt(std::vector<uint8_t>& message);

    /// @brief Authenticates and decrypts message in place, dropping the tag
    /// @return false on an AEAD failure, which spec connection.md "Failure Handling" makes a
    /// silent failure
    bool decrypt(std::vector<uint8_t>& message);

    const HandshakeHash& handshake_hash() const {
        return this->handshake_hash_;
    }

private:
    friend class NoiseInitiator;

    std::unique_ptr<NoiseCipherState_s, detail::CipherStateDeleter> send_;
    std::unique_ptr<NoiseCipherState_s, detail::CipherStateDeleter> receive_;
    HandshakeHash handshake_hash_{};
};

/// @brief One KKpsk2 handshake as the initiator: write message 1, read message 2, split.
///
/// The PSK is bound at creation; KKpsk2 mixes it in only at message 2 (spec connection.md
/// "Pre-Shared Key"), so message 1 does not depend on it. That lets replay_message_2() verify
/// message 2 under another PSK without a second message 1. The ephemeral private key is kept for
/// that replay and wiped when the initiator is destroyed.
class NoiseInitiator {
public:
    /// @param local must outlive the initiator
    /// @return nullopt when noise-c rejects a key or the suite
    static std::optional<NoiseInitiator> create(CipherSuite suite, const Identity& local,
                                                const PublicKey& remote,
                                                std::span<const uint8_t> prologue, const Psk& psk);

    ~NoiseInitiator();
    NoiseInitiator(NoiseInitiator&&) noexcept = default;
    NoiseInitiator& operator=(NoiseInitiator&&) noexcept = default;

    /// @return The handshake message bytes carrying payload, or nullopt out of sequence
    std::optional<std::vector<uint8_t>> write_message_1(std::span<const uint8_t> payload);

    /// @return The decrypted payload, or nullopt when the message fails to authenticate or
    /// arrives out of sequence
    std::optional<std::vector<uint8_t>> read_message_2(std::span<const uint8_t> message);

    /// @brief Reads message 2 under psk instead of the PSK bound at creation: a new handshake
    /// state with message 1's ephemeral key writes message 1 again from its payload, byte for
    /// byte, and reads message 2. On success split() takes the new state. Only after
    /// write_message_1(); the state read_message_2() failed in is discarded.
    /// @return The decrypted payload, or nullopt when the message fails to authenticate under psk
    std::optional<std::vector<uint8_t>> replay_message_2(const Psk& psk,
                                                         std::span<const uint8_t> message);

    /// @brief The transport ciphers and `h`; only after read_message_2() or replay_message_2()
    /// succeeded, and once
    std::optional<TransportCiphers> split();

private:
    NoiseInitiator() = default;

    /// @brief A started handshake state with this initiator's keys and prologue, keyed by psk
    std::unique_ptr<NoiseHandshakeState_s, detail::HandshakeStateDeleter> start_state(
        const Psk& psk) const;

    std::unique_ptr<NoiseHandshakeState_s, detail::HandshakeStateDeleter> state_;
    CipherSuite suite_{CipherSuite::CHACHAPOLY_SHA256};
    const Identity* local_{nullptr};
    PublicKey remote_{};
    std::vector<uint8_t> prologue_;
    /// Message 1's payload and bytes, and its ephemeral private key, from write_message_1() on
    std::vector<uint8_t> payload_1_;
    std::vector<uint8_t> message_1_;
    std::array<uint8_t, X25519_KEY_SIZE> ephemeral_{};
};

}  // namespace sendspin::server
