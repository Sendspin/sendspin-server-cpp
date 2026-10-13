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

/// @file test_one_group.cpp
/// @brief server_cli's --one-group with raw players: a client that leaves the server's group, or
/// reconnects into another group, is moved back into it

#include "one_group.h"
#include "raw_client.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::GroupInfo;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::test::client_state_json;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::player_hello_json;
using sendspin::server::test::player_state_json;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;

constexpr const char* PCM16 =
    R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])";
constexpr const char* LEAVE = R"({"type":"client/leave","payload":{}})";

/// Gathers every client into the server's group from on_group_changed() and
/// on_client_connected(), as server_cli's listener does with --one-group, while gather is set; and
/// records each client reported in a group other than the server's
class Gathering : public Recorder {
public:
    /// Whether on_group_changed() reported client_id in a group other than the server's
    bool seen_elsewhere(const std::string& client_id) const {
        const std::lock_guard<std::mutex> lock(this->seen_mutex_);
        return this->elsewhere_.contains(client_id);
    }

    void on_client_connected(const ClientInfo& client) override {
        Recorder::on_client_connected(client);
        if (this->gather.load()) {
            sendspin::server::examples::gather_into_server_group(*this->server);
        }
    }

    void on_group_changed(const GroupInfo& group) override {
        Recorder::on_group_changed(group);
        if (group.id != this->server->default_group()) {
            const std::lock_guard<std::mutex> lock(this->seen_mutex_);
            this->elsewhere_.insert(group.client_ids.begin(), group.client_ids.end());
        }
        if (this->gather.load()) {
            sendspin::server::examples::gather_into_server_group(*this->server);
        }
    }

    SendspinServer* server{nullptr};
    std::atomic<bool> gather{false};

private:
    mutable std::mutex seen_mutex_;
    std::set<std::string> elsewhere_;
};

class OneGroupServer {
public:
    OneGroupServer() {
        this->listener.server = &this->server;
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Sync";
        config.approve_all_clients = true;
        EXPECT_EQ(this->server.start(config, &this->provider, &this->listener), StartResult::OK);
    }

    ~OneGroupServer() {
        this->server.stop();
    }

    OneGroupServer(const OneGroupServer&) = delete;
    OneGroupServer& operator=(const OneGroupServer&) = delete;

    /// Activates client as a player that reports its state, once the server dialed it
    static void activate(RawClient& client, const std::string& name) {
        client.activate(player_hello_json(name, PCM16, 1000000));
        client.send_json(client_state_json(true, player_state_json(150, 0, 0)));
    }

    /// Whether the server's group lists client
    bool in_server_group(const RawClient& client) const {
        const std::string id = client.noise.identity().peer_id();
        for (const GroupInfo& group : this->server.groups()) {
            if (group.id == this->server.default_group()) {
                return std::find(group.client_ids.begin(), group.client_ids.end(), id) !=
                       group.client_ids.end();
            }
        }
        return false;
    }

    /// Whether a group other than the server's lists client
    bool elsewhere(const RawClient& client) const {
        const std::string id = client.noise.identity().peer_id();
        const std::vector<GroupInfo> groups = this->server.groups();
        return std::any_of(groups.begin(), groups.end(), [&](const GroupInfo& group) {
            return group.id != this->server.default_group() &&
                   std::find(group.client_ids.begin(), group.client_ids.end(), id) !=
                       group.client_ids.end();
        });
    }

    /// Samples groups() until done holds, with no timeout, so a regression hangs into the suite
    /// watchdog
    static void wait_until(const std::function<bool()>& done) {
        while (!done()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    MemoryProvider provider;
    Gathering listener;
    SendspinServer server;
};

// messaging.md "client/leave": a client that leaves a shared group is moved to a group of its own;
// gathering, it is moved straight back. Control: without gathering it stays in its own group.
TEST(OneGroup, MovesBackAClientThatLeftTheGroup) {
    for (const bool gather : {true, false}) {
        SCOPED_TRACE(gather ? "gathering" : "Control: not gathering");
        OneGroupServer s;
        s.listener.gather = gather;
        RawClient a;
        RawClient b;
        ASSERT_TRUE(s.server.connect_url(a.url()));
        OneGroupServer::activate(a, "A");
        ASSERT_TRUE(s.server.connect_url(b.url()));
        OneGroupServer::activate(b, "B");
        OneGroupServer::wait_until([&] { return s.in_server_group(a) && s.in_server_group(b); });

        a.send_json(LEAVE);
        const std::string id = a.noise.identity().peer_id();
        if (gather) {
            OneGroupServer::wait_until(
                [&] { return s.listener.seen_elsewhere(id) && s.in_server_group(a); });
        } else {
            OneGroupServer::wait_until([&] { return s.elsewhere(a); });
            EXPECT_FALSE(s.in_server_group(a));
        }
        EXPECT_TRUE(s.in_server_group(b));
    }
}

// A client that drops keeps its place in its group for its rejoin grace and rejoins it when the
// server dials it again: a client that left into a group of its own reconnects there, and,
// gathering by then, is moved into the server's group. Control: without gathering it rejoins its
// own group. Each row waits about a second for the server's dial again.
TEST(OneGroup, MovesBackAClientThatReconnectsIntoAnotherGroup) {
    for (const bool gather : {true, false}) {
        SCOPED_TRACE(gather ? "gathering" : "Control: not gathering");
        OneGroupServer s;
        RawClient a;
        RawClient b;
        ASSERT_TRUE(s.server.connect_url(a.url()));
        OneGroupServer::activate(a, "A");
        ASSERT_TRUE(s.server.connect_url(b.url()));
        OneGroupServer::activate(b, "B");
        OneGroupServer::wait_until([&] { return s.in_server_group(a) && s.in_server_group(b); });
        a.send_json(LEAVE);
        OneGroupServer::wait_until([&] { return s.elsewhere(a); });

        a.drop();
        s.listener.wait_for_disconnects(1);
        s.listener.gather = gather;
        // The server dials it again; it rejoins the group it held its place in
        OneGroupServer::activate(a, "A");
        if (gather) {
            OneGroupServer::wait_until([&] { return s.in_server_group(a); });
        } else {
            OneGroupServer::wait_until([&] { return s.elsewhere(a); });
            EXPECT_FALSE(s.in_server_group(a));
        }
        EXPECT_TRUE(s.in_server_group(b));
    }
}

}  // namespace
