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

/// @file test_ws_handshake.cpp
/// @brief The ESP-IDF dial's opening handshake on the host: the key and accept values, the
/// upgrade request, the response check's rejections, and the request, the check and masked frames
/// against Beast's WebSocket server over loopback.

#include "transport.h"
#include "ws_batch.h"
#include "ws_frame.h"
#include "ws_handshake.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
using asio::ip::tcp;
using sendspin::server::check_upgrade_response;
using sendspin::server::DialTarget;
using sendspin::server::MessageKind;
using sendspin::server::ws_accept_value;
using sendspin::server::ws_handshake_key;
using sendspin::server::WS_KEY_NONCE_BYTES;
using sendspin::server::ws_parse_frame_header;
using sendspin::server::ws_upgrade_request;
using sendspin::server::WS_UPGRADE_RESPONSE_MAX;
using sendspin::server::WsBatch;
using sendspin::server::WsHeaderParse;
using sendspin::server::WsHeaderStatus;
using sendspin::server::WsMaskKey;
using sendspin::server::WsUpgradeCheck;
using sendspin::server::WsUpgradeStatus;

/// RFC 6455 section 1.3's example
constexpr const char* SAMPLE_KEY = "dGhlIHNhbXBsZSBub25jZQ==";
constexpr const char* SAMPLE_ACCEPT = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

// RFC 6455 section 1.3: the sample nonce's key and the accept a server derives from it
TEST(WsHandshake, KeyAndAcceptMatchTheRfcExample) {
    const std::string nonce_text = "the sample nonce";
    std::array<uint8_t, WS_KEY_NONCE_BYTES> nonce{};
    std::copy(nonce_text.begin(), nonce_text.end(), nonce.begin());
    EXPECT_EQ(ws_handshake_key(nonce), SAMPLE_KEY);
    EXPECT_EQ(ws_accept_value(SAMPLE_KEY), SAMPLE_ACCEPT);
}

