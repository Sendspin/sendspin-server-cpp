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

// parse_ws_url(), shared by every transport implementation

#include "transport.h"
#include "url_text.h"
#include <arpa/inet.h>
#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace sendspin::server {

namespace {

constexpr uint16_t DEFAULT_WS_PORT = 80;
/// RFC 4291 section 2: 128 bits
constexpr size_t IPV6_ADDRESS_BYTES = 16;

/// Whether text holds only unreserved characters, sub-delims, percent-encodings (RFC 3986 section
/// 2) and the characters of extra, so no control character or space reaches the upgrade request
bool only_url_characters(std::string_view text, std::string_view extra) {
    constexpr std::string_view SUB_DELIMS = "!$&'()*+,;=";
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size() || !is_hex_digit(text[i + 1]) || !is_hex_digit(text[i + 2])) {
                return false;
            }
            i += 2;
            continue;
        }
        if (!is_unreserved(c) && SUB_DELIMS.find(c) == std::string_view::npos &&
            extra.find(c) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

/// The host an IP literal (RFC 3986 section 3.2.2) names, its zone identifier as the system
/// resolver takes it (address%zone): written raw, or as RFC 6874 section 2 encodes it ("%25"
/// first, so a raw zone that starts with "25" reads as encoded)
std::optional<std::string> ip_literal_host(std::string_view literal) {
    const size_t percent = literal.find('%');
    const std::string_view address = literal.substr(0, percent);
    // Checked as text first, so no platform's inet_pton takes more (lwIP's takes malformed
    // groups); it then refuses every form on a build without IPv6, and lwIP's some IPv4 tails
    if (!is_ipv6_text(address)) {
        return std::nullopt;
    }
    std::string host(address);
    std::array<uint8_t, IPV6_ADDRESS_BYTES> bytes{};
    if (inet_pton(AF_INET6, host.c_str(), bytes.data()) != 1) {
        return std::nullopt;
    }
    if (percent == std::string_view::npos) {
        return host;
    }
    std::string_view zone = literal.substr(percent + 1);
    constexpr std::string_view ENCODED_PERCENT = "25";
    if (zone.starts_with(ENCODED_PERCENT)) {
        zone.remove_prefix(ENCODED_PERCENT.size());
    }
    if (!is_zone_id(zone)) {
        return std::nullopt;
    }
    host += '%';
    host += zone;
    return host;
}

}  // namespace

std::optional<DialTarget> parse_ws_url(std::string_view url) {
    constexpr std::string_view SCHEME = "ws://";
    if (!url.starts_with(SCHEME)) {
        return std::nullopt;
    }
    url.remove_prefix(SCHEME.size());
    const size_t slash = url.find('/');
    const std::string_view authority = url.substr(0, slash);
    if (url.find('#') != std::string_view::npos ||
        authority.find_first_of("@?") != std::string_view::npos) {
        return std::nullopt;
    }

    DialTarget target;
    target.path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    // RFC 3986 section 3.3, with the query (section 3.4) the request target carries
    if (!only_url_characters(target.path, ":@/?")) {
        return std::nullopt;
    }
    std::optional<std::string_view> port_text;
    if (authority.starts_with('[')) {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        std::optional<std::string> host = ip_literal_host(authority.substr(1, close - 1));
        if (!host) {
            return std::nullopt;
        }
        target.host = std::move(*host);
        const std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') {
                return std::nullopt;
            }
            port_text = rest.substr(1);
        }
    } else {
        const size_t colon = authority.find(':');
        target.host = authority.substr(0, colon);
        // RFC 3986 section 3.2.2: a registered name or an IPv4 address
        if (!only_url_characters(target.host, "")) {
            return std::nullopt;
        }
        if (colon != std::string_view::npos) {
            port_text = authority.substr(colon + 1);
        }
    }
    if (target.host.empty()) {
        return std::nullopt;
    }

    target.port = DEFAULT_WS_PORT;
    if (port_text) {
        constexpr uint32_t DECIMAL_BASE = 10;
        uint32_t value = 0;
        for (const char digit : *port_text) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            value = (value * DECIMAL_BASE) + static_cast<uint32_t>(digit - '0');
            if (value > std::numeric_limits<uint16_t>::max()) {
                return std::nullopt;
            }
        }
        if (value == 0) {
            return std::nullopt;
        }
        target.port = static_cast<uint16_t>(value);
    }
    return target;
}

}  // namespace sendspin::server
