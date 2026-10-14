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

/// @file url_text.h
/// @brief The text a dial URL's bracketed host and a browsed address are checked against: RFC 3986
/// character classes, IPv4 and IPv6 address text and an RFC 6874 zone identifier, as pure functions
/// with no platform parser, so every transport takes the same forms

#pragma once

#include <string_view>

namespace sendspin::server {

/// @brief RFC 3986 HEXDIG, in either case
bool is_hex_digit(char c);

/// @brief RFC 3986 section 2.3
bool is_unreserved(char c);

/// @brief RFC 3986 section 3.2.2 `IPv4address`: four decimal octets of 0 to 255, each without a
/// leading zero (`dec-octet`)
bool is_ipv4_text(std::string_view text);

/// @brief An RFC 4291 section 2.2 text form: groups of 1 to 4 hex digits separated by ':', at
/// most one "::" standing for one or more zero groups, eight groups without it, and optionally a
/// final is_ipv4_text() part counting as two groups (RFC 3986 section 3.2.2 `IPv6address`)
bool is_ipv6_text(std::string_view text);

/// @brief RFC 6874 section 2 `ZoneID`: one or more unreserved characters
bool is_zone_id(std::string_view text);

/// @brief A literal address as browsing reports one: is_ipv4_text(), or is_ipv6_text() with an
/// optional `%` and is_zone_id()
bool is_ip_address_text(std::string_view text);

}  // namespace sendspin::server
