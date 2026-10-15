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

/// @file test_transport_url.cpp
/// @brief The transport's platform-free parts: a dial URL parsed into its target, and a socket
/// call's errno as the close reason both transports report.

#include "transport.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <optional>

namespace {

using sendspin::server::close_reason_for_errno;
using sendspin::server::CloseReason;
using sendspin::server::DialTarget;
using sendspin::server::parse_ws_url;

// ==============================================================================
// URL parsing
// ==============================================================================

TEST(TransportUrl, ParsesWebSocketUrls) {
    struct Row {
        const char* url;
        bool valid;
        const char* host;
        uint16_t port;
        const char* path;
    };
    const Row rows[] = {
        // Control:
        {"ws://192.168.1.20:8928/sendspin", true, "192.168.1.20", 8928, "/sendspin"},
        {"ws://speaker.local", true, "speaker.local", 80, "/"},
        {"ws://[fe80::1]:9000/a/b?x=1", true, "fe80::1", 9000, "/a/b?x=1"},
        {"ws://[::1]/p", true, "::1", 80, "/p"},
        {"ws://[::ffff:192.0.2.1]/p", true, "::ffff:192.0.2.1", 80, "/p"},
        {"wss://host/p", false, "", 0, ""},
        {"http://host/p", false, "", 0, ""},
        {"ws://", false, "", 0, ""},
        {"ws://:8928/p", false, "", 0, ""},
        {"ws://host:/p", false, "", 0, ""},
        {"ws://host:0/p", false, "", 0, ""},
        {"ws://host:65536/p", false, "", 0, ""},
        {"ws://host:12ab/p", false, "", 0, ""},
        {"ws://[::1/p", false, "", 0, ""},
        {"ws://[::1]x/p", false, "", 0, ""},
        {"ws://user@host/p", false, "", 0, ""},
        {"ws://host?x=1", false, "", 0, ""},
        {"ws://host/p#frag", false, "", 0, ""},
        {"ws://host#frag", false, "", 0, ""},
        // RFC 3986 sections 2 and 3: nothing that could break the upgrade request's lines.
        // Control: every character class a host and a path may hold
        {"ws://h-1.local/a_b~c.%2F;x=1!$&'()*+,@:?q=/?", true, "h-1.local", 80,
         "/a_b~c.%2F;x=1!$&'()*+,@:?q=/?"},
        // An IPv6 zone identifier, kept as the system resolver takes it.
        // Control: percent-encoded (RFC 6874 section 2)
        {"ws://[fe80::1%25en0]/p", true, "fe80::1%en0", 80, "/p"},
        // Control: raw, by name and by index
        {"ws://[fe80::1%en0]:8928/sendspin", true, "fe80::1%en0", 8928, "/sendspin"},
        {"ws://[fe80::1%1]/", true, "fe80::1%1", 80, "/"},
        {"ws://[fe80::1%]/p", false, "", 0, ""},
        {"ws://[fe80::1%25]/p", false, "", 0, ""},
        {"ws://[fe80::1%en 0]/p", false, "", 0, ""},
        {"ws://[fe80::1%en\x01]/p", false, "", 0, ""},
        {"ws://[fe80::1%en0\r\n]/p", false, "", 0, ""},
        // A zone holding '%', before or after decoding
        {"ws://[fe%80::1%en0]/p", false, "", 0, ""},
        {"ws://[fe80::1%25en%250]/p", false, "", 0, ""},
        {"ws://[fe80::1%%en0]/p", false, "", 0, ""},
        // An address that is empty or holds a character besides hex digits, ':' and '.'
        {"ws://[%en0]/p", false, "", 0, ""},
        {"ws://[fe80::g]/p", false, "", 0, ""},
        // RFC 3986 section 3.2.2: a bracketed address is IPv6, in a form is_ipv6_text() takes
        // (its own table is in test_url_text.cpp).
        // Control: the forms with and without "::", in either case, with an IPv4 tail
        {"ws://[::]/p", true, "::", 80, "/p"},
        {"ws://[1:2:3:4:5:6:7:8]/p", true, "1:2:3:4:5:6:7:8", 80, "/p"},
        {"ws://[FE80::1]/p", true, "FE80::1", 80, "/p"},
        {"ws://[64:ff9b::1.2.3.4]/p", true, "64:ff9b::1.2.3.4", 80, "/p"},
        {"ws://[1]/p", false, "", 0, ""},
        {"ws://[::::]/p", false, "", 0, ""},
        {"ws://[1.2.3.4]/p", false, "", 0, ""},
        {"ws://[1.2.3.4%en0]/p", false, "", 0, ""},
        // Forms macOS's inet_pton takes that RFC 4291 does not
        {"ws://[0fac9::]/p", false, "", 0, ""},
        {"ws://[::1.2.3.04]/p", false, "", 0, ""},
        {"ws://host/a\r\nX-Injected: 1", false, "", 0, ""},
        {"ws://host/a\nb", false, "", 0, ""},
        {"ws://host/a b", false, "", 0, ""},
        {"ws://host/a\x7F", false, "", 0, ""},
        {"ws://host/a\x01", false, "", 0, ""},
        {"ws://host/a\"b", false, "", 0, ""},
        {"ws://host/caf\xC3\xA9", false, "", 0, ""},
        {"ws://host/%2", false, "", 0, ""},
        {"ws://host/%zz", false, "", 0, ""},
        {"ws://host/%g0", false, "", 0, ""},
        {"ws://host/%0g", false, "", 0, ""},
        {"ws://ho\r\nst/p", false, "", 0, ""},
        // A percent-encoding in a host outside brackets, which no one would decode.
        // Control: in the path, it stays as written (above)
        {"ws://h%20st/p", false, "", 0, ""},
        {"ws://h%41st/p", false, "", 0, ""},
        {"ws://1.2.3.4%251/p", false, "", 0, ""},
        {"ws://ho st/p", false, "", 0, ""},
        {"ws://[::1\r\n]/p", false, "", 0, ""},
        {"ws://[::1 ]/p", false, "", 0, ""},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.url);
        const std::optional<DialTarget> target = parse_ws_url(row.url);
        ASSERT_EQ(target.has_value(), row.valid);
        if (row.valid) {
            EXPECT_EQ(target->host, row.host);
            EXPECT_EQ(target->port, row.port);
            EXPECT_EQ(target->path, row.path);
            EXPECT_TRUE(target->addresses.empty());
        }
    }
}

// ==============================================================================
// Socket errors
// ==============================================================================

// A socket call's errno as both transports report it: the host's for an Asio system error
// (PeerResetIsReportedAsPeerClosed), the ESP-IDF transport's for its sends, waits and reads
TEST(TransportErrno, MapsASocketErrorToItsCloseReason) {
    struct Row {
        const char* name;
        int error;
        CloseReason reason;
    };
    const Row rows[] = {
        {"connection reset", ECONNRESET, CloseReason::PEER_CLOSED},
        {"connection aborted", ECONNABORTED, CloseReason::PEER_CLOSED},
        {"broken pipe", EPIPE, CloseReason::PEER_CLOSED},
        {"not connected", ENOTCONN, CloseReason::PEER_CLOSED},
        {"Control: timed out", ETIMEDOUT, CloseReason::FAILED},
        {"Control: host unreachable", EHOSTUNREACH, CloseReason::FAILED},
        {"Control: bad descriptor", EBADF, CloseReason::FAILED},
        {"Control: no error", 0, CloseReason::FAILED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(close_reason_for_errno(row.error), row.reason);
    }
}

}  // namespace
