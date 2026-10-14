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

/// @file test_url_text.cpp
/// @brief The address text a dial's host and a browsed address are checked against, as a table

#include "url_text.h"
#include <gtest/gtest.h>

#include <string>

namespace {

// RFC 3986 section 3.2.2 (IPv4address, dec-octet), RFC 4291 section 2.2 (the IPv6 text forms)
// and RFC 6874 section 2 (ZoneID)
TEST(UrlText, TakesOnlyLiteralAddresses) {
    struct Row {
        std::string text;
        bool ipv6;
        bool address;
    };
    const Row rows[] = {
        // Control: IPv4, the bounds of an octet
        {"1.2.3.4", false, true},
        {"0.0.0.0", false, true},
        {"255.255.255.255", false, true},
        {"", false, false},
        {"1.2.3", false, false},
        {"1.2.3.4.5", false, false},
        {"1..3.4", false, false},
        {"1.2.3.4.", false, false},
        {"1.2.3.256", false, false},
        {"01.2.3.4", false, false},
        {"1.2.3.1234", false, false},
        {"1.2.3.4\r\nX-Injected: 1", false, false},
        {"1.2.3.4 ", false, false},
        {"localhost", false, false},
        // Control: IPv6 with and without "::", in either case, with an IPv4 tail
        {"::", true, true},
        {"::1", true, true},
        {"1::", true, true},
        {"1:2:3:4:5:6:7:8", true, true},
        {"1:2:3:4:5:6:7::", true, true},
        {"::2:3:4:5:6:7:8", true, true},
        {"FE80::1", true, true},
        {"::ffff:1.2.3.4", true, true},
        {"64:ff9b::1.2.3.4", true, true},
        {"1:2:3:4:5:6:1.2.3.4", true, true},
        {"fffff::1", false, false},
        {"0fac9::", false, false},
        {"1:2:3:4:5:6:7:", false, false},
        {":1:2:3:4:5:6:7", false, false},
        {"1::2:", false, false},
        {":1::2", false, false},
        {"1:2:3:4:5:6:7:89abc", false, false},
        {"1::2::3", false, false},
        {":::1", false, false},
        {"1:::2", false, false},
        {"1:2:3:4:5:6:7:8:9", false, false},
        {"1:2:3:4:5:6:7", false, false},
        {"1:2:3:4:5:6:7:8::", false, false},
        {"::1.2.3.256", false, false},
        {"::1.2.3.04", false, false},
        {"::1.2.3", false, false},
        {"1.2.3.4::", false, false},
        {"::1.2.3.4:5", false, false},
        {"1:2:3:4:5:6:7:1.2.3.4", false, false},
        {"fe80::g", false, false},
        // Control: a zone of unreserved characters, by name and by index
        {"fe80::1%en0", false, true},
        {"fe80::1%1", false, true},
        {"fe80::1%", false, false},
        {"fe80::1%en 0", false, false},
        {"fe80::1%en%0", false, false},
        {"fe80::1%en0\r\nX: y", false, false},
        {"1.2.3.4%en0", false, false},
        {"fffff::1%en0", false, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.text);
        EXPECT_EQ(sendspin::server::is_ipv6_text(row.text), row.ipv6);
        EXPECT_EQ(sendspin::server::is_ip_address_text(row.text), row.address);
    }
}

}  // namespace
