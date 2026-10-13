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
/// @brief The caps on connections in their handshake (connection_limits): accepted connections past
/// the cap are closed, and dials past it wait for one in progress to end; and the internal RAM a
/// new connection needs

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
using sendspin::server::ErrorKind;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::connection_limits::MAX_ACCEPTED_HANDSHAKES;
using sendspin::server::connection_limits::MAX_DIALING;
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
