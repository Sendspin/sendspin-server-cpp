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

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sendspin::server {

namespace {

constexpr uint16_t DEFAULT_WS_PORT = 80;

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
    std::optional<std::string_view> port_text;
    if (authority.starts_with('[')) {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        target.host = authority.substr(1, close - 1);
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
