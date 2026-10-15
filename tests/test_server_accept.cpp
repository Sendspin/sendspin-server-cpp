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

/// @file test_server_accept.cpp
/// @brief Clients that connect to SendspinServer (spec connection.md "Client Initiated
/// Connections"), as the raw test client dials its listener: the path it serves, the handshake with
/// the server still the Noise initiator, and the activities a client that connected declares as its
/// group's playback starts and stops

#include "raw_client.h"
#include "silence_feeder.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::DisconnectReason;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputOptions;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::test::client_goodbye_json;
using sendspin::server::test::LocalServer;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;

const std::string PLAYER_HELLO = sendspin::server::test::player_hello_json(
    "Player", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 200000);

std::string activation(const std::string& activities, const std::string& roles) {
    return R"({"type":"server/activate","payload":{"activities":)" + activities +
           R"(,"active_roles":)" + roles + "}}";
}

/// Dials server, completes the handshake, sends hello and, once activated, takes the connection
/// with an available player's client/state
/// @return the first server/activate
std::string connect_to(const SendspinServer& server, RawClient& client, const std::string& hello) {
    EXPECT_EQ(client.dial(server.listen_port()), 101U);
    client.complete_handshake();
    client.send_json(hello);
    std::string activation = client.receive_until("server/activate");
    client.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(0, 0, 0)));
    return activation;
}

// spec connection.md "Client Initiated Connections": the server serves its configured path and
// refuses an upgrade to any other (the query string aside)
TEST(ServerAccept, ServesOnlyItsPath) {
    struct Row {
        const char* name;
        const char* configured;
        const char* requested;
        unsigned status;
    };
    const Row rows[] = {
        {"Control: the default path", "/sendspin", "/sendspin", 101},
        {"Control: a query string", "/sendspin", "/sendspin?client=1", 101},
        {"another path", "/sendspin", "/other", 404},
        {"Control: a configured path", "/custom", "/custom", 101},
        {"the default path when another is configured", "/custom", "/sendspin", 404},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        ServerConfig config = sendspin::server::test::local_config();
        config.path = row.configured;
        LocalServer s;
        ASSERT_EQ(s.start(config), StartResult::OK);
        RawClient client;
        EXPECT_EQ(client.dial(s.server.listen_port(), row.requested), row.status);
        if (row.status != 101) {
            continue;
        }
        // The server is the Noise initiator on a connection the client made too
        ASSERT_NO_FATAL_FAILURE(client.complete_handshake());
        client.send_json(sendspin::server::test::client_hello_json("Caller"));
        EXPECT_NE(client.receive_until("server/activate"), "");
        s.recorder.wait_for("connected " + client.noise.identity().peer_id());
    }
}

// The listening port in use: a second server cannot start on it
TEST(ServerAccept, ReportsAPortInUse) {
    MemoryProvider provider;
    SendspinServer first;
    ASSERT_EQ(first.start(sendspin::server::test::local_config(), &provider, nullptr),
              StartResult::OK);
    ServerConfig config = sendspin::server::test::local_config();
    config.listen_port = first.listen_port();
    SendspinServer second;
    EXPECT_EQ(second.start(config, &provider, nullptr), StartResult::BIND_FAILED);
    EXPECT_EQ(second.listen_port(), 0);
    EXPECT_EQ(second.server_id(), "");
}

// A client that connected declares `playback` in its first activation only while its group plays,
// and later as its group starts and stops playing, keeping its roles. It is listed as connected to
// the server, at its address, and not once gone, since the server cannot dial it.
TEST(ServerAccept, ActivatesAClientThatConnectedByItsGroupsPlayback) {
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    LocalServer s;
    ASSERT_EQ(s.start(config), StartResult::OK);

    RawClient early;
    EXPECT_EQ(connect_to(s.server, early, PLAYER_HELLO), activation("[]", R"(["player@v1"])"));
    const std::string early_id = early.noise.identity().peer_id();
    s.recorder.wait_for("connected " + early_id);
    const ClientInfo info = s.server.clients().at(0);
    EXPECT_TRUE(info.connected);
    EXPECT_TRUE(info.accepted);
    EXPECT_FALSE(info.discovered);
    EXPECT_EQ(info.url, "");
    EXPECT_EQ(info.address.rfind("[::1]:", 0), 0U) << info.address;

    const sendspin::server::OpenedInput opened = s.server.open_input(
        *s.server.default_group(), InputOptions{InputFormat{48000, 2, SampleFormat::S16}});
    ASSERT_TRUE(opened.input);
    RawClient late;
    {
        const sendspin::server::test::SilenceFeeder feeder(opened.input);
        EXPECT_EQ(early.receive_until("server/activate"),
                  R"({"type":"server/activate","payload":{"activities":["playback"]}})");
        EXPECT_EQ(connect_to(s.server, late, PLAYER_HELLO),
                  activation(R"(["playback"])", R"(["player@v1"])"));
    }
    for (RawClient* client : {&early, &late}) {
        EXPECT_EQ(client->receive_until("server/activate"),
                  R"({"type":"server/activate","payload":{"activities":[]}})");
    }

    late.send_json(client_goodbye_json("restart"));
    late.read_until_closed();
    s.recorder.wait_for("disconnected " + late.noise.identity().peer_id() + " " +
                        std::to_string(static_cast<int>(DisconnectReason::GOODBYE)));
    const std::vector<ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_EQ(clients[0].client_id, early_id);
}

