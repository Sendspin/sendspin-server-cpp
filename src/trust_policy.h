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

/// @file trust_policy.h
/// @brief Which PSK a handshake references and which activity sets a matched PSK allows, as pure
/// functions (spec connection.md "Pre-Shared Key", messaging.md "server/activate")

#pragma once

#include "crypto/keys.h"
#include "noise.h"

#include <optional>

namespace sendspin::server {

/// @brief The PSK a session's keys were derived from
struct PskMatch {
    PskCategory category{PskCategory::SENTINEL};
    /// Set when message 2 failed under the referenced PSK, this category's, and validated under
    /// the Sentinel: an authenticated credential-mismatch signal (connection.md "Sentinel
    /// Fallback"). A re-handshake clears it.
    std::optional<PskCategory> fallback_from;

    bool operator==(const PskMatch&) const = default;
};

/// @brief The PSK message 1 references: the client's long-term PSK when the server holds one, else
/// the pairing PSK of a Pairing PSK pairing the operator started for the client, else the Sentinel
/// @param long_term, pairing nullptr for none
HandshakePsk select_handshake_psk(const Psk* long_term, const Psk* pairing);

/// @brief A server/activate's `activities`
struct Activities {
    bool playback{false};
    bool pairing{false};
};

/// @brief Whether activities is an allowed set for the matched PSK (messaging.md "server/activate"
/// table): long-term `[]` or `['playback']`; pairing and Sentinel also `['pairing']`, and the sets
/// with `'playback'` only with unpaired access enabled
bool activity_set_allowed(PskCategory matched, Activities activities, bool unpaired_access);

/// @brief Whether a connection declaring activities is playback-capable: they are an allowed set
/// once extended with `'playback'` (messaging.md "server/activate"), so it may carry roles
bool playback_capable(PskCategory matched, Activities activities, bool unpaired_access);

}  // namespace sendspin::server
