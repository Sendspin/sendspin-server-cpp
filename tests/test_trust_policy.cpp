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

/// @file test_trust_policy.cpp
/// @brief The PSK a handshake references and the activity sets a matched PSK allows

#include "trust_fixtures.h"
#include "trust_policy.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace {

using sendspin::server::Activities;
using sendspin::server::Psk;
using sendspin::server::PskCategory;
using sendspin::server::test::psk_of;

// A long-term PSK on record, then an operator-started pairing PSK, then the Sentinel (connection.md
// "Pre-Shared Key": the Sentinel whenever no other PSK applies)
TEST(HandshakePsk, ReferencesTheLongTermThenThePairingThenTheSentinel) {
    struct Row {
        const char* name;
        std::optional<Psk> long_term;
        std::optional<Psk> pairing;
        PskCategory category;
        Psk psk;
    };
    const Row rows[] = {
        {"Control: neither", std::nullopt, std::nullopt, PskCategory::SENTINEL,
         sendspin::server::SENTINEL_PSK},
        {"a long-term PSK", psk_of(1), std::nullopt, PskCategory::LONG_TERM, psk_of(1)},
        {"a pairing PSK", std::nullopt, psk_of(2), PskCategory::PAIRING, psk_of(2)},
        {"both: the long-term PSK", psk_of(1), psk_of(2), PskCategory::LONG_TERM, psk_of(1)},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const sendspin::server::HandshakePsk selected = sendspin::server::select_handshake_psk(
            row.long_term ? &*row.long_term : nullptr, row.pairing ? &*row.pairing : nullptr);
        EXPECT_EQ(selected.category, row.category);
        EXPECT_EQ(selected.psk, row.psk);
    }
}

// messaging.md "server/activate": the allowed activity sets per matched PSK, and
// playback-capable connections, the sets that stay allowed with 'playback' added
TEST(ActivitySets, AllowedPerMatchedPsk) {
    struct Row {
        PskCategory matched;
        Activities activities;
        bool unpaired_access;
        bool allowed;
        bool capable;
    };
    const Activities none{false, false};
    const Activities playback{true, false};
    const Activities pairing{false, true};
    const Activities both{true, true};
    const Row rows[] = {
        // Control: long-term `[]` and `['playback']`, with unpaired access off
        {PskCategory::LONG_TERM, none, false, true, true},
        {PskCategory::LONG_TERM, playback, false, true, true},
        {PskCategory::LONG_TERM, pairing, true, false, false},
        {PskCategory::LONG_TERM, both, true, false, false},
        // Control: the Sentinel's `[]` and `['pairing']` need no unpaired access
        {PskCategory::SENTINEL, none, false, true, false},
        {PskCategory::SENTINEL, pairing, false, true, false},
        {PskCategory::SENTINEL, playback, false, false, false},
        {PskCategory::SENTINEL, both, false, false, false},
        {PskCategory::SENTINEL, none, true, true, true},
        {PskCategory::SENTINEL, playback, true, true, true},
        {PskCategory::SENTINEL, both, true, true, true},
        {PskCategory::PAIRING, pairing, false, true, false},
        {PskCategory::PAIRING, playback, false, false, false},
        {PskCategory::PAIRING, playback, true, true, true},
        {PskCategory::PAIRING, both, true, true, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(std::string(sendspin::server::to_wire(row.matched)) + " playback " +
                     std::to_string(row.activities.playback) + " pairing " +
                     std::to_string(row.activities.pairing) + " unpaired access " +
                     std::to_string(row.unpaired_access));
        EXPECT_EQ(sendspin::server::activity_set_allowed(row.matched, row.activities,
                                                         row.unpaired_access),
                  row.allowed);
        EXPECT_EQ(
            sendspin::server::playback_capable(row.matched, row.activities, row.unpaired_access),
            row.capable);
    }
}

}  // namespace