// A client connected twice, such as one whose earlier connection the server has not seen drop, is
// reported connected once and listed once; when the reported connection goes, the other takes
// its place, and the client keeps no place in its group while a connection of it remains
TEST(ServerAccept, ReportsASecondConnectionOfAClientOnlyInThePlaceOfTheFirst) {
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    LocalServer s;
    ASSERT_EQ(s.start(config), StartResult::OK);
    RawClient first;
    const std::string id = first.noise.identity().peer_id();
    RawClient second(
        sendspin::server::CipherSuite::CHACHAPOLY_SHA256,
        *sendspin::server::Identity::from_private_bytes(first.noise.identity().private_bytes));
    connect_to(s.server, first, PLAYER_HELLO);
    s.recorder.wait_for("connected " + id);
    connect_to(s.server, second, PLAYER_HELLO);
    // The second's client/state was taken in: its time request is answered after it
    second.send_json(sendspin::server::test::client_time_json(1));
    EXPECT_NE(second.receive_until("server/time"), "");
    EXPECT_EQ(s.recorder.count("connected " + id), 1U);
    EXPECT_EQ(s.server.clients().size(), 1U);

    first.send_json(client_goodbye_json("restart"));
    first.read_until_closed();
    s.recorder.wait_for_count("connected " + id, 2);
    EXPECT_EQ(s.recorder.count("disconnected " + id + " " +
                               std::to_string(static_cast<int>(DisconnectReason::GOODBYE))),
              1U);
    const std::vector<ClientInfo> clients = s.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].connected);
}

// SendspinServer::disconnect() closes every connection of the client, one still in its handshake
// once its client/init named the client among them, and reports the client gone once: a
// connection the host is closing is not reported in the place of the one that closed first, and
// the one in its handshake is no on_error(): the host closed it
TEST(ServerAccept, DisconnectsEveryConnectionOfAClientAndReportsItOnce) {
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    LocalServer s;
    ASSERT_EQ(s.start(config), StartResult::OK);
    RawClient first;
    const std::string id = first.noise.identity().peer_id();
    const auto same_client = [&] {
        return std::make_unique<RawClient>(
            sendspin::server::CipherSuite::CHACHAPOLY_SHA256,
            *sendspin::server::Identity::from_private_bytes(first.noise.identity().private_bytes));
    };
    const std::unique_ptr<RawClient> second = same_client();
    const std::unique_ptr<RawClient> handshaking = same_client();
    connect_to(s.server, first, PLAYER_HELLO);
    s.recorder.wait_for("connected " + id);
    connect_to(s.server, *second, PLAYER_HELLO);
    // The second's client/state was taken in: its time request is answered after it
    second->send_json(sendspin::server::test::client_time_json(1));
    EXPECT_NE(second->receive_until("server/time"), "");
    EXPECT_EQ(handshaking->dial(s.server.listen_port()), 101U);
    handshaking->send_client_init();
    handshaking->receive_message_1();

    ASSERT_TRUE(s.server.disconnect(id));
    for (RawClient* client : {&first, second.get(), handshaking.get()}) {
        client->read_until_closed();
    }
    // A later client's connection: every event of the disconnect is delivered before it
    RawClient later;
    connect_to(s.server, later, PLAYER_HELLO);
    s.recorder.wait_for("connected " + later.noise.identity().peer_id());
    EXPECT_EQ(s.recorder.count("connected " + id), 1U);
    EXPECT_EQ(
        s.recorder.count("disconnected " + id + " " +
                         std::to_string(static_cast<int>(DisconnectReason::DISCONNECTED_BY_HOST))),
        1U);
    EXPECT_EQ(s.recorder.count_errors(sendspin::server::ErrorKind::HANDSHAKE_FAILED), 0U);
    for (const ClientInfo& client : s.server.clients()) {
        EXPECT_NE(client.client_id, id);
    }
}

}  // namespace
