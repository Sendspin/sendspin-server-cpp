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

/// @file test_server_limits.cpp
/// @brief The caps on connections (connection_limits): accepted connections in their handshake
/// past the cap are closed, dials past it wait for one in progress to end, and accepted clients
/// past the activated caps are refused; the internal RAM a new connection needs; and the cap on a
/// client/hello name

#include "raw_client.h"
#include "server_core.h"
#include "silent_listener.h"
#include "test_recorder.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
using asio::ip::tcp;
using sendspin::server::ApprovalScope;
using sendspin::server::ErrorKind;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::config_limits::MAX_NAME_BYTES;
using sendspin::server::connection_limits::MAX_ACCEPTED_CLIENTS;
using sendspin::server::connection_limits::MAX_ACCEPTED_HANDSHAKES;
using sendspin::server::connection_limits::MAX_ACCEPTED_UNAPPROVED;
using sendspin::server::connection_limits::MAX_DIALING;
using sendspin::server::test::client_hello_json;
using sendspin::server::test::complete_upgrade;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::SilentListener;
using sendspin::server::test::upgrade_request;

/// Long enough for a connection or dial the server should not make to show up
constexpr auto ABSENCE_WINDOW = std::chrono::milliseconds(200);

std::string handshake_failed() {
    return "error " + std::to_string(static_cast<int>(ErrorKind::HANDSHAKE_FAILED)) + " ";
}

// The cap counts connections in their WebSocket upgrade and in their handshake together: with
// half of it in each, one more connection is closed before its upgrade is answered, and once one
// of them ends, the next connection is taken
TEST(ServerLimits, ClosesAnAcceptedConnectionPastTheHandshakeCap) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(sendspin::server::test::local_config(), &provider, &recorder),
              StartResult::OK);
    // raw_client.h: the IPv6 loopback reaches this server's dual-stack listener for certain
    const tcp::endpoint endpoint(asio::ip::make_address("::1"), server.listen_port());

    // Connected first, so the server has accepted all of them by the time it answers an upgrade
    // that follows them: it accepts in arrival order
    asio::io_context io;
    std::vector<std::unique_ptr<tcp::socket>> upgrading;
    for (size_t i = 0; i < MAX_ACCEPTED_HANDSHAKES / 2; ++i) {
        upgrading.push_back(std::make_unique<tcp::socket>(io));
        upgrading.back()->connect(endpoint);
    }
    std::vector<std::unique_ptr<RawClient>> handshaking;
    for (size_t i = upgrading.size(); i < MAX_ACCEPTED_HANDSHAKES; ++i) {
        handshaking.push_back(std::make_unique<RawClient>());
        ASSERT_EQ(handshaking.back()->dial(server.listen_port()), 101U) << "connection " << i;
    }

    RawClient refused;
    EXPECT_NE(refused.dial(server.listen_port()), 101U);

    handshaking.front()->drop();
    recorder.wait_for(handshake_failed());
    RawClient taken;
    EXPECT_EQ(taken.dial(server.listen_port()), 101U);
}

/// A server whose dial handshakes do not time out, so nothing but the test frees a dial's place
ServerConfig patient_config() {
    ServerConfig config = sendspin::server::test::local_config();
    config.timing.handshake_step_timeout_us = sendspin::server::config_limits::MAX_TIMING_US;
    return config;
}

/// Accepts count dials and completes each upgrade, so each waits in its handshake
std::vector<std::unique_ptr<SilentListener::WebSocket>> hold_in_handshake(SilentListener& listener,
                                                                          size_t count) {
    std::vector<std::unique_ptr<SilentListener::WebSocket>> held;
    for (size_t i = 0; i < count; ++i) {
        std::unique_ptr<SilentListener::Socket> socket = listener.accept();
        const auto request = upgrade_request(*socket);
        held.push_back(complete_upgrade(std::move(socket), request));
    }
    return held;
}

