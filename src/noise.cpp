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

#include "noise.h"

#include "crypto/crypto.h"

extern "C" {
#include <noise/protocol/buffer.h>
#include <noise/protocol/cipherstate.h>
#include <noise/protocol/constants.h>
#include <noise/protocol/dhstate.h>
#include <noise/protocol/handshakestate.h>
}

namespace sendspin::server {

namespace {

struct SuiteNames {
    CipherSuite suite;
    std::string_view wire;
    const char* protocol;
};

constexpr SuiteNames SUITES[] = {
    {CipherSuite::CHACHAPOLY_SHA256, "25519_ChaChaPoly_SHA256",
     "Noise_KKpsk2_25519_ChaChaPoly_SHA256"},
    {CipherSuite::AESGCM_SHA256, "25519_AESGCM_SHA256", "Noise_KKpsk2_25519_AESGCM_SHA256"},
};

const SuiteNames& names_of(CipherSuite suite) {
    for (const SuiteNames& names : SUITES) {
        if (names.suite == suite) {
            return names;
        }
    }
    return SUITES[0];
}

/// Room a handshake message needs beyond its payload: KKpsk2 sends one ephemeral key per
/// message, and the payload carries an AEAD tag
constexpr size_t HANDSHAKE_OVERHEAD = X25519_KEY_SIZE + 16;

}  // namespace

std::optional<CipherSuite> cipher_suite_from_wire(std::string_view name) {
    for (const SuiteNames& names : SUITES) {
        if (names.wire == name) {
            return names.suite;
        }
    }
    return std::nullopt;
}

std::string_view to_wire(CipherSuite suite) {
    return names_of(suite).wire;
}

std::string_view to_wire(PskCategory category) {
    switch (category) {
        case PskCategory::LONG_TERM:
            return "lt";
        case PskCategory::PAIRING:
            return "pr";
        case PskCategory::SENTINEL:
            break;
    }
    return "sn";
}

HandshakePsk::~HandshakePsk() {
    secure_zero(this->psk.data(), this->psk.size());
}

namespace detail {

void HandshakeStateDeleter::operator()(NoiseHandshakeState_s* state) const {
    noise_handshakestate_free(state);
}

void CipherStateDeleter::operator()(NoiseCipherState_s* state) const {
    noise_cipherstate_free(state);
}

}  // namespace detail

// ==============================================================================
// TransportCiphers
// ==============================================================================

bool TransportCiphers::encrypt(std::vector<uint8_t>& message) {
    const size_t plaintext_size = message.size();
    message.resize(plaintext_size + noise_cipherstate_get_mac_length(this->send_.get()));
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, message.data(), plaintext_size, message.size());
    if (noise_cipherstate_encrypt(this->send_.get(), &buffer) != NOISE_ERROR_NONE) {
        return false;
    }
    message.resize(buffer.size);
    return true;
}

bool TransportCiphers::decrypt(std::vector<uint8_t>& message) {
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, message.data(), message.size(), message.size());
    if (noise_cipherstate_decrypt(this->receive_.get(), &buffer) != NOISE_ERROR_NONE) {
        return false;
    }
    message.resize(buffer.size);
    return true;
}

// ==============================================================================
// NoiseInitiator
// ==============================================================================

std::optional<NoiseInitiator> NoiseInitiator::create(CipherSuite suite, const Identity& local,
                                                     const PublicKey& remote,
                                                     std::span<const uint8_t> prologue,
                                                     const Psk& psk) {
    NoiseInitiator initiator;
    initiator.suite_ = suite;
    initiator.local_ = &local;
    initiator.remote_ = remote;
    initiator.prologue_.assign(prologue.begin(), prologue.end());
    initiator.state_ = initiator.start_state(psk);
    if (!initiator.state_) {
        return std::nullopt;
    }
    return initiator;
}

NoiseInitiator::~NoiseInitiator() {
    secure_zero(this->ephemeral_.data(), this->ephemeral_.size());
}

std::unique_ptr<NoiseHandshakeState_s, detail::HandshakeStateDeleter> NoiseInitiator::start_state(
    const Psk& psk) const {
    NoiseHandshakeState* raw = nullptr;
    if (noise_handshakestate_new_by_name(&raw, names_of(this->suite_).protocol,
                                         NOISE_ROLE_INITIATOR) != NOISE_ERROR_NONE) {
        return nullptr;
    }
    std::unique_ptr<NoiseHandshakeState_s, detail::HandshakeStateDeleter> state(raw);
    if (noise_dhstate_set_keypair_private(noise_handshakestate_get_local_keypair_dh(raw),
                                          this->local_->private_bytes.data(),
                                          this->local_->private_bytes.size()) != NOISE_ERROR_NONE ||
        noise_dhstate_set_public_key(noise_handshakestate_get_remote_public_key_dh(raw),
                                     this->remote_.data(),
                                     this->remote_.size()) != NOISE_ERROR_NONE ||
        noise_handshakestate_set_prologue(raw, this->prologue_.data(), this->prologue_.size()) !=
            NOISE_ERROR_NONE ||
        noise_handshakestate_set_pre_shared_key(raw, psk.data(), psk.size()) != NOISE_ERROR_NONE ||
        noise_handshakestate_start(raw) != NOISE_ERROR_NONE) {
        return nullptr;
    }
    return state;
}

