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

/// @file test_discovery_policy.cpp
/// @brief The dialing and discovery policy as tables: the first and later activations by
/// connection and dial intent, the reconnect policy per connection end, when a target is dialed
/// on its own, and the mDNS values the server advertises and keeps

#include "discovery_policy.h"
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::ConnectionEnd;
using sendspin::server::ConnectionOrigin;
using sendspin::server::DeclaredActivities;
using sendspin::server::DialIntent;
using sendspin::server::DialState;
using sendspin::server::DiscoveredClient;
using sendspin::server::GoodbyeReason;
using sendspin::server::Reconnect;

// A dial for playback declares it, an idle dial does not, a client that connected does while its
// group plays; later activations follow the group (spec messaging.md "server/activate": the minimal
// set of activities)
TEST(DialPolicy, FirstAndLaterActivationsFollowTheTable) {
    struct Row {
        const char* name;
        ConnectionOrigin origin;
        DialIntent intent;
        bool playing;
        bool playback;
    };
    const Row rows[] = {
        {"Control: a dial for playback, group stopped", ConnectionOrigin::DIALED,
         DialIntent::PLAYBACK, false, true},
        {"a dial for playback, group playing", ConnectionOrigin::DIALED, DialIntent::PLAYBACK, true,
         true},
        {"an idle dial, group stopped", ConnectionOrigin::DIALED, DialIntent::IDLE, false, false},
        {"an idle dial, group playing", ConnectionOrigin::DIALED, DialIntent::IDLE, true, false},
        {"a dial for pairing, group playing", ConnectionOrigin::DIALED, DialIntent::PAIRING, true,
         false},
        {"a client that connected, group stopped", ConnectionOrigin::ACCEPTED, DialIntent::IDLE,
         false, false},
        {"a client that connected, group playing", ConnectionOrigin::ACCEPTED, DialIntent::IDLE,
         true, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::first_activation_playback(row.origin, row.intent, row.playing),
                  row.playback);
    }
    EXPECT_TRUE(sendspin::server::later_activation_playback(true));
    EXPECT_FALSE(sendspin::server::later_activation_playback(false));
}

// From spec messaging.md "client/goodbye": every reason, a goodbye before activation, a drop by the
// connection's activities, and a failed handshake
TEST(DialPolicy, ReconnectsPerGoodbyeReasonAndDrop) {
    struct Row {
        const char* name;
        ConnectionEnd end;
        Reconnect reconnect;
    };
    const auto goodbye = [](std::optional<GoodbyeReason> reason) {
        return ConnectionEnd{true, true, reason, DeclaredActivities::PLAYBACK};
    };
    const Row rows[] = {
        {"Control: restart", goodbye(GoodbyeReason::RESTART), Reconnect::AGAIN},
        {"concurrent_attempt, retried after the wait", goodbye(GoodbyeReason::CONCURRENT_ATTEMPT),
         Reconnect::AGAIN},
        {"another_server", goodbye(GoodbyeReason::ANOTHER_SERVER), Reconnect::STOP},
        {"shutdown", goodbye(GoodbyeReason::SHUTDOWN), Reconnect::STOP},
        {"user_request", goodbye(GoodbyeReason::USER_REQUEST), Reconnect::STOP},
        {"unpaired", goodbye(GoodbyeReason::UNPAIRED), Reconnect::STOP},
        {"unauthorized", goodbye(GoodbyeReason::UNAUTHORIZED), Reconnect::STOP},
        {"pairing_required", goodbye(GoodbyeReason::PAIRING_REQUIRED), Reconnect::STOP},
        {"an unrecognized reason", goodbye(std::nullopt), Reconnect::STOP},
        {"restart before activation",
         {false, true, GoodbyeReason::RESTART, DeclaredActivities::NONE},
         Reconnect::AGAIN},
        {"shutdown before activation",
         {false, true, GoodbyeReason::SHUTDOWN, DeclaredActivities::NONE},
         Reconnect::STOP},
        {"a drop declaring []",
         {true, false, std::nullopt, DeclaredActivities::NONE},
         Reconnect::AGAIN},
        {"a drop declaring playback",
         {true, false, std::nullopt, DeclaredActivities::PLAYBACK},
         Reconnect::AGAIN},
        {"a drop declaring pairing only",
         {true, false, std::nullopt, DeclaredActivities::PAIRING_ONLY},
         Reconnect::STOP},
        {"a failed handshake",
         {false, false, std::nullopt, DeclaredActivities::NONE},
         Reconnect::UNCHANGED},
        {"the host's disconnect of a playing connection",
         {true, false, std::nullopt, DeclaredActivities::PLAYBACK, true},
         Reconnect::STOP},
        {"a restart goodbye after the host stopped dialing the target",
         {true, true, GoodbyeReason::RESTART, DeclaredActivities::PLAYBACK, true},
         Reconnect::STOP},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::reconnect_after(row.end), row.reconnect);
    }
}

