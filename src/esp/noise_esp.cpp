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

// ESP-IDF backing for the esphome__noise-c component (cmake/esp_idf.cmake): its custom RNG hook
// from the hardware RNG (libsodium's randombytes too), the AES-GCM cipher it is built without, from
// mbedtls (its GCM API through ESP-IDF 5, PSA Crypto from ESP-IDF 6, whose mbedtls 4 has no
// mbedtls/gcm.h), and the X25519 ladder CPace calls, from libsodium. The hooks are weak: an
// application that defines noise_rand_bytes() itself (another noise-c user) wins, and sendspin-cpp
// defines a weak one too, of which the linker keeps one.

#include "esp_idf_version.h"
#include "esp_random.h"
#include "sodium.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define SENDSPIN_SERVER_AESGCM_PSA 1
#include "psa/crypto.h"
#else
#define SENDSPIN_SERVER_AESGCM_PSA 0
#include "mbedtls/gcm.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include "protocol/internal.h"
}

// The hardware RNG is a CSPRNG only while an RF subsystem runs or bootloader_random_enable() is
// in effect (ESP-IDF "Random Number Generation"), so the host enables one of them before
// SendspinServer::start() generates keys
extern "C" __attribute__((weak)) void noise_rand_bytes(void* bytes, size_t size) {
    esp_fill_random(bytes, size);
}

namespace {

struct EspAesGcmState {
    NoiseCipherState parent;
#if SENDSPIN_SERVER_AESGCM_PSA
    psa_key_id_t key;
#else
    mbedtls_gcm_context ctx;
#endif
    uint8_t iv[12];
    /// The key was set; noise-c's init_key cannot report a failure, so encrypt and decrypt do
    bool keyed;
};

// The 96-bit nonce: 32 bits of zeros, then n big-endian
void aesgcm_setup_iv(EspAesGcmState* st) {
    std::memset(st->iv, 0, 4);
    for (int i = 0; i < 8; ++i) {
        st->iv[4 + i] = static_cast<uint8_t>(st->parent.n >> (56 - (8 * i)));
    }
}

#if SENDSPIN_SERVER_AESGCM_PSA

void aesgcm_release_key(EspAesGcmState* st) {
    if (st->keyed) {
        psa_destroy_key(st->key);
        st->key = PSA_KEY_ID_NULL;
        st->keyed = false;
    }
}

void aesgcm_init_key(NoiseCipherState* state, const uint8_t* key) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    aesgcm_release_key(st);
    if (psa_crypto_init() != PSA_SUCCESS) {
        return;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    st->keyed = psa_import_key(&attributes, key, 32, &st->key) == PSA_SUCCESS;
    psa_reset_key_attributes(&attributes);
}

// In place: PSA's AEAD output is the ciphertext followed by the 16-byte tag, the layout noise-c
// gives data (data_len bytes and the tag's room after them)
int aesgcm_encrypt(NoiseCipherState* state, const uint8_t* ad, size_t ad_len, uint8_t* data,
                   size_t data_len) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    if (!st->keyed) {
        return NOISE_ERROR_SYSTEM;
    }
    aesgcm_setup_iv(st);
    size_t written = 0;
    if (psa_aead_encrypt(st->key, PSA_ALG_GCM, st->iv, sizeof(st->iv), ad, ad_len, data, data_len,
                         data, data_len + 16, &written) != PSA_SUCCESS ||
        written != data_len + 16) {
        return NOISE_ERROR_SYSTEM;
    }
    return NOISE_ERROR_NONE;
}

int aesgcm_decrypt(NoiseCipherState* state, const uint8_t* ad, size_t ad_len, uint8_t* data,
                   size_t data_len) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    if (!st->keyed) {
        return NOISE_ERROR_SYSTEM;
    }
    aesgcm_setup_iv(st);
    size_t written = 0;
    if (psa_aead_decrypt(st->key, PSA_ALG_GCM, st->iv, sizeof(st->iv), ad, ad_len, data,
                         data_len + 16, data, data_len, &written) != PSA_SUCCESS ||
        written != data_len) {
        return NOISE_ERROR_MAC_FAILURE;
    }
    return NOISE_ERROR_NONE;
}

void aesgcm_free(NoiseCipherState* state) {
    aesgcm_release_key(reinterpret_cast<EspAesGcmState*>(state));
}

#else

