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

/// @file test_server_discovery.cpp
/// @brief SendspinServer with a host-supplied DiscoveryProvider (ServerConfig::discovery): what it
/// advertises, and how it dials the clients browsing finds, with which intent, and again after each
/// kind of end, against the raw test client

#include "raw_client.h"
#include "server_core.h"
#include "silence_feeder.h"
#include "silent_listener.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <sendspin/server/discovery.h>
#include <sendspin/server/server.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::DisconnectReason;
using sendspin::server::DiscoveredClient;
using sendspin::server::DiscoveryProvider;
using sendspin::server::DiscoveryRequest;
using sendspin::server::DiscoverySink;
using sendspin::server::GroupInput;
using sendspin::server::InputFormat;
using sendspin::server::InputOptions;
using sendspin::server::LogLevel;
using sendspin::server::SampleFormat;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::test::client_goodbye_json;
using sendspin::server::test::client_hello_json;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;

/// A provider the test drives: it records what the server starts it with and reports browse
/// results when the test says so, from the test's thread
class FakeDiscovery : public DiscoveryProvider {
public:
    bool start(const DiscoveryRequest& request, DiscoverySink& sink) override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ++this->starts_;
        this->request_ = request;
        if (this->refuse) {
            return false;
        }
        this->sink_ = &sink;
        return true;
    }

    void stop() override {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ++this->stops_;
        this->sink_ = nullptr;
    }

    void found(const DiscoveredClient& client) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ASSERT_NE(this->sink_, nullptr);
        this->sink_->on_client_found(client);
    }

    void lost(const std::string& instance) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ASSERT_NE(this->sink_, nullptr);
        this->sink_->on_client_lost(instance);
    }

    void unavailable() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ASSERT_NE(this->sink_, nullptr);
        this->sink_->on_unavailable();
    }

    void available() {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        ASSERT_NE(this->sink_, nullptr);
        this->sink_->on_available();
    }

    int starts() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->starts_;
    }

    int stops() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->stops_;
    }

    DiscoveryRequest request() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->request_;
    }

    /// Set before the server starts
    bool refuse{false};

private:
    mutable std::mutex mutex_;
    DiscoverySink* sink_{nullptr};
    DiscoveryRequest request_;
    int starts_{0};
    int stops_{0};
};

/// The browse result that reaches client
DiscoveredClient service_of(const RawClient& client, const std::string& name) {
    DiscoveredClient service;
    service.instance = name;
    service.addresses = {"127.0.0.1"};
    service.host = "raw.local.";
    service.port = client.port();
    service.path = "/sendspin";
    service.name = name;
    return service;
}

ServerConfig browsing_config(const std::shared_ptr<FakeDiscovery>& discovery) {
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Discovery";
    config.browse = true;
    config.discovery = discovery;
    config.approve_all_clients = true;
    // Short waits before a dial on the server's own, so a reconnect comes at once
    config.timing.handshake_backoff_min_us = 1000;
    config.timing.handshake_backoff_max_us = 2000;
    return config;
}

std::string id_of(const RawClient& client) {
    return client.noise.identity().peer_id();
}

std::string disconnected(const std::string& client_id, DisconnectReason reason) {
    return "disconnected " + client_id + " " + std::to_string(static_cast<int>(reason));
}

/// The client/state a client sends once it takes the connection: available, with a player
/// object when it is a player
std::string state_for(const std::string& hello) {
    return hello.find("player@v1") == std::string::npos
               ? sendspin::server::test::client_state_json(true)
               : sendspin::server::test::client_state_json(
                     true, sendspin::server::test::player_state_json(0, 0, 0));
}

/// The first server/activate after the server dials client, which accepts it with hello as the
/// client/hello; with take set, the client then takes the connection with its client/state
/// (connection.md "Multiple servers (server-initiated)")
std::string accept_and_activate(RawClient& client, const std::string& hello, bool take = true) {
    EXPECT_EQ(client.accept(), "/sendspin");
    client.complete_handshake();
    client.send_json(hello);
    std::string activation = client.receive_until("server/activate");
    if (take) {
        client.send_json(state_for(hello));
    }
    return activation;
}

std::string activation(const std::string& activities, const std::string& roles) {
    return R"({"type":"server/activate","payload":{"activities":)" + activities +
           R"(,"active_roles":)" + roles + "}}";
}

/// The one client clients() lists
ClientInfo only_client(const SendspinServer& server) {
    const std::vector<ClientInfo> clients = server.clients();
    EXPECT_EQ(clients.size(), 1U);
    return clients.empty() ? ClientInfo{} : clients.front();
}

