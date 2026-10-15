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

/// @file trust_fixtures.h
/// @brief For tests of trust and pairing: distinct PSKs and the client record a server saved

#pragma once

#include "client_records.h"
#include "crypto/keys.h"
#include "test_recorder.h"

#include <sendspin/server/persistence_keys.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin::server::test {

/// A PSK distinct for each value of byte
inline Psk psk_of(uint8_t byte) {
    Psk psk{};
    for (size_t i = 0; i < psk.size(); ++i) {
        psk[i] = static_cast<uint8_t>(byte + i);
    }
    return psk;
}

/// The record a provider holds in slot
inline std::optional<ClientRecord> stored_record(const MemoryProvider& provider, size_t slot) {
    const auto it = provider.blobs.find(persistence_keys::client_record_key(slot));
    return it == provider.blobs.end() ? std::nullopt : decode_client_record(it->second);
}

}  // namespace sendspin::server::test