void aesgcm_init_key(NoiseCipherState* state, const uint8_t* key) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    mbedtls_gcm_free(&st->ctx);
    mbedtls_gcm_init(&st->ctx);
    st->keyed = mbedtls_gcm_setkey(&st->ctx, MBEDTLS_CIPHER_ID_AES, key, 256) == 0;
}

int aesgcm_encrypt(NoiseCipherState* state, const uint8_t* ad, size_t ad_len, uint8_t* data,
                   size_t data_len) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    if (!st->keyed) {
        return NOISE_ERROR_SYSTEM;
    }
    aesgcm_setup_iv(st);
    if (mbedtls_gcm_crypt_and_tag(&st->ctx, MBEDTLS_GCM_ENCRYPT, data_len, st->iv, sizeof(st->iv),
                                  ad, ad_len, data, data, 16, data + data_len) != 0) {
        return NOISE_ERROR_SYSTEM;
    }
    return NOISE_ERROR_NONE;
}

int aesgcm_decrypt(NoiseCipherState* state, const uint8_t* ad, size_t ad_len, uint8_t* data,
                   size_t data_len) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    if (!st->keyed) {
        return NOISE_ERROR_SYSTEM;
    }
    aesgcm_setup_iv(st);
    if (mbedtls_gcm_auth_decrypt(&st->ctx, data_len, st->iv, sizeof(st->iv), ad, ad_len,
                                 data + data_len, 16, data, data) != 0) {
        return NOISE_ERROR_MAC_FAILURE;
    }
    return NOISE_ERROR_NONE;
}

void aesgcm_free(NoiseCipherState* state) {
    auto* st = reinterpret_cast<EspAesGcmState*>(state);
    mbedtls_gcm_free(&st->ctx);
}

#endif

}  // namespace

namespace {

// libsodium's randombytes on the hardware RNG. noise_init_framework() runs sodium_init(), which
// stirs the default randombytes implementation, and that one opens /dev/urandom, which ESP-IDF has
// not, and aborts (sodium_misuse()). Set before main(); an application may set its own later.
const char* esp_randombytes_name() {
    return "esp_fill_random";
}

uint32_t esp_randombytes_random() {
    return esp_random();
}

void esp_randombytes_buf(void* const buf, const size_t size) {
    esp_fill_random(buf, size);
}

const randombytes_implementation ESP_RANDOMBYTES{
    esp_randombytes_name, esp_randombytes_random, nullptr, nullptr, esp_randombytes_buf, nullptr,
};

[[maybe_unused]] const int ESP_RANDOMBYTES_SET = randombytes_set_implementation(&ESP_RANDOMBYTES);

}  // namespace

// The X25519 ladder CPace calls (src/crypto/cpace.cpp), which noise-c's reference backend has and
// the libsodium-backed component does not: libsodium's, which always clamps the scalar, as CPace
// asks. For a low-order point libsodium returns -1 without writing out (its ref10 ladder checks
// the result before copying it), so out is zeroed here: the all-zero result the reference ladder
// gives, which CPace rejects itself and which leaves no earlier contents of out in use.
extern "C" __attribute__((weak)) int x25519(uint8_t out[32], const uint8_t scalar[32],
                                            const uint8_t x1[32], int clamp) {
    if (clamp == 0 || crypto_scalarmult_curve25519(out, scalar, x1) != 0) {
        std::memset(out, 0, 32);
        return -1;
    }
    return 0;
}

// noise-c's AES-GCM constructor, which esphome__noise-c's cipherstate.c calls once
// cmake/esp_idf.cmake compiles the AESGCM cipher id into it
extern "C" __attribute__((weak)) NoiseCipherState* noise_aesgcm_new(void) {
    auto* state = static_cast<EspAesGcmState*>(noise_new_object(sizeof(EspAesGcmState)));
    if (state == nullptr) {
        return nullptr;
    }
#if SENDSPIN_SERVER_AESGCM_PSA
    state->key = PSA_KEY_ID_NULL;
#else
    mbedtls_gcm_init(&state->ctx);
#endif
    state->keyed = false;
    state->parent.cipher_id = NOISE_CIPHER_AESGCM;
    state->parent.key_len = 32;
    state->parent.mac_len = 16;
    state->parent.create = noise_aesgcm_new;
    state->parent.destroy = aesgcm_free;
    state->parent.init_key = aesgcm_init_key;
    state->parent.encrypt = aesgcm_encrypt;
    state->parent.decrypt = aesgcm_decrypt;
    return &state->parent;
}
