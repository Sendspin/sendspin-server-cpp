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

/// @file ws_handshake.h
/// @brief The client's opening handshake (RFC 6455 section 4.1) for a transport that dials
/// without a WebSocket library (the ESP-IDF one): the upgrade request and the check of the
/// server's response. Platform-free and host-tested.

#pragma once

#include "transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sendspin::server {

/// @brief The random nonce a Sec-WebSocket-Key encodes (RFC 6455 section 4.1)
constexpr size_t WS_KEY_NONCE_BYTES = 16;

/// @brief The longest upgrade response taken, its blank line included; a longer one fails the
/// dial. A 101 is about 200 bytes, so this leaves room for a server's extra headers while bounding
/// a hostile one, and sizes each dialed connection's read buffer.
constexpr size_t WS_UPGRADE_RESPONSE_MAX = 2048;

/// @brief The Sec-WebSocket-Key for nonce: its Base64
std::string ws_handshake_key(const std::array<uint8_t, WS_KEY_NONCE_BYTES>& nonce);

/// @brief The Sec-WebSocket-Accept a server answers key with (RFC 6455 section 4.2.2)
std::string ws_accept_value(std::string_view key);

/// @brief The Host header's value for target: host:port, an IPv6 literal bracketed and without
/// its zone (RFC 6874 section 4: a ZoneID has only local significance)
std::string ws_host_value(const DialTarget& target);

/// @brief The upgrade GET for target with key: Host as ws_host_value(), and no subprotocol or
/// extension offered
std::string ws_upgrade_request(const DialTarget& target, std::string_view key);

enum class WsUpgradeStatus : uint8_t {
    INCOMPLETE,  ///< No blank line yet, within WS_UPGRADE_RESPONSE_MAX
    ACCEPTED,
    REJECTED,  ///< The dial fails
};

struct WsUpgradeCheck {
    WsUpgradeStatus status{WsUpgradeStatus::INCOMPLETE};
    /// For ACCEPTED: the response's length, its blank line included; the bytes after it are
    /// frames
    size_t response_bytes{0};
};

/// @brief Checks the bytes received so far for the response to an upgrade sent with key (RFC
/// 6455 section 4.1): lines ending only in CRLF, with no NUL; status 101 on HTTP/1.1 (a redirect
/// or any other status is REJECTED); every field name a token, and every field value and the
/// reason phrase only SP, HTAB, visible characters and obs-text (RFC 9110 section 5.5, RFC 9112
/// section 4); one Upgrade line of `websocket` and Connection `Upgrade` (on any of its lines) in
/// any case, one Sec-WebSocket-Accept line of the value key requires, and no
/// Sec-WebSocket-Extensions or Sec-WebSocket-Protocol, as none was offered
WsUpgradeCheck check_upgrade_response(std::string_view received, std::string_view key);

}  // namespace sendspin::server