// spec connection.md "Client Initiated Connections": the server advertises its listening port and
// path, and names itself; a provider that cannot start fails the start, and none is started when
// the server neither advertises nor browses. The provider stops with the run.
TEST(ServerDiscovery, StartsTheProviderWithTheAdvertisementAndStopsIt) {
    struct Row {
        const char* name;
        bool advertise;
        bool browse;
        bool refuse;
        StartResult result;
        int starts;
    };
    const Row rows[] = {
        {"Control: advertise and browse", true, true, false, StartResult::OK, 1},
        {"advertise only", true, false, false, StartResult::OK, 1},
        {"browse only", false, true, false, StartResult::OK, 1},
        {"neither", false, false, false, StartResult::OK, 0},
        {"a provider that cannot start", true, true, true, StartResult::DISCOVERY_FAILED, 1},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        auto discovery = std::make_shared<FakeDiscovery>();
        discovery->refuse = row.refuse;
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Living Room";
        config.path = "/custom";
        config.advertise = row.advertise;
        config.browse = row.browse;
        config.discovery = discovery;
        MemoryProvider provider;
        uint16_t port = 0;
        {
            SendspinServer server;
            EXPECT_EQ(server.start(config, &provider, nullptr), row.result);
            port = server.listen_port();
            EXPECT_EQ(discovery->starts(), row.starts);
            if (row.result == StartResult::OK) {
                EXPECT_NE(port, 0);
            }
        }
        EXPECT_EQ(discovery->stops(), row.result == StartResult::OK ? row.starts : 0);
        if (row.starts == 0) {
            continue;
        }
        const DiscoveryRequest request = discovery->request();
        EXPECT_EQ(request.browse, row.browse);
        ASSERT_EQ(request.advertise.has_value(), row.advertise);
        if (row.advertise && row.result == StartResult::OK) {
            EXPECT_EQ(request.advertise->instance, "Living Room");
            EXPECT_EQ(request.advertise->name, "Living Room");
            EXPECT_EQ(request.advertise->path, "/custom");
            EXPECT_EQ(request.advertise->port, port);
        }
    }
}

// A provider that loses the system's mDNS service tells the host, and the server runs on; the
// loss is logged as a warning and the service's return as information
TEST(ServerDiscovery, ReportsDiscoveryUnavailableAndLogsItsReturn) {
    std::mutex mutex;
    std::condition_variable logged;
    std::vector<LogLevel> levels;
    auto discovery = std::make_shared<FakeDiscovery>();
    ServerConfig config = browsing_config(discovery);
    config.log_level = LogLevel::INFO;
    config.log = [&](LogLevel level, std::string_view /*line*/) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            levels.push_back(level);
        }
        logged.notify_all();
    };
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    discovery->unavailable();
    EXPECT_EQ(recorder.wait_for_error("").kind, sendspin::server::ErrorKind::DISCOVERY_UNAVAILABLE);
    EXPECT_NE(server.listen_port(), 0);
    discovery->available();
    std::unique_lock<std::mutex> lock(mutex);
    logged.wait(lock, [&] { return levels.size() >= 2; });
    EXPECT_EQ(levels, (std::vector<LogLevel>{LogLevel::WARN, LogLevel::INFO}));
}

// A client browsing finds is dialed idle, so its first activation declares no activities; after a
// goodbye with `another_server` it is listed as not connected and not dialed again on the server's
// own, and connect() reclaims it with a dial for playback
TEST(ServerDiscovery, DialsAFoundClientIdleAndReclaimsItForPlaybackAfterAnotherServer) {
    auto discovery = std::make_shared<FakeDiscovery>();
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(browsing_config(discovery), &provider, &recorder), StartResult::OK);
    RawClient client;
    const std::string id = id_of(client);
    discovery->found(service_of(client, "Kitchen"));
    recorder.wait_for("seen Kitchen");
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Kitchen")),
              activation("[]", R"(["metadata@v1"])"));
    recorder.wait_for("connected " + id);
    ClientInfo info = only_client(server);
    EXPECT_TRUE(info.connected);
    EXPECT_TRUE(info.discovered);
    EXPECT_FALSE(info.accepted);
    EXPECT_EQ(info.url, client.url());

    client.send_json(client_goodbye_json("another_server"));
    client.read_until_closed();
    recorder.wait_for(disconnected(id, DisconnectReason::GOODBYE));
    info = only_client(server);
    EXPECT_EQ(info.client_id, id);
    EXPECT_FALSE(info.connected);
    EXPECT_FALSE(info.reconnecting);

    EXPECT_FALSE(server.connect("not listed"));
    ASSERT_TRUE(server.connect(id));
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Kitchen")),
              activation(R"(["playback"])", R"(["metadata@v1"])"));
    recorder.wait_for_count("connected " + id, 2);
    // connect() lifted the suppression: a drop is dialed again, idle
    client.drop();
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Kitchen")),
              activation("[]", R"(["metadata@v1"])"));
    recorder.wait_for_count("connected " + id, 3);
}