// A dial past the cap waits until one in progress ends, a dialed connection in its handshake or
// a dial, and the waiting dials start one per place freed, in the order they were asked for
TEST(ServerLimits, HoldsADialPastTheCapUntilOneEnds) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(patient_config(), &provider, &recorder), StartResult::OK);
    SilentListener listener;
    const auto path = [](size_t i) { return "/" + std::to_string(i); };
    for (size_t i = 0; i < MAX_DIALING + 2; ++i) {
        ASSERT_TRUE(server.connect_url(listener.url(path(i))));
    }
    std::vector<std::unique_ptr<SilentListener::WebSocket>> dialed =
        hold_in_handshake(listener, MAX_DIALING);
    std::this_thread::sleep_for(ABSENCE_WINDOW);
    EXPECT_FALSE(listener.dial_pending());

    // A dialed connection that ends in its handshake frees one place
    dialed.front()->next_layer().close();
    std::unique_ptr<SilentListener::Socket> next = listener.accept();
    EXPECT_EQ(upgrade_request(*next).target(), path(MAX_DIALING));
    std::this_thread::sleep_for(ABSENCE_WINDOW);
    EXPECT_FALSE(listener.dial_pending());

    // So does a dial that fails
    next->close();
    next = listener.accept();
    EXPECT_EQ(upgrade_request(*next).target(), path(MAX_DIALING + 1));
}

// A dialed client that completes its handshake frees its place as it is activated
TEST(ServerLimits, StartsAWaitingDialOnceADialedClientIsActivated) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(patient_config(), &provider, &recorder), StartResult::OK);
    SilentListener listener;
    RawClient client;
    for (size_t i = 0; i + 1 < MAX_DIALING; ++i) {
        ASSERT_TRUE(server.connect_url(listener.url("/held" + std::to_string(i))));
    }
    ASSERT_TRUE(server.connect_url(client.url()));
    ASSERT_TRUE(server.connect_url(listener.url("/waiting")));
    std::vector<std::unique_ptr<SilentListener::WebSocket>> held =
        hold_in_handshake(listener, MAX_DIALING - 1);
    ASSERT_NO_FATAL_FAILURE(client.handshake());
    std::this_thread::sleep_for(ABSENCE_WINDOW);
    EXPECT_FALSE(listener.dial_pending());

    client.send_json(sendspin::server::test::client_hello_json());
    EXPECT_NE(client.receive_until("server/activate"), "");
    std::unique_ptr<SilentListener::Socket> next = listener.accept();
    EXPECT_EQ(upgrade_request(*next).target(), "/waiting");
}

/// Connects client to server, completes the handshake and sends hello
void say_hello(const SendspinServer& server, RawClient& client,
               const std::string& hello = client_hello_json()) {
    ASSERT_EQ(client.dial(server.listen_port()), 101U);
    ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
    client.send_json(hello);
}

// An accepted client past the cap is closed once its handshake completes, with no activation; a
// dialed client is neither counted nor refused; and once an accepted client leaves, the next one is
// activated. It is slow for the real handshakes that fill the cap.
TEST(ServerLimits, RefusesAnAcceptedClientPastTheCap) {
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    std::vector<std::unique_ptr<RawClient>> accepted;
    for (size_t i = 0; i < MAX_ACCEPTED_CLIENTS; ++i) {
        accepted.push_back(std::make_unique<RawClient>());
        ASSERT_NO_FATAL_FAILURE(say_hello(server, *accepted.back()));
        // Control: every client up to the cap
        ASSERT_NE(accepted.back()->receive_until("server/activate"), "") << "client " << i;
        accepted.back()->send_json(sendspin::server::test::client_state_json(true));
    }

    RawClient refused;
    ASSERT_NO_FATAL_FAILURE(say_hello(server, refused));
    refused.expect_silent_close();

    RawClient dialed;
    ASSERT_TRUE(server.connect_url(dialed.url()));
    ASSERT_NO_FATAL_FAILURE(dialed.activate());

    const std::string left = accepted.front()->noise.identity().peer_id();
    accepted.front()->close();
    recorder.wait_for_match(
        [&](const std::string& what) { return what.starts_with("disconnected " + left + " "); });
    // The refusal is no error, which the dispatch thread would have reported before this disconnect
    EXPECT_EQ(recorder.count_errors(ErrorKind::HANDSHAKE_FAILED), 0U);
    RawClient taken;
    ASSERT_NO_FATAL_FAILURE(say_hello(server, taken));
    EXPECT_NE(taken.receive_until("server/activate"), "");
}

