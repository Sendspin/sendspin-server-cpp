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

#include "crypto/psk_wrap.h"

#include "crypto/crypto.h"

#include <cstring>
#include <memory>

extern "C" {
#include <noise/protocol/buffer.h>
#include <noise/protocol/cipherstate.h>
}

namespace sendspin::server {

namespace {

struct CipherStateDeleter {
    void operator()(NoiseCipherState* state) const {
        noise_cipherstate_free(state);
    }
};

/// A cipher keyed with key at nonce 0, the all-zero nonce of either suite; nullptr on a failure
std::unique_ptr<NoiseCipherState, CipherStateDeleter> keyed_cipher(const char* cipher_name,
                                                                   const WrapKey& key) {
    NoiseCipherState* raw = nullptr;
    if (cipher_name == nullptr ||
        noise_cipherstate_new_by_name(&raw, cipher_name) != NOISE_ERROR_NONE || raw == nullptr) {
        return nullptr;
    }
    std::unique_ptr<NoiseCipherState, CipherStateDeleter> cipher(raw);
    if (noise_cipherstate_init_key(cipher.get(), key.data(), key.size()) != NOISE_ERROR_NONE ||
        noise_cipherstate_set_nonce(cipher.get(), 0) != NOISE_ERROR_NONE) {
        return nullptr;
    }
    return cipher;
}

}  // namespace

std::optional<WrapKey> derive_wrap_key(std::string_view label, const std::vector<uint8_t>& sid,
                                       const std::array<uint8_t, CPACE_ISK_SIZE>& isk) {
    Sha256 hash;
    hash.update(reinterpret_cast<const uint8_t*>(label.data()), label.size());
    hash.update(sid.data(), sid.size());
    hash.update(isk.data(), isk.size());
    WrapKey key = hash.finalize();
    // A failed hash finalizes to zeros, a public key
    if (!hash.ok()) {
        return std::nullopt;
    }
    return key;
}

std::optional<WrappedValue> wrap_value(std::string_view label, const char* cipher_name,
                                       const std::vector<uint8_t>& sid,
                                       const std::array<uint8_t, CPACE_ISK_SIZE>& isk,
                                       const std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>& value) {
    std::optional<WrapKey> key = derive_wrap_key(label, sid, isk);
    if (!key) {
        return std::nullopt;
    }
    const std::unique_ptr<NoiseCipherState, CipherStateDeleter> cipher =
        keyed_cipher(cipher_name, *key);
    secure_zero(key->data(), key->size());
    if (!cipher) {
        return std::nullopt;
    }
    WrappedValue wrapped{};
    std::memcpy(wrapped.data(), value.data(), value.size());
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, wrapped.data(), value.size(), wrapped.size());
    if (noise_cipherstate_encrypt_with_ad(cipher.get(), nullptr, 0, &buffer) != NOISE_ERROR_NONE ||
        buffer.size != wrapped.size()) {
        secure_zero(wrapped.data(), wrapped.size());
        return std::nullopt;
    }
    return wrapped;
}

std::optional<std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>> unwrap_value(
    std::string_view label, const char* cipher_name, const std::vector<uint8_t>& sid,
    const std::array<uint8_t, CPACE_ISK_SIZE>& isk, const WrappedValue& wrapped) {
    std::optional<WrapKey> key = derive_wrap_key(label, sid, isk);
    if (!key) {
        return std::nullopt;
    }
    const std::unique_ptr<NoiseCipherState, CipherStateDeleter> cipher =
        keyed_cipher(cipher_name, *key);
    secure_zero(key->data(), key->size());
    if (!cipher) {
        return std::nullopt;
    }
    WrappedValue buffer_bytes = wrapped;
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, buffer_bytes.data(), buffer_bytes.size(), buffer_bytes.size());
    std::optional<std::array<uint8_t, WRAPPED_PLAINTEXT_SIZE>> value;
    if (noise_cipherstate_decrypt_with_ad(cipher.get(), nullptr, 0, &buffer) == NOISE_ERROR_NONE &&
        buffer.size == WRAPPED_PLAINTEXT_SIZE) {
        value.emplace();
        std::memcpy(value->data(), buffer_bytes.data(), WRAPPED_PLAINTEXT_SIZE);
    }
    secure_zero(buffer_bytes.data(), buffer_bytes.size());
    return value;
}

}  // namespace sendspin::server