// connection.md "Multiple servers (server-initiated)": a client that refuses the connection with
// `concurrent_attempt` before taking it with a client/state was never connected, so the host hears
// of no connection and the client is not listed; the server retries later
TEST(ServerDiscovery, RetriesARefusedDialWithoutReportingItConnected) {
    auto discovery = std::make_shared<FakeDiscovery>();
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(browsing_config(discovery), &provider, &recorder), StartResult::OK);
    RawClient client;
    const std::string id = id_of(client);
    discovery->found(service_of(client, "Busy"));
    const std::string hello = client_hello_json("Busy");
    EXPECT_EQ(accept_and_activate(client, hello, false), activation("[]", R"(["metadata@v1"])"));
    client.send_json(client_goodbye_json("concurrent_attempt"));
    client.read_until_closed();
    const sendspin::server::ServerError refused = recorder.wait_for_error(client.url());
    EXPECT_EQ(refused.kind, sendspin::server::ErrorKind::GOODBYE_BEFORE_ACTIVATION);
    EXPECT_EQ(refused.goodbye_reason, sendspin::server::GoodbyeReason::CONCURRENT_ATTEMPT);
    // Listed as found, not as the client the dial reached
    EXPECT_EQ(only_client(server).client_id, "");

    EXPECT_EQ(accept_and_activate(client, hello), activation("[]", R"(["metadata@v1"])"));
    recorder.wait_for("connected " + id);
    EXPECT_EQ(recorder.count("connected " + id), 1U);
    EXPECT_EQ(only_client(server).client_id, id);
    for (const Recorder::Entry& entry : recorder.entries()) {
        EXPECT_NE(entry.what.rfind("disconnected " + id, 0), 0U) << entry.what;
    }
}

// `restart` and a drop dial the client again on the server's own; `user_request` does not, until
// the client stops advertising and advertises again
TEST(ServerDiscovery, ReconnectsAfterARestartOrADropAndOnceAClientAdvertisesAgain) {
    auto discovery = std::make_shared<FakeDiscovery>();
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(browsing_config(discovery), &provider, &recorder), StartResult::OK);
    RawClient client;
    const std::string id = id_of(client);
    const std::string idle = activation("[]", R"(["metadata@v1"])");
    discovery->found(service_of(client, "Den"));
    // Each connection ends only once the server counted the client connected, so the end cannot
    // overtake the client/state that took the connection
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Den")), idle);
    recorder.wait_for_count("connected " + id, 1);

    client.send_json(client_goodbye_json("restart"));
    client.read_until_closed();
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Den")), idle);
    recorder.wait_for_count("connected " + id, 2);

    client.drop();
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Den")), idle);
    recorder.wait_for_count("connected " + id, 3);

    client.send_json(client_goodbye_json("user_request"));
    client.read_until_closed();
    recorder.wait_for(disconnected(id, DisconnectReason::GOODBYE));
    EXPECT_FALSE(only_client(server).reconnecting);
    // Seen again once found again, and dialed
    discovery->lost("Den");
    discovery->found(service_of(client, "Den"));
    recorder.wait_for_count("seen Den", 2);
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Den")), idle);
    recorder.wait_for_count("connected " + id, 4);
}

// A client held at no activities declares `playback` once its group plays, keeping its roles;
// dialed again after a drop while its group plays, the dial is for playback, so its first
// activation declares it
TEST(ServerDiscovery, DialsForPlaybackBackIntoAGroupThatPlays) {
    auto discovery = std::make_shared<FakeDiscovery>();
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(browsing_config(discovery), &provider, &recorder), StartResult::OK);
    RawClient client;
    const std::string hello = sendspin::server::test::player_hello_json(
        "Player", R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 200000);
    discovery->found(service_of(client, "Player"));
    EXPECT_EQ(accept_and_activate(client, hello), activation("[]", R"(["player@v1"])"));

    const sendspin::server::OpenedInput opened = server.open_input(
        *server.default_group(), InputOptions{InputFormat{48000, 2, SampleFormat::S16}});
    ASSERT_TRUE(opened.input);
    {
        const sendspin::server::test::SilenceFeeder feeder(opened.input);
        EXPECT_EQ(client.receive_until("server/activate"),
                  R"({"type":"server/activate","payload":{"activities":["playback"]}})");
        recorder.wait_for("connected " + id_of(client));
        client.drop();
        EXPECT_EQ(accept_and_activate(client, hello),
                  activation(R"(["playback"])", R"(["player@v1"])"));
    }
    // The run ended with the input's stop: the minimal activities again
    EXPECT_EQ(client.receive_until("server/activate"),
              R"({"type":"server/activate","payload":{"activities":[]}})");
}