// A discovered client is dialed unless suppressed, a URL only while it reconnects or retries until
// connected; neither while busy or while its client is connected another way
TEST(DialPolicy, DialsDiscoveredClientsUnlessSuppressed) {
    struct Row {
        const char* name;
        DialState state;
        bool dials;
    };
    const Row rows[] = {
        {"Control: discovered and advertised", {true, true, false, false, false, false}, true},
        {"discovered, suppressed", {true, true, true, false, false, false}, false},
        {"discovered, its instance lost", {true, false, false, true, false, false}, false},
        {"discovered, busy", {true, true, false, false, true, false}, false},
        {"discovered, connected elsewhere", {true, true, false, false, false, true}, false},
        {"Control: a URL that reconnects", {false, false, false, true, false, false}, true},
        {"a URL the operator dialed once", {false, false, false, false, false, false}, false},
        {"a URL that reconnects, busy", {false, false, false, true, true, false}, false},
        {"a URL that reconnects, connected elsewhere",
         {false, false, false, true, false, true},
         false},
        {"Control: a URL that retries until connected",
         {false, false, false, false, false, false, true},
         true},
        {"a URL that retries, busy", {false, false, false, false, true, false, true}, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::dials_on_its_own(row.state), row.dials);
    }
}

DiscoveredClient service() {
    DiscoveredClient client;
    client.instance = "Kitchen";
    client.addresses = {"192.0.2.7"};
    client.host = "kitchen.local.";
    client.port = 8928;
    client.path = "/sendspin";
    client.name = "Kitchen";
    return client;
}

// spec connection.md "Server Initiated Connections": `path` is required; what the network
// reports is kept only within the TXT and DNS limits, with a bounded list of literal addresses
TEST(DiscoveryValues, KeepsOnlyValidBrowseResults) {
    struct Row {
        const char* name;
        void (*edit)(DiscoveredClient&);
        bool kept;
        // For a kept result: its addresses, when not those given
        std::vector<std::string> addresses;
    };
    const Row rows[] = {
        {"Control: a complete result", [](DiscoveredClient&) {}, true, {}},
        {"Control: no TXT name", [](DiscoveredClient& c) { c.name.reset(); }, true, {}},
        {"no instance", [](DiscoveredClient& c) { c.instance.clear(); }, false, {}},
        {"Control: a host with UTF-8",
         [](DiscoveredClient& c) {
             c.host = "k\xC3\xBC"
                      "che.local.";
         },
         true,
         {}},
        {"a host with a line break",
         [](DiscoveredClient& c) { c.host = "kitchen.local.\r\nX: y"; },
         false,
         {}},
        {"a host with a space", [](DiscoveredClient& c) { c.host = "kit chen.local."; }, false, {}},
        {"a host with DEL", [](DiscoveredClient& c) { c.host = "kitchen\x7F.local."; }, false, {}},
        {"no address", [](DiscoveredClient& c) { c.addresses.clear(); }, false, {}},
        // The dial's addresses, the first of them the Host header's for no host
        {"Control: no host", [](DiscoveredClient& c) { c.host.clear(); }, true, {}},
        {"the root as the host", [](DiscoveredClient& c) { c.host = "."; }, false, {}},
        {"a host of dots only", [](DiscoveredClient& c) { c.host = ".."; }, false, {}},
        {"a host holding ':'", [](DiscoveredClient& c) { c.host = "fe80::1"; }, false, {}},
        {"Control: IPv4 and IPv6 with a zone",
         [](DiscoveredClient& c) { c.addresses = {"192.0.2.7", "fe80::1%en0"}; },
         true,
         {}},
        {"an address with a line break",
         [](DiscoveredClient& c) {
             c.host.clear();
             c.addresses = {"1.2.3.4\r\nX-Injected: 1"};
         },
         false,
         {}},
        {"an empty address", [](DiscoveredClient& c) { c.addresses = {""}; }, false, {}},
        {"a name as the address",
         [](DiscoveredClient& c) { c.addresses = {"kitchen.local"}; },
         false,
         {}},
        {"a malformed address beside a literal one",
         [](DiscoveredClient& c) { c.addresses = {"1.2.3.4\r\nX: y", "fffff::1", "192.0.2.8"}; },
         true,
         {"192.0.2.8"}},
        {"no port", [](DiscoveredClient& c) { c.port = 0; }, false, {}},
        {"no path", [](DiscoveredClient& c) { c.path.clear(); }, false, {}},
        {"a path without '/'", [](DiscoveredClient& c) { c.path = "sendspin"; }, false, {}},
        {"a path with a query", [](DiscoveredClient& c) { c.path = "/a?b"; }, false, {}},
        {"a path over its TXT string",
         [](DiscoveredClient& c) { c.path = "/" + std::string(250, 'p'); },
         false,
         {}},
        {"a name over its TXT string",
         [](DiscoveredClient& c) { c.name = std::string(251, 'n'); },
         false,
         {}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        DiscoveredClient client = service();
        row.edit(client);
        const std::optional<DiscoveredClient> kept = sendspin::server::validated(client);
        ASSERT_EQ(kept.has_value(), row.kept);
        if (kept) {
            EXPECT_EQ(kept->addresses, row.addresses.empty() ? client.addresses : row.addresses);
        }
    }
    DiscoveredClient many = service();
    many.addresses.assign(sendspin::server::discovery_limits::MAX_ADDRESSES + 1, "192.0.2.1");
    many.addresses.front() = "192.0.2.9";
    // Dropped before the cap, so it takes no literal address's place
    many.addresses.insert(many.addresses.begin(), "kitchen.local");
    const std::optional<DiscoveredClient> kept = sendspin::server::validated(many);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->addresses.size(), sendspin::server::discovery_limits::MAX_ADDRESSES);
    EXPECT_EQ(kept->addresses.front(), "192.0.2.9");
}

