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

#include "discovery_policy.h"

#include "url_text.h"

#include <algorithm>

namespace sendspin::server {

namespace {

/// RFC 6763 section 4.1.1: an instance name is one DNS label
constexpr size_t MAX_INSTANCE_BYTES = 63;
/// RFC 6763 section 6.1: a TXT string holds 255 bytes, `key=` included
constexpr size_t MAX_TXT_VALUE_BYTES = 250;
constexpr std::string_view DEFAULT_INSTANCE = "Sendspin Server";

}  // namespace

bool first_activation_playback(ConnectionOrigin origin, DialIntent intent, bool group_playing) {
    if (origin == ConnectionOrigin::ACCEPTED) {
        return group_playing;
    }
    return intent == DialIntent::PLAYBACK;
}

bool later_activation_playback(bool group_playing) {
    return group_playing;
}

Reconnect reconnect_after(const ConnectionEnd& end) {
    if (end.stopped_by_host) {
        return Reconnect::STOP;
    }
    if (!end.goodbye) {
        if (!end.activated) {
            // messaging.md "client/goodbye": backoff on repeated handshake failures
            return Reconnect::UNCHANGED;
        }
        // A drop is taken as a restart, except on a connection only for pairing
        return end.activities == DeclaredActivities::PAIRING_ONLY ? Reconnect::STOP
                                                                  : Reconnect::AGAIN;
    }
    if (!end.reason) {
        return Reconnect::STOP;
    }
    switch (*end.reason) {
        case GoodbyeReason::RESTART:
        // The client refused it for another server's: retried later, with the wait
        case GoodbyeReason::CONCURRENT_ATTEMPT:
            return Reconnect::AGAIN;
        // ANOTHER_SERVER leaves the client available to connect(); UNAUTHORIZED is a server bug,
        // not retried with the same activities; PAIRING_REQUIRED waits for pairing
        case GoodbyeReason::ANOTHER_SERVER:
        case GoodbyeReason::SHUTDOWN:
        case GoodbyeReason::USER_REQUEST:
        case GoodbyeReason::UNAUTHORIZED:
        case GoodbyeReason::PAIRING_REQUIRED:
        case GoodbyeReason::UNPAIRED:
            return Reconnect::STOP;
    }
    return Reconnect::STOP;
}

bool dials_on_its_own(const DialState& state) {
    if (state.busy || state.connected_elsewhere) {
        return false;
    }
    if (state.discovered) {
        return state.present && !state.suppressed;
    }
    return state.reconnecting || state.retrying;
}

bool is_valid_path(std::string_view path, size_t max_bytes) {
    if (path.empty() || path.front() != '/' || path.size() > max_bytes) {
        return false;
    }
    constexpr char FIRST_PRINTABLE = '!';
    constexpr char LAST_PRINTABLE = '~';
    return std::all_of(path.begin(), path.end(), [](char c) {
        return c >= FIRST_PRINTABLE && c <= LAST_PRINTABLE && c != '?' && c != '#';
    });
}

std::optional<DiscoveredClient> validated(const DiscoveredClient& client) {
    // The host goes into the upgrade request's Host header
    constexpr char FIRST_PRINTABLE = '!';
    constexpr char DELETE = '\x7F';
    const bool host_printable = std::all_of(client.host.begin(), client.host.end(), [](char c) {
        return static_cast<unsigned char>(c) >= FIRST_PRINTABLE && c != DELETE;
    });
    // A name of dots only leaves no host or only dots once the dial drops its final dot, and a
    // ':' would be bracketed as an IPv6 literal; a DNS name holds neither (RFC 1035 section 2.3.1)
    const bool dots_only =
        !client.host.empty() && client.host.find_first_not_of('.') == std::string::npos;
    const bool host_name = !dots_only && client.host.find(':') == std::string::npos;
    if (client.instance.empty() || client.instance.size() > discovery_limits::MAX_NAME_BYTES ||
        client.host.size() > discovery_limits::MAX_NAME_BYTES || !host_printable || !host_name ||
        client.port == 0 || !is_valid_path(client.path, MAX_TXT_VALUE_BYTES) ||
        (client.name && client.name->size() > MAX_TXT_VALUE_BYTES)) {
        return std::nullopt;
    }
    DiscoveredClient kept = client;
    // Each address is dialed as a literal, and the first goes into the Host header for no host
    std::erase_if(kept.addresses,
                  [](const std::string& address) { return !is_ip_address_text(address); });
    if (kept.addresses.empty()) {
        return std::nullopt;
    }
    if (kept.addresses.size() > discovery_limits::MAX_ADDRESSES) {
        kept.addresses.resize(discovery_limits::MAX_ADDRESSES);
    }
    return kept;
}

std::string utf8_prefix(std::string_view text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    // Back off continuation bytes (10xxxxxx), so the cut falls before a character's first byte
    constexpr unsigned char CONTINUATION_MASK = 0xC0;
    constexpr unsigned char CONTINUATION = 0x80;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & CONTINUATION_MASK) == CONTINUATION) {
        --end;
    }
    return std::string(text.substr(0, end));
}

AdvertisedServer advertised_server(std::string_view name, uint16_t port, std::string_view path) {
    AdvertisedServer server;
    server.instance =
        name.empty() ? std::string(DEFAULT_INSTANCE) : utf8_prefix(name, MAX_INSTANCE_BYTES);
    server.port = port;
    server.path = std::string(path);
    server.name = utf8_prefix(name, MAX_TXT_VALUE_BYTES);
    return server;
}

std::string discovered_address(const DiscoveredClient& client) {
    const std::string& address = client.addresses.front();
    const std::string host = address.find(':') == std::string::npos ? address : "[" + address + "]";
    return host + ":" + std::to_string(client.port);
}

std::string discovered_url(const DiscoveredClient& client) {
    return "ws://" + discovered_address(client) + client.path;
}

}  // namespace sendspin::server
