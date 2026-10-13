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

#include "url_text.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace sendspin::server {

namespace {

/// RFC 4291 section 2.2: eight 16-bit pieces; an IPv4 tail fills two
constexpr size_t IPV6_GROUPS = 8;
constexpr size_t IPV4_TAIL_GROUPS = 2;
constexpr size_t MAX_GROUP_DIGITS = 4;
constexpr size_t IPV4_OCTETS = 4;
constexpr size_t MAX_OCTET_DIGITS = 3;
constexpr unsigned MAX_OCTET = 255;

bool is_decimal_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_octet(std::string_view text) {
    if (text.empty() || text.size() > MAX_OCTET_DIGITS ||
        !std::all_of(text.begin(), text.end(), is_decimal_digit) ||
        (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    constexpr unsigned DECIMAL_BASE = 10;
    unsigned value = 0;
    for (const char digit : text) {
        value = (value * DECIMAL_BASE) + static_cast<unsigned>(digit - '0');
    }
    return value <= MAX_OCTET;
}

/// The number of groups in text, separated by single colons, the last an IPv4 part filling two
/// when ipv4_last allows it; nullopt when one is malformed. Empty text has none.
std::optional<size_t> count_groups(std::string_view text, bool ipv4_last) {
    size_t groups = 0;
    while (!text.empty()) {
        const size_t colon = text.find(':');
        const std::string_view group = text.substr(0, colon);
        if (colon == std::string_view::npos && ipv4_last &&
            group.find('.') != std::string_view::npos) {
            return is_ipv4_text(group) ? std::optional(groups + IPV4_TAIL_GROUPS) : std::nullopt;
        }
        if (group.empty() || group.size() > MAX_GROUP_DIGITS ||
            !std::all_of(group.begin(), group.end(), is_hex_digit)) {
            return std::nullopt;
        }
        ++groups;
        if (colon == std::string_view::npos) {
            break;
        }
        text.remove_prefix(colon + 1);
        // A single trailing ':'
        if (text.empty()) {
            return std::nullopt;
        }
    }
    return groups;
}

}  // namespace

bool is_hex_digit(char c) {
    return is_decimal_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_unreserved(char c) {
    constexpr std::string_view UNRESERVED_SYMBOLS = "-._~";
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_decimal_digit(c) ||
           UNRESERVED_SYMBOLS.find(c) != std::string_view::npos;
}

bool is_ipv4_text(std::string_view text) {
    for (size_t octet = 0; octet < IPV4_OCTETS; ++octet) {
        const size_t dot = text.find('.');
        const bool last = octet + 1 == IPV4_OCTETS;
        if (last != (dot == std::string_view::npos) || !is_octet(text.substr(0, dot))) {
            return false;
        }
        text.remove_prefix(last ? text.size() : dot + 1);
    }
    return true;
}

bool is_ipv6_text(std::string_view text) {
    constexpr std::string_view ELISION = "::";
    const size_t elision = text.find(ELISION);
    if (elision == std::string_view::npos) {
        return count_groups(text, true) == IPV6_GROUPS;
    }
    const std::string_view head = text.substr(0, elision);
    // A second "::" or a third colon beside the first leaves an empty group in the tail
    const std::string_view tail = text.substr(elision + ELISION.size());
    const std::optional<size_t> head_groups = count_groups(head, false);
    const std::optional<size_t> tail_groups = count_groups(tail, true);
    // "::" stands for at least one zero group
    return head_groups && tail_groups && *head_groups + *tail_groups < IPV6_GROUPS;
}

bool is_zone_id(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), is_unreserved);
}

bool is_ip_address_text(std::string_view text) {
    const size_t percent = text.find('%');
    if (percent == std::string_view::npos) {
        return is_ipv4_text(text) || is_ipv6_text(text);
    }
    return is_ipv6_text(text.substr(0, percent)) && is_zone_id(text.substr(percent + 1));
}

}  // namespace sendspin::server