// A client found by browsing that is lost while its dial is in progress frees the dial's place
// (connection_limits::MAX_DIALING), and one lost while its dial waits for a place is not dialed
TEST(ServerDiscovery, FreesTheDialPlaceOfALostClient) {
    using sendspin::server::connection_limits::MAX_DIALING;
    using sendspin::server::test::SilentListener;
    auto discovery = std::make_shared<FakeDiscovery>();
    ServerConfig config = browsing_config(discovery);
    // Only the test frees a place
    config.timing.handshake_step_timeout_us = sendspin::server::config_limits::MAX_TIMING_US;
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    SilentListener listener;
    const auto find = [&](const std::string& name) {
        DiscoveredClient service;
        service.instance = name;
        service.addresses = {"127.0.0.1"};
        service.port = listener.port();
        service.path = "/" + name;
        service.name = name;
        discovery->found(service);
    };
    for (size_t i = 0; i + 1 < MAX_DIALING; ++i) {
        ASSERT_TRUE(server.connect_url(listener.url("/held" + std::to_string(i))));
    }
    find("dialing");
    find("lost-waiting");
    ASSERT_TRUE(server.connect_url(listener.url("/waiting")));

    // Every dial but the found client's completes its upgrade and waits in its handshake
    std::vector<std::unique_ptr<SilentListener::WebSocket>> held;
    std::unique_ptr<SilentListener::Socket> dialing;
    for (size_t i = 0; i < MAX_DIALING; ++i) {
        std::unique_ptr<SilentListener::Socket> socket = listener.accept();
        const auto request = sendspin::server::test::upgrade_request(*socket);
        if (request.target() == "/dialing") {
            dialing = std::move(socket);
        } else {
            held.push_back(sendspin::server::test::complete_upgrade(std::move(socket), request));
        }
    }
    ASSERT_NE(dialing, nullptr);

    discovery->lost("lost-waiting");
    discovery->lost("dialing");
    std::unique_ptr<SilentListener::Socket> next = listener.accept();
    EXPECT_EQ(sendspin::server::test::upgrade_request(*next).target(), "/waiting");
}

// cancel_connect_url() while the client is connected: the connection stays, and once it drops the
// URL is not dialed again on the server's own, where a drop otherwise is
TEST(ServerDiscovery, DoesNotRedialACancelledUrlWhenItsConnectionDrops) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel ? "cancelled while connected" : "Control: not cancelled");
        MemoryProvider provider;
        Recorder recorder;
        SendspinServer server;
        ServerConfig config = browsing_config(std::make_shared<FakeDiscovery>());
        config.browse = false;
        ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
        RawClient client;
        const std::string id = id_of(client);
        ASSERT_TRUE(server.connect_url(client.url()));
        accept_and_activate(client, client_hello_json("Hall"));
        recorder.wait_for("connected " + id);
        if (cancel) {
            ASSERT_TRUE(server.cancel_connect_url(client.url()));
            EXPECT_TRUE(only_client(server).connected);
        }
        client.drop();
        recorder.wait_for(disconnected(id, DisconnectReason::CONNECTION_LOST));
        // The snapshot published with the close says whether the server dials it again
        EXPECT_EQ(only_client(server).reconnecting, !cancel);
        if (!cancel) {
            accept_and_activate(client, client_hello_json("Hall"));
            recorder.wait_for_count("connected " + id, 2);
        }
    }
}

// disconnect() suppresses a client found by browsing, and its instance lost and found again lifts
// the suppression, as for a goodbye that stops reconnecting
TEST(ServerDiscovery, SuppressesAFoundClientOnDisconnectUntilItAdvertisesAgain) {
    auto discovery = std::make_shared<FakeDiscovery>();
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(browsing_config(discovery), &provider, &recorder), StartResult::OK);
    RawClient client;
    const std::string id = id_of(client);
    const std::string idle = activation("[]", R"(["metadata@v1"])");
    discovery->found(service_of(client, "Attic"));
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Attic")), idle);
    recorder.wait_for("connected " + id);

    ASSERT_TRUE(server.disconnect(id));
    client.read_until_closed();
    recorder.wait_for(disconnected(id, DisconnectReason::DISCONNECTED_BY_HOST));
    EXPECT_FALSE(only_client(server).reconnecting);
    EXPECT_FALSE(only_client(server).connected);

    discovery->lost("Attic");
    discovery->found(service_of(client, "Attic"));
    recorder.wait_for_count("seen Attic", 2);
    EXPECT_EQ(accept_and_activate(client, client_hello_json("Attic")), idle);
    recorder.wait_for_count("connected " + id, 2);
}

}  // namespace