namespace {

/// Writes message 1 carrying payload from state
std::optional<std::vector<uint8_t>> write_message(NoiseHandshakeState* state,
                                                  std::span<const uint8_t> payload) {
    std::vector<uint8_t> payload_copy(payload.begin(), payload.end());
    std::vector<uint8_t> message(payload.size() + HANDSHAKE_OVERHEAD);
    NoiseBuffer payload_buffer;
    noise_buffer_set_input(payload_buffer, payload_copy.data(), payload_copy.size());
    NoiseBuffer message_buffer;
    noise_buffer_set_output(message_buffer, message.data(), message.size());
    if (noise_handshakestate_write_message(state, &message_buffer, &payload_buffer) !=
        NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    message.resize(message_buffer.size);
    return message;
}

/// Reads message 2 into state
std::optional<std::vector<uint8_t>> read_message(NoiseHandshakeState* state,
                                                 std::span<const uint8_t> message) {
    std::vector<uint8_t> message_copy(message.begin(), message.end());
    // The payload is never longer than the message carrying it
    std::vector<uint8_t> payload(message.size());
    NoiseBuffer message_buffer;
    noise_buffer_set_input(message_buffer, message_copy.data(), message_copy.size());
    NoiseBuffer payload_buffer;
    noise_buffer_set_output(payload_buffer, payload.data(), payload.size());
    if (noise_handshakestate_read_message(state, &message_buffer, &payload_buffer) !=
        NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    payload.resize(payload_buffer.size);
    return payload;
}

}  // namespace

std::optional<std::vector<uint8_t>> NoiseInitiator::write_message_1(
    std::span<const uint8_t> payload) {
    if (!this->state_ ||
        noise_handshakestate_get_action(this->state_.get()) != NOISE_ACTION_WRITE_MESSAGE) {
        return std::nullopt;
    }
    // The ephemeral key goes through the fixed-key slot, the only one noise-c lets a caller read
    // back, so that replay_message_2() can write the same message 1
    NoiseDHState* ephemeral = noise_handshakestate_get_fixed_ephemeral_dh(this->state_.get());
    PublicKey ephemeral_public{};
    if (ephemeral == nullptr ||
        (noise_dhstate_has_keypair(ephemeral) == 0 &&
         noise_dhstate_generate_keypair(ephemeral) != NOISE_ERROR_NONE) ||
        noise_dhstate_get_keypair(ephemeral, this->ephemeral_.data(), this->ephemeral_.size(),
                                  ephemeral_public.data(),
                                  ephemeral_public.size()) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> message = write_message(this->state_.get(), payload);
    if (message) {
        this->payload_1_.assign(payload.begin(), payload.end());
        this->message_1_ = *message;
    }
    return message;
}

std::optional<std::vector<uint8_t>> NoiseInitiator::read_message_2(
    std::span<const uint8_t> message) {
    if (!this->state_ ||
        noise_handshakestate_get_action(this->state_.get()) != NOISE_ACTION_READ_MESSAGE) {
        return std::nullopt;
    }
    return read_message(this->state_.get(), message);
}

std::optional<std::vector<uint8_t>> NoiseInitiator::replay_message_2(
    const Psk& psk, std::span<const uint8_t> message) {
    if (this->message_1_.empty()) {
        return std::nullopt;
    }
    std::unique_ptr<NoiseHandshakeState_s, detail::HandshakeStateDeleter> state =
        this->start_state(psk);
    NoiseDHState* ephemeral =
        state ? noise_handshakestate_get_fixed_ephemeral_dh(state.get()) : nullptr;
    if (ephemeral == nullptr ||
        noise_dhstate_set_keypair_private(ephemeral, this->ephemeral_.data(),
                                          this->ephemeral_.size()) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    // The replay must produce the message the client read, or the two transcripts differ
    const std::optional<std::vector<uint8_t>> message_1 =
        write_message(state.get(), this->payload_1_);
    if (!message_1 || *message_1 != this->message_1_) {
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> payload = read_message(state.get(), message);
    if (payload) {
        this->state_ = std::move(state);
    }
    return payload;
}

std::optional<TransportCiphers> NoiseInitiator::split() {
    if (!this->state_ ||
        noise_handshakestate_get_action(this->state_.get()) != NOISE_ACTION_SPLIT) {
        return std::nullopt;
    }
    TransportCiphers ciphers;
    if (noise_handshakestate_get_handshake_hash(this->state_.get(), ciphers.handshake_hash_.data(),
                                                ciphers.handshake_hash_.size()) !=
        NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    NoiseCipherState* send = nullptr;
    NoiseCipherState* receive = nullptr;
    if (noise_handshakestate_split(this->state_.get(), &send, &receive) != NOISE_ERROR_NONE) {
        return std::nullopt;
    }
    ciphers.send_.reset(send);
    ciphers.receive_.reset(receive);
    this->state_.reset();
    return ciphers;
}

}  // namespace sendspin::server