// RFC 6455 section 4.1: the request line, Host with the port (an IPv6 literal bracketed, its
// zone left out as RFC 6874 section 4 requires), the upgrade headers and version 13
TEST(WsHandshake, BuildsTheUpgradeRequest) {
    DialTarget target;
    target.host = "speaker.local";
    target.port = 8928;
    target.path = "/sendspin";
    EXPECT_EQ(ws_upgrade_request(target, SAMPLE_KEY),
              "GET /sendspin HTTP/1.1\r\nHost: speaker.local:8928\r\nUpgrade: websocket\r\n"
              "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              "Sec-WebSocket-Version: 13\r\n\r\n");
    target.host = "fe80::1";
    target.port = 80;
    target.path = "/";
    EXPECT_EQ(ws_upgrade_request(target, SAMPLE_KEY).substr(0, 39),
              "GET / HTTP/1.1\r\nHost: [fe80::1]:80\r\nUpg");
    target.host = "fe80::1%en0";
    EXPECT_EQ(ws_upgrade_request(target, SAMPLE_KEY).substr(0, 39),
              "GET / HTTP/1.1\r\nHost: [fe80::1]:80\r\nUpg");
}

// RFC 6455 section 4.1: the server's response is taken only as a 101 with the upgrade headers
// and the accept the key requires, within WS_UPGRADE_RESPONSE_MAX
TEST(WsHandshake, ChecksTheResponse) {
    const std::string accept = std::string("Sec-WebSocket-Accept: ") + SAMPLE_ACCEPT + "\r\n";
    const std::string ok_status = "HTTP/1.1 101 Switching Protocols\r\n";
    const std::string ok_headers = "Upgrade: websocket\r\nConnection: Upgrade\r\n" + accept;
    const std::string padding(WS_UPGRADE_RESPONSE_MAX, 'x');
    struct Row {
        std::string name;
        std::string received;
        WsUpgradeStatus status;
        // For ACCEPTED
        size_t response_bytes;
    };
    const std::string ok = ok_status + ok_headers + "\r\n";
    // A response of exactly n bytes, padded with a header
    const auto response_of = [&](size_t n) {
        const std::string unpadded = ok_status + "X-Pad: \r\n" + ok_headers + "\r\n";
        return ok_status + "X-Pad: " + std::string(n - unpadded.size(), 'x') + "\r\n" + ok_headers +
               "\r\n";
    };
    const std::string longest = response_of(WS_UPGRADE_RESPONSE_MAX);
    const Row rows[] = {
        {"Control: accepted", ok, WsUpgradeStatus::ACCEPTED, ok.size()},
        {"Control: frames after the response are not part of it", ok + "\x82\x01z",
         WsUpgradeStatus::ACCEPTED, ok.size()},
        {"Control: names and values in any case, Connection as a list, a bare status",
         "HTTP/1.1 101\r\nupgrade:WebSocket\r\nCONNECTION: keep-alive, upgrade\r\n"
         "sec-websocket-accept:  " +
             std::string(SAMPLE_ACCEPT) + " \r\nServer: test\r\n\r\n",
         WsUpgradeStatus::ACCEPTED, 0},
        {"Control: Connection on two lines, Upgrade first",
         ok_status + "Upgrade: websocket\r\nConnection: Upgrade\r\nConnection: keep-alive\r\n" +
             accept + "\r\n",
         WsUpgradeStatus::ACCEPTED, 0},
        {"Control: Connection on two lines, Upgrade last",
         ok_status + "Upgrade: websocket\r\nConnection: keep-alive\r\nConnection: Upgrade\r\n" +
             accept + "\r\n",
         WsUpgradeStatus::ACCEPTED, 0},
        {"Control: a response of WS_UPGRADE_RESPONSE_MAX bytes, a frame after it",
         longest + "\x82\x01z", WsUpgradeStatus::ACCEPTED, longest.size()},
        {"no blank line yet", ok_status + ok_headers, WsUpgradeStatus::INCOMPLETE, 0},
        {"nothing yet", "", WsUpgradeStatus::INCOMPLETE, 0},
        {"OK instead of 101", "HTTP/1.1 200 OK\r\n" + ok_headers + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"a redirect", "HTTP/1.1 301 Moved Permanently\r\nLocation: ws://x/\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"HTTP/1.0", "HTTP/1.0 101 Switching Protocols\r\n" + ok_headers + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"status 1010", "HTTP/1.1 1010 x\r\n" + ok_headers + "\r\n", WsUpgradeStatus::REJECTED, 0},
        {"wrong accept",
         ok_status + "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "
                     "s3pPLMBiTxaQ9kYGzzhZRbK+xOo\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"no accept", ok_status + "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"no Upgrade", ok_status + "Connection: Upgrade\r\n" + accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"Upgrade to another protocol",
         ok_status + "Upgrade: h2c\r\nConnection: Upgrade\r\n" + accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"another protocol's Upgrade line, then websocket's",
         ok_status + "Upgrade: h2c\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" + accept +
             "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"two Upgrade lines of websocket",
         ok_status + "Upgrade: websocket\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
             accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"whitespace before a field's colon",
         ok_status + "Upgrade : h2c\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" + accept +
             "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"Upgrade as a list",
         ok_status + "Upgrade: websocket, h2c\r\nConnection: Upgrade\r\n" + accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"no Connection", ok_status + "Upgrade: websocket\r\n" + accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"Connection without upgrade",
         ok_status + "Upgrade: websocket\r\nConnection: keep-alive\r\n" + accept + "\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"an extension not offered",
         ok_status + ok_headers + "Sec-WebSocket-Extensions: permessage-deflate\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"a subprotocol not offered", ok_status + ok_headers + "Sec-WebSocket-Protocol: x\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"a header line without a colon", ok_status + ok_headers + "garbage\r\n\r\n",
         WsUpgradeStatus::REJECTED, 0},
        {"no blank line within the cap", ok_status + "X-Pad: " + padding, WsUpgradeStatus::REJECTED,
         0},
        {"a response one byte over the cap", response_of(WS_UPGRADE_RESPONSE_MAX + 1),
         WsUpgradeStatus::REJECTED, 0},
        {"a blank line past the cap",
         ok_status + "X-Pad: " + padding + "\r\n" + ok_headers + "\r\n", WsUpgradeStatus::REJECTED,
         0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const WsUpgradeCheck check = check_upgrade_response(row.received, SAMPLE_KEY);
        EXPECT_EQ(check.status, row.status);
        if (row.status == WsUpgradeStatus::ACCEPTED && row.response_bytes > 0) {
            EXPECT_EQ(check.response_bytes, row.response_bytes);
        }
    }
}

// The dial's handshake and framing against an independent WebSocket server: Beast accepts the
// request and its response passes the check; Beast reads the masked messages back, and refuses
// unmasked ones (RFC 6455 section 5.1); the server's own message parses as a client receives it.
TEST(WsHandshake, InteroperatesWithBeastAsTheServer) {
    asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::vector<uint8_t> first{1, 2, 3, 4, 5, 6, 7};
    const std::vector<uint8_t> second(300, 0x5A);
    const std::vector<uint8_t> reply{9, 8, 7};

    struct Seen {
        std::vector<std::vector<uint8_t>> messages;
        bool unmasked_refused{false};
        std::string error;
    } seen;
    std::thread server([&] {
        try {
            beast::websocket::stream<tcp::socket> ws(acceptor.accept());
            ws.accept();
            for (int i = 0; i < 2; ++i) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                const auto* data = static_cast<const uint8_t*>(buffer.data().data());
                seen.messages.emplace_back(data, data + buffer.size());
            }
            ws.binary(true);
            ws.write(asio::buffer(reply));
            beast::flat_buffer buffer;
            beast::error_code ec;
            ws.read(buffer, ec);
            seen.unmasked_refused = ec == beast::websocket::error::bad_unmasked_frame;
        } catch (const std::exception& e) {
            seen.error = e.what();
        }
    });

    tcp::socket client(io);
    client.connect(acceptor.local_endpoint());
    DialTarget target;
    target.host = "127.0.0.1";
    target.port = acceptor.local_endpoint().port();
    target.path = "/sendspin";
    const std::string key = ws_handshake_key({0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x10,
                                              0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE});
    asio::write(client, asio::buffer(ws_upgrade_request(target, key)));

    std::string received;
    WsUpgradeCheck check;
    while (check.status == WsUpgradeStatus::INCOMPLETE) {
        std::array<char, 256> chunk{};
        received.append(chunk.data(), client.read_some(asio::buffer(chunk)));
        check = check_upgrade_response(received, key);
    }
    ASSERT_EQ(check.status, WsUpgradeStatus::ACCEPTED) << received;

    WsBatch batch;
    batch.append(MessageKind::BINARY, first, WsMaskKey{0x11, 0x22, 0x33, 0x44});
    batch.append(MessageKind::BINARY, second, WsMaskKey{0xA1, 0xB2, 0xC3, 0xD4});
    asio::write(client, asio::buffer(batch.bytes().data(), batch.bytes().size()));

    std::vector<uint8_t> frame(received.begin() + static_cast<std::ptrdiff_t>(check.response_bytes),
                               received.end());
    WsHeaderParse parse;
    while ((parse = ws_parse_frame_header(frame, false)).status == WsHeaderStatus::INCOMPLETE ||
           frame.size() < parse.header.header_bytes + parse.header.payload_bytes) {
        std::array<uint8_t, 256> chunk{};
        const size_t got = client.read_some(asio::buffer(chunk));
        frame.insert(frame.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    ASSERT_EQ(parse.status, WsHeaderStatus::COMPLETE);
    EXPECT_TRUE(parse.header.fin);
    EXPECT_EQ(parse.header.opcode, sendspin::server::ws_opcode::BINARY);
    EXPECT_EQ(
        std::vector<uint8_t>(frame.begin() + static_cast<std::ptrdiff_t>(parse.header.header_bytes),
                             frame.end()),
        reply);

    WsBatch unmasked;
    unmasked.append(MessageKind::BINARY, first);
    asio::write(client, asio::buffer(unmasked.bytes().data(), unmasked.bytes().size()));
    // Beast fails the connection with its close frame and waits for the peer's: the TCP close
    // ends that wait
    client.shutdown(tcp::socket::shutdown_send);
    server.join();
    EXPECT_EQ(seen.error, "");
    ASSERT_EQ(seen.messages.size(), 2U);
    EXPECT_EQ(seen.messages[0], first);
    EXPECT_EQ(seen.messages[1], second);
    EXPECT_TRUE(seen.unmasked_refused);
}

}  // namespace