// Past its cap an accepted client that may not play is refused as one past
// connection_limits::MAX_ACCEPTED_CLIENTS is, while one that may play is activated; once one of
// the clients counted is approved, the next one is activated
TEST(ServerLimits, RefusesAnUnapprovedClientPastItsCap) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(sendspin::server::test::local_config(), &provider, &recorder),
              StartResult::OK);
    std::vector<std::unique_ptr<RawClient>> unapproved;
    for (size_t i = 0; i < MAX_ACCEPTED_UNAPPROVED; ++i) {
        unapproved.push_back(std::make_unique<RawClient>());
        ASSERT_NO_FATAL_FAILURE(say_hello(server, *unapproved.back()));
        // Control: every client up to the cap
        ASSERT_NE(unapproved.back()->receive_until("server/activate"), "") << "client " << i;
    }

    RawClient refused;
    ASSERT_NO_FATAL_FAILURE(say_hello(server, refused));
    refused.expect_silent_close();

    // Control: a client that may play
    RawClient approved;
    ASSERT_TRUE(server.approve(approved.noise.identity().peer_id(), ApprovalScope::PLAYBACK));
    ASSERT_NO_FATAL_FAILURE(say_hello(server, approved));
    EXPECT_NE(approved.receive_until("server/activate"), "");

    // Activated for playback once approved, so it no longer counts
    ASSERT_TRUE(
        server.approve(unapproved.front()->noise.identity().peer_id(), ApprovalScope::PLAYBACK));
    ASSERT_NE(unapproved.front()->receive_until("server/activate"), "");
    RawClient next;
    ASSERT_NO_FATAL_FAILURE(say_hello(server, next));
    EXPECT_NE(next.receive_until("server/activate"), "");
}

// A client/hello name past config_limits::MAX_NAME_BYTES, the cap on the host's names, is cut
// between UTF-8 characters in the client's listing and its group's name
TEST(ServerLimits, CutsAHelloNamePastTheNameCap) {
    struct Row {
        const char* name;
        std::string hello_name;
        std::string listed;
    };
    const std::string at_cap(MAX_NAME_BYTES, 'a');
    const std::string before_cap(MAX_NAME_BYTES - 1, 'a');
    const Row rows[] = {
        {"Control: a name at the cap", at_cap, at_cap},
        {"a name past it, cut before the character the cap splits", before_cap + "\xC3\xA9" + "b",
         before_cap},
    };
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(sendspin::server::test::local_config(), &provider, &recorder),
              StartResult::OK);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(say_hello(server, client, client_hello_json(row.hello_name)));
        ASSERT_NE(client.receive_until("server/activate"), "");
        const std::string id = client.noise.identity().peer_id();
        recorder.wait_for("connected " + id);
        bool listed = false;
        for (const sendspin::server::ClientInfo& info : server.clients()) {
            if (info.client_id == id) {
                listed = true;
                EXPECT_EQ(info.name, row.listed);
            }
        }
        EXPECT_TRUE(listed);
        bool grouped = false;
        for (const sendspin::server::GroupInfo& group : server.groups()) {
            if (group.client_ids == std::vector<std::string>{id}) {
                grouped = true;
                EXPECT_EQ(group.name, row.listed);
            }
        }
        EXPECT_TRUE(grouped);
    }
}

// connection_limits::heap_admits(): the reserve stays free beyond a share for the new connection
// and each one opening, and the largest block holds the minimum
TEST(ServerLimits, AdmitsAConnectionOnlyWithTheInternalRamItNeeds) {
    using sendspin::server::InternalHeap;
    using sendspin::server::connection_limits::heap_admits;
    using sendspin::server::connection_limits::HeapReserve;
    constexpr HeapReserve RESERVE{1000, 100, 50};
    struct Row {
        const char* name;
        size_t free_bytes;
        size_t largest_block_bytes;
        size_t opening;
        bool admits;
    };
    const Row rows[] = {
        {"Control: the reserve and the new connection's share", 1100, 50, 0, true},
        {"a byte short of the new connection's share", 1099, 50, 0, false},
        {"Control: a share for each connection opening", 1300, 50, 2, true},
        {"a byte short of a share for each connection opening", 1299, 50, 2, false},
        {"the largest block under the minimum", 5000, 49, 0, false},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(heap_admits(InternalHeap{row.free_bytes, row.largest_block_bytes}, row.opening,
                              RESERVE),
                  row.admits)
            << row.name;
    }
}

}  // namespace