TEST(DiscoveryValues, BuildsTheDialUrl) {
    DiscoveredClient client = service();
    EXPECT_EQ(sendspin::server::discovered_url(client), "ws://192.0.2.7:8928/sendspin");
    client.addresses = {"fe80::1%en0"};
    EXPECT_EQ(sendspin::server::discovered_address(client), "[fe80::1%en0]:8928");
}

// spec connection.md "Client Initiated Connections": TXT `path` and `name`; RFC 6763 bounds the
// instance to one DNS label and a TXT string to 255 bytes, cut between UTF-8 characters
TEST(DiscoveryValues, AdvertisesTheServerWithinTheMdnsLimits) {
    const sendspin::server::AdvertisedServer plain =
        sendspin::server::advertised_server("Living Room", 8927, "/sendspin");
    EXPECT_EQ(plain.instance, "Living Room");
    EXPECT_EQ(plain.name, "Living Room");
    EXPECT_EQ(plain.port, 8927);
    EXPECT_EQ(plain.path, "/sendspin");

    const sendspin::server::AdvertisedServer unnamed =
        sendspin::server::advertised_server("", 8927, "/sendspin");
    EXPECT_EQ(unnamed.instance, "Sendspin Server");
    EXPECT_EQ(unnamed.name, "");

    // 62 ASCII bytes, then a two-byte character that would end past the 63rd byte
    const std::string long_name = std::string(62, 'a') + "\xC3\xA9" + std::string(300, 'b');
    const sendspin::server::AdvertisedServer cut =
        sendspin::server::advertised_server(long_name, 8927, "/sendspin");
    EXPECT_EQ(cut.instance, std::string(62, 'a'));
    EXPECT_EQ(cut.name.size(), 250U);
    EXPECT_EQ(cut.name, long_name.substr(0, 250));
    EXPECT_EQ(sendspin::server::utf8_prefix("ab\xC3\xA9", 4), "ab\xC3\xA9");
    EXPECT_EQ(sendspin::server::utf8_prefix("ab\xC3\xA9", 3), "ab");
}

TEST(DiscoveryValues, ChecksPaths) {
    struct Row {
        const char* path;
        bool valid;
    };
    const Row rows[] = {
        {"/sendspin", true}, {"/", true},     {"", false},     {"sendspin", false},
        {"/a b", false},     {"/a#b", false}, {"/a?b", false}, {"/\x7f", false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.path);
        EXPECT_EQ(sendspin::server::is_valid_path(row.path, 250), row.valid);
    }
    EXPECT_FALSE(sendspin::server::is_valid_path("/ab", 2));
}

}  // namespace
