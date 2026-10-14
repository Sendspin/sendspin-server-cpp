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

/// @file discovery_policy.h
/// @brief Which clients the server dials on its own, with which intent, and what a connection's end
/// does to that; and the mDNS values it advertises and takes from browsing. Pure functions.

#pragma once

#include "sendspin/server/discovery.h"
#include "sendspin/server/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief What a dial is for
enum class DialIntent : uint8_t {
    /// Browsing, or a reconnect: the client is held without displacing another server
    IDLE,
    /// The operator's dial, or a reconnect into a group that plays
    PLAYBACK,
    /// The operator's pairing of a client not connected: the first activation is the pairing one
    PAIRING,
};

enum class ConnectionOrigin : uint8_t {
    /// The server dialed the client (spec connection.md "Server Initiated Connections")
    DIALED,
    /// The client connected to the server (spec connection.md "Client Initiated Connections")
    ACCEPTED,
};

/// @brief Whether a client that may play declares `playback` in its first activation, the one it
/// arbitrates (spec connection.md "Multiple servers (server-initiated)"): a dial for playback does;
/// an idle dial or one for pairing does not; a client that connected does while its group plays
bool first_activation_playback(ConnectionOrigin origin, DialIntent intent, bool group_playing);

/// @brief Whether a later activation of a client that may play declares `playback`: while its
/// group plays (spec messaging.md "server/activate": the minimal set of activities)
bool later_activation_playback(bool group_playing);

/// @brief A client found by browsing
struct DiscoveryEntry {
    DiscoveredClient service;
    /// Still advertised: a lost instance's entry is kept only while its client is connected
    bool present{true};
    /// A client/goodbye asked the server not to reconnect; cleared when the instance is lost and
    /// found again, or by the operator's dial
    bool suppressed{false};
};

/// @brief The activities the last activation of a connection declared
enum class DeclaredActivities : uint8_t { NONE, PLAYBACK, PAIRING_ONLY };

/// @brief How a connection to a dialed client ended
struct ConnectionEnd {
    /// The initial server/activate was queued
    bool activated{false};
    /// A client/goodbye arrived; reason nullopt for one the server does not recognize
    bool goodbye{false};
    std::optional<GoodbyeReason> reason;
    DeclaredActivities activities{DeclaredActivities::NONE};
    /// The host closed it (SendspinServer::disconnect()), or stopped dialing its target
    /// (SendspinServer::cancel_connect_url(), disconnect()) while it was open
    bool stopped_by_host{false};
};

/// @brief What a connection's end does to the server dialing the client again on its own
enum class Reconnect : uint8_t {
    /// Dial again, after a wait that grows with each consecutive such end
    AGAIN,
    /// A failed handshake: counts toward the backoff and changes nothing else
    UNCHANGED,
    /// Not until the operator dials, or a client found by browsing advertises again
    STOP,
};

/// @brief From spec messaging.md "client/goodbye": `restart`, a drop on a connection declaring `[]`
/// or `playback`, and `concurrent_attempt` (retried after the wait) dial again; every other reason,
/// an unrecognized one, a drop of a pairing-only connection and the host's disconnect (an operator
/// action) stop it
Reconnect reconnect_after(const ConnectionEnd& end);

/// @brief Where a target to dial stands
struct DialState {
    /// Found by browsing (DiscoveryEntry), rather than by connect_url()
    bool discovered{false};
    /// DiscoveryEntry::present
    bool present{false};
    /// DiscoveryEntry::suppressed
    bool suppressed{false};
    /// A target reached by connect_url() whose last connection ended with Reconnect::AGAIN
    bool reconnecting{false};
    /// Connected, dialing, or waiting out its backoff before a dial
    bool busy{false};
    /// The client the target reached last is connected through another connection
    bool connected_elsewhere{false};
    /// A connect_url() target asked to retry until connected (ConnectOptions) that has not been
    /// activated since
    bool retrying{false};
};

/// @brief Whether the server dials a target on its own now: a discovered client that still
/// advertises unless suppressed, or a connect_url() target that reconnects or retries; never one
/// busy or whose client is connected already
bool dials_on_its_own(const DialState& state);

/// @brief Limits on what browsing reports, which comes from the network
namespace discovery_limits {
/// Discovered clients the server keeps; later ones are ignored until one is lost
constexpr size_t MAX_CLIENTS = 256;
/// Addresses kept per client, in the order reported
constexpr size_t MAX_ADDRESSES = 8;
/// A DNS name or label as text, with escapes (RFC 1035 section 2.3.4)
constexpr size_t MAX_NAME_BYTES = 1024;
}  // namespace discovery_limits

/// @brief Whether path is a WebSocket path the server serves or dials: '/' and then printable
/// ASCII other than '?' and '#', at most max_bytes
bool is_valid_path(std::string_view path, size_t max_bytes);

/// @brief client as the server keeps it: only the addresses is_ip_address_text() takes
/// (`url_text.h`), at most discovery_limits::MAX_ADDRESSES of them; nullopt when its instance is
/// empty or too long, its host is too long, holds a control character, a space or ':' or is dots
/// only, it has no such address or no port, or its path or name is not a valid TXT value (spec
/// connection.md "Server Initiated Connections": `path` is required). An empty host is kept: the
/// dial names the first address instead.
std::optional<DiscoveredClient> validated(const DiscoveredClient& client);

/// @brief The `_sendspin-server._tcp` advertisement for a server named name (spec connection.md
/// "Client Initiated Connections"): the instance is the name cut to 63 bytes, or "Sendspin
/// Server" for none, and TXT `name` the name cut to fit its TXT string; cuts fall between UTF-8
/// characters
AdvertisedServer advertised_server(std::string_view name, uint16_t port, std::string_view path);

/// @brief text cut to at most max_bytes, between UTF-8 characters
std::string utf8_prefix(std::string_view text, size_t max_bytes);

/// @brief client's first address and its port, as "192.0.2.1:8928" or "[2001:db8::1]:8928"
std::string discovered_address(const DiscoveredClient& client);

/// @brief The URL a dial to client requests: `ws://`, discovered_address() and its path
std::string discovered_url(const DiscoveredClient& client);

}  // namespace sendspin::server
