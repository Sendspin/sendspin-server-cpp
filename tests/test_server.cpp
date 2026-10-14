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

/// @file test_server.cpp
/// @brief SendspinServer through its public API: the server key's persistence, the threading
/// contract of the listener and the trace, and connections to in-process sendspin-cpp clients
/// that the server activates and keeps time-synced.

#include "crypto/keys.h"
#include "peer.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using sendspin::server::ClientDisconnect;
using sendspin::server::ClientInfo;
using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::Recorder;
namespace persistence_keys = sendspin::server::persistence_keys;
using Bytes = std::vector<uint8_t>;

std::string server_id_of(const Bytes& private_key) {
    std::array<uint8_t, sendspin::server::X25519_KEY_SIZE> bytes{};
    EXPECT_EQ(private_key.size(), bytes.size());
    std::copy(private_key.begin(), private_key.end(), bytes.begin());
    return sendspin::server::Identity::from_private_bytes(bytes)->peer_id();
}

ServerConfig test_config() {
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Test server";
    return config;
}

// ==============================================================================
// Persistence
// ==============================================================================

// connection.md "Identities": the static key pair is long-lived, so it is created once and kept
TEST(ServerPersistence, CreatesAndSavesTheKeyOnFirstStartOrFails) {
    struct Row {
        const char* name;
        std::function<void(MemoryProvider&)> setup;
        StartResult result;
        int saves;
        int commits;
    };
    const Row rows[] = {
        {"Control: first start", [](MemoryProvider&) {}, StartResult::OK, 1, 1},
        {"save refused", [](MemoryProvider& p) { p.refuse_save = true; },
         StartResult::PERSISTENCE_FAILED, 1, 0},
        {"commit refused", [](MemoryProvider& p) { p.refuse_commit = true; },
         StartResult::PERSISTENCE_FAILED, 1, 1},
        // Never overwritten: a new key is a different server to every client
        {"a stored key of the wrong size",
         [](MemoryProvider& p) { p.blobs[persistence_keys::SERVER_KEY] = Bytes(31, 7); },
         StartResult::PERSISTENCE_FAILED, 0, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        MemoryProvider provider;
        row.setup(provider);
        SendspinServer server;
        EXPECT_EQ(server.start(test_config(), &provider, nullptr), row.result);
        EXPECT_EQ(provider.saves, row.saves);
        EXPECT_EQ(provider.commits, row.commits);
        if (row.result != StartResult::OK) {
            EXPECT_EQ(server.server_id(), "");
            EXPECT_FALSE(server.connect_url("ws://127.0.0.1:1/sendspin"));
            continue;
        }
        ASSERT_EQ(provider.blobs.count(persistence_keys::SERVER_KEY), 1U);
        const Bytes& key = provider.blobs[persistence_keys::SERVER_KEY];
        EXPECT_EQ(key.size(), persistence_keys::SERVER_KEY_SIZE);
        EXPECT_EQ(server.server_id(), server_id_of(key));
        EXPECT_NE(provider.thread, std::this_thread::get_id());
    }

    SendspinServer server;
    EXPECT_EQ(server.start(test_config(), nullptr, nullptr), StartResult::PERSISTENCE_FAILED);
}

// A configuration the server could not honor is refused before anything starts
TEST(ServerConfiguration, RefusesValuesOutOfRange) {
    namespace limits = sendspin::server::config_limits;
    struct Row {
        const char* name;
        std::function<void(ServerConfig&)> edit;
        StartResult result;
    };
    const Row rows[] = {
        {"Control: every value at its limit",
         [](ServerConfig& c) {
             c.name = std::string(limits::MAX_NAME_BYTES, 'n');
             c.languages.assign(limits::MAX_LANGUAGES,
                                std::string(limits::MAX_LANGUAGE_TAG_BYTES, 'l'));
             c.timing.handshake_step_timeout_us = limits::MAX_TIMING_US;
             c.timing.write_stall_us = limits::MAX_TIMING_US;
             c.timing.handshake_backoff_min_us = limits::MAX_TIMING_US;
             c.timing.handshake_backoff_max_us = limits::MAX_TIMING_US;
             c.timing.group_rejoin_grace_us = limits::MAX_TIMING_US;
             c.path = "/" + std::string(limits::MAX_PATH_BYTES - 1, 'p');
             c.log_level = sendspin::server::LogLevel::INFO;
         },
         StartResult::OK},
        {"Control: the shortest timings",
         [](ServerConfig& c) {
             c.timing.handshake_step_timeout_us = 1;
             c.timing.write_stall_us = 1;
             c.timing.handshake_backoff_min_us = 1;
             c.timing.handshake_backoff_max_us = 1;
             c.timing.group_rejoin_grace_us = 1;
         },
         StartResult::OK},
        {"a long name",
         [](ServerConfig& c) { c.name = std::string(limits::MAX_NAME_BYTES + 1, 'n'); },
         StartResult::INVALID_CONFIG},
        {"too many languages",
         [](ServerConfig& c) { c.languages.assign(limits::MAX_LANGUAGES + 1, "en"); },
         StartResult::INVALID_CONFIG},
        {"a long language tag",
         [](ServerConfig& c) {
             c.languages = {"en", std::string(limits::MAX_LANGUAGE_TAG_BYTES + 1, 'l')};
         },
         StartResult::INVALID_CONFIG},
        {"no backoff", [](ServerConfig& c) { c.timing.handshake_backoff_min_us = 0; },
         StartResult::INVALID_CONFIG},
        {"a backoff maximum below its minimum",
         [](ServerConfig& c) {
             c.timing.handshake_backoff_max_us = c.timing.handshake_backoff_min_us - 1;
         },
         StartResult::INVALID_CONFIG},
        {"no write stall time", [](ServerConfig& c) { c.timing.write_stall_us = 0; },
         StartResult::INVALID_CONFIG},
        {"no rejoin grace", [](ServerConfig& c) { c.timing.group_rejoin_grace_us = 0; },
         StartResult::INVALID_CONFIG},
        {"a rejoin grace past the limit",
         [](ServerConfig& c) { c.timing.group_rejoin_grace_us = limits::MAX_TIMING_US + 1; },
         StartResult::INVALID_CONFIG},
        {"no step timeout", [](ServerConfig& c) { c.timing.handshake_step_timeout_us = 0; },
         StartResult::INVALID_CONFIG},
        {"a step timeout past the limit",
         [](ServerConfig& c) { c.timing.handshake_step_timeout_us = limits::MAX_TIMING_US + 1; },
         StartResult::INVALID_CONFIG},
        {"a write stall time past the limit",
         [](ServerConfig& c) { c.timing.write_stall_us = limits::MAX_TIMING_US + 1; },
         StartResult::INVALID_CONFIG},
        {"a backoff minimum past the limit",
         [](ServerConfig& c) {
             c.timing.handshake_backoff_min_us = limits::MAX_TIMING_US + 1;
             c.timing.handshake_backoff_max_us = limits::MAX_TIMING_US + 1;
         },
         StartResult::INVALID_CONFIG},
        {"a backoff maximum past the limit",
         [](ServerConfig& c) { c.timing.handshake_backoff_max_us = limits::MAX_TIMING_US + 1; },
         StartResult::INVALID_CONFIG},
        {"no path", [](ServerConfig& c) { c.path.clear(); }, StartResult::INVALID_CONFIG},
        {"a path without '/'", [](ServerConfig& c) { c.path = "sendspin"; },
         StartResult::INVALID_CONFIG},
        {"a path with a query", [](ServerConfig& c) { c.path = "/sendspin?x"; },
         StartResult::INVALID_CONFIG},
        {"a path past the limit",
         [](ServerConfig& c) { c.path = "/" + std::string(limits::MAX_PATH_BYTES, 'p'); },
         StartResult::INVALID_CONFIG},
        {"a log level past INFO",
         [](ServerConfig& c) {
             c.log_level = static_cast<sendspin::server::LogLevel>(
                 static_cast<int>(sendspin::server::LogLevel::INFO) + 1);
         },
         StartResult::INVALID_CONFIG},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        ServerConfig config = test_config();
        row.edit(config);
        MemoryProvider provider;
        SendspinServer server;
        EXPECT_EQ(server.start(config, &provider, nullptr), row.result);
        // Refused before the key is created
        EXPECT_EQ(provider.saves, row.result == StartResult::OK ? 1 : 0);
        EXPECT_EQ(server.server_id().empty(), row.result != StartResult::OK);
    }
}

TEST(ServerPersistence, LoadsTheSameKeyOnLaterStarts) {
    MemoryProvider provider;
    std::string first_id;
    {
        SendspinServer server;
        ASSERT_EQ(server.start(test_config(), &provider, nullptr), StartResult::OK);
        first_id = server.server_id();
        EXPECT_EQ(server.start(test_config(), &provider, nullptr), StartResult::ALREADY_RUNNING);
        // A stopped server starts again, from the stored key
        server.stop();
        EXPECT_EQ(server.server_id(), "");
        ASSERT_EQ(server.start(test_config(), &provider, nullptr), StartResult::OK);
        EXPECT_EQ(server.server_id(), first_id);
    }
    SendspinServer server;
    ASSERT_EQ(server.start(test_config(), &provider, nullptr), StartResult::OK);
    EXPECT_EQ(server.server_id(), first_id);
    EXPECT_EQ(server.server_id(), server_id_of(provider.blobs[persistence_keys::SERVER_KEY]));
    EXPECT_EQ(provider.saves, 1);
}

// ==============================================================================
// An in-process client
// ==============================================================================

/// A sendspin-cpp client listening on a loopback port for the server to dial. Its main loop runs
/// on the test thread through run_until().
class PeerClient {
public:
    PeerClient() : PeerClient(sendspin::server::test::free_port()) {}

    /// Listens on port, for a URL the server was given before the peer started
    explicit PeerClient(uint16_t port)
        : port_(port), client_(sendspin::server::test::peer_config("sendspin-cpp peer", port_)) {
        sendspin::PlayerRoleConfig player;
        // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list
        // assignment
        player.audio_formats.assign(1, sendspin::AudioSupportedFormatObject{
                                           sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
        this->client_.add_player(player);
        this->client_.add_metadata();
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~PeerClient() {
        this->client_.stop();
    }

    PeerClient(const PeerClient&) = delete;
    PeerClient& operator=(const PeerClient&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    /// Runs the client's main loop until done() holds, with no timeout
    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::run_until(this->client_, done);
    }

private:
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
};

/// The client's clock, sendspin-cpp's host platform_time_us()
int64_t client_clock_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Index of the first log entry equal to what, or the log's size
size_t index_of(const std::vector<Recorder::Entry>& entries, const std::string& what) {
    return static_cast<size_t>(
        std::find_if(entries.begin(), entries.end(),
                     [&](const Recorder::Entry& entry) { return entry.what == what; }) -
        entries.begin());
}

// ==============================================================================
// Connections
// ==============================================================================

// messaging.md "Communication" and "Clock Synchronization": the server activates the client,
// which counts as connected only once it has the activation, and answers its time requests well
// enough for its time filter to map the server's clock onto its own
TEST(ServerConnection, ActivatesASendspinCppClientAndKeepsItTimeSynced) {
    MemoryProvider provider;
    Recorder recorder;
    ServerConfig config = test_config();
    config.trace = [&recorder](const TracedMessage& message) { recorder.trace(message); };
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    PeerClient peer;
    ASSERT_TRUE(server.connect_url(peer.url()));

    // Two full bursts answered keep the filter fed; the client stays connected throughout
    constexpr size_t BURSTS = 2;
    const size_t replies = BURSTS * sendspin::SendspinClientConfig::DEFAULT_BURST_SIZE;
    peer.run_until([&] {
        return peer.client().is_time_synced() && recorder.count("out server/time") >= replies;
    });
    sendspin::SendspinClient& client = peer.client();
    EXPECT_TRUE(client.is_connected());
    ASSERT_TRUE(client.get_server_information());
    EXPECT_EQ(client.get_server_information()->server_id, server.server_id());
    EXPECT_EQ(client.get_server_information()->name, "Test server");

    // Far above loopback delay and far below the error of a misplaced stamp, which the two
    // processes' unrelated clock origins would make seconds to days; the client's clock is read
    // on both sides of the server's, so a thread held up between the reads widens nothing
    constexpr int64_t TOLERANCE_US = 100000;
    const int64_t before = client_clock_us();
    const int64_t mapped = client.get_client_time(server.now_us());
    const int64_t after = client_clock_us();
    EXPECT_GT(mapped, before - TOLERANCE_US) << mapped - before;
    EXPECT_LT(mapped, after + TOLERANCE_US) << mapped - after;

    recorder.wait_for("connected " + client.client_id());
    const std::vector<ClientInfo> clients = server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_EQ(clients[0].client_id, client.client_id());
    EXPECT_EQ(clients[0].name, "sendspin-cpp peer");
    EXPECT_EQ(clients[0].url, peer.url());
    // The client's own messages after activation (its client/state, say) did not close it
    EXPECT_FALSE(
        recorder.contains("disconnected " + client.client_id() + " " +
                          std::to_string(static_cast<int>(DisconnectReason::CLOSED_BY_SERVER))));
    // A second dial to a connected URL is kept as the one connection
    EXPECT_TRUE(server.connect_url(peer.url()));

    // messaging.md "client/goodbye": the client's stop() says goodbye
    client.stop();
    const std::string disconnected = "disconnected " + client.client_id() + " " +
                                     std::to_string(static_cast<int>(DisconnectReason::GOODBYE));
    recorder.wait_for(disconnected);
    // Listed as not connected, since the server can dial its URL again
    ASSERT_EQ(server.clients().size(), 1U);
    EXPECT_FALSE(server.clients()[0].connected);
    EXPECT_EQ(server.clients()[0].client_id, client.client_id());
    EXPECT_EQ(recorder.count("connected " + client.client_id()), 1U);
}

// spec connection.md "Client Initiated Connections": a sendspin-cpp client that connects to the
// server is activated and kept time-synced as a dialed one is, and is not listed once gone, since
// the server cannot dial it
TEST(ServerConnection, ActivatesASendspinCppClientThatConnects) {
    MemoryProvider provider;
    Recorder recorder;
    ServerConfig config = test_config();
    config.approve_all_clients = true;
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    PeerClient peer;
    sendspin::SendspinClient& client = peer.client();
    client.connect_to("ws://127.0.0.1:" + std::to_string(server.listen_port()) + "/sendspin");
    const std::string id = client.client_id();
    peer.run_until([&] { return client.is_time_synced() && recorder.contains("connected " + id); });
    ASSERT_TRUE(client.get_server_information());
    EXPECT_EQ(client.get_server_information()->server_id, server.server_id());
    const std::vector<ClientInfo> clients = server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].accepted);
    EXPECT_EQ(clients[0].url, "");

    client.stop();
    recorder.wait_for("disconnected " + id + " " +
                      std::to_string(static_cast<int>(DisconnectReason::GOODBYE)));
    EXPECT_TRUE(server.clients().empty());
}

// The threading contract: listener and trace calls run on one dispatch thread, the provider's
// too, in the order the core thread produced them
TEST(ServerThreading, DeliversEveryCallOnTheDispatchThreadInOrder) {
    MemoryProvider provider;
    Recorder recorder;
    ServerConfig config = test_config();
    config.trace = [&recorder](const TracedMessage& message) { recorder.trace(message); };
    SendspinServer server;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    PeerClient peer;
    ASSERT_TRUE(server.connect_url(peer.url()));
    peer.run_until([&] { return recorder.contains("out server/time"); });
    const std::string id = peer.client().client_id();
    peer.client().stop();
    const std::string disconnected =
        "disconnected " + id + " " + std::to_string(static_cast<int>(DisconnectReason::GOODBYE));
    recorder.wait_for(disconnected);

    const std::vector<Recorder::Entry> entries = recorder.entries();
    for (const Recorder::Entry& entry : entries) {
        SCOPED_TRACE(entry.what);
        EXPECT_EQ(entry.thread, provider.thread);
    }
    EXPECT_NE(provider.thread, std::this_thread::get_id());

    // The handshake, then the connection report ahead of the activation it queued, then time
    // sync, then the goodbye and its report. server/hello is not in the chain: the peer sends
    // client/hello once the Noise handshake completes, which can be before server/hello is taken.
    const std::vector<std::string> order{
        "in client/init",  "out server/init",   "out noise/handshake", "in noise/handshake",
        "in client/hello", "connected " + id,   "out server/activate", "in client/time",
        "out server/time", "in client/goodbye", disconnected,
    };
    size_t previous = 0;
    for (const std::string& what : order) {
        SCOPED_TRACE(what);
        const size_t index = index_of(entries, what);
        ASSERT_LT(index, entries.size());
        EXPECT_GE(index, previous);
        previous = index;
    }
    EXPECT_LT(index_of(entries, "out server/hello"), index_of(entries, "out server/activate"));
}

// stop() from a listener only starts the shutdown, and the clients still connected are reported
// as stopped after it
TEST(ServerThreading, StopsFromAListenerAndStartsAgain) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    recorder.stop_on_connect = &server;
    ASSERT_EQ(server.start(test_config(), &provider, &recorder), StartResult::OK);
    const std::string server_id = server.server_id();
    PeerClient peer;
    ASSERT_TRUE(server.connect_url(peer.url()));
    const std::string id = peer.client().client_id();
    const std::string stopped = "disconnected " + id + " " +
                                std::to_string(static_cast<int>(DisconnectReason::SERVER_STOPPED));
    peer.run_until([&] { return recorder.contains(stopped); });
    EXPECT_TRUE(server.clients().empty());
    EXPECT_EQ(server.server_id(), "");
    EXPECT_FALSE(server.connect_url(peer.url()));
    // The client sees the server close the connection
    peer.run_until([&] { return !peer.client().is_connected(); });

    recorder.stop_on_connect = nullptr;
    ASSERT_EQ(server.start(test_config(), &provider, &recorder), StartResult::OK);
    EXPECT_EQ(server.server_id(), server_id);
}

// A listener call made for the destructor's stop may call the server while the destructor waits
// for it, and gets a stopped server's answers. A destructor that released its state first fails
// here every time under libc++, whose ~unique_ptr nulls the pointer first, and likely under
// libstdc++.
TEST(ServerThreading, AListenerCallOfTheDestructorsStopMayCallTheServer) {
    class CallsBack : public Recorder {
    public:
        void on_client_disconnected(const ClientInfo& client,
                                    const ClientDisconnect& disconnect) override {
            Recorder::on_client_disconnected(client, disconnect);
            this->groups_listed = this->server->groups().size();
            this->default_group = this->server->default_group().has_value();
        }

        SendspinServer* server{nullptr};
        size_t groups_listed{1};
        bool default_group{true};
    };
    MemoryProvider provider;
    CallsBack listener;
    auto server = std::make_unique<SendspinServer>();
    listener.server = server.get();
    ASSERT_EQ(server->start(test_config(), &provider, &listener), StartResult::OK);
    PeerClient peer;
    ASSERT_TRUE(server->connect_url(peer.url()));
    const std::string id = peer.client().client_id();
    peer.run_until([&] { return listener.contains("connected " + id); });
    server.reset();
    EXPECT_EQ(listener.disconnects().size(), 1U);
    EXPECT_EQ(listener.groups_listed, 0U);
    EXPECT_FALSE(listener.default_group);
}

// The destructor stops the server and joins the dispatch thread, so a listener call made for the
// stop has finished when it returns. The listener's delay makes a destructor that does not wait
// return first, which the counter shows; under ThreadSanitizer its unsynchronized write is a
// reported race as well.
TEST(ServerThreading, DestructorWaitsForTheListenerCallsOfItsStop) {
    MemoryProvider provider;
    Recorder recorder;
    recorder.disconnect_delay = std::chrono::milliseconds(100);
    auto server = std::make_unique<SendspinServer>();
    ASSERT_EQ(server->start(test_config(), &provider, &recorder), StartResult::OK);
    PeerClient peer;
    ASSERT_TRUE(server->connect_url(peer.url()));
    const std::string id = peer.client().client_id();
    peer.run_until([&] { return recorder.contains("connected " + id); });
    server.reset();
    EXPECT_EQ(recorder.unsynchronized_disconnects, 1);
    EXPECT_TRUE(
        recorder.contains("disconnected " + id + " " +
                          std::to_string(static_cast<int>(DisconnectReason::SERVER_STOPPED))));
}

// ConnectOptions::retry_until_connected: a URL whose first dial fails is dialed again on the
// server's own, so a client that starts listening later is reached, and the retry keeps the
// operator's dial intent: its first activation declares `playback`, where a later dial on the
// server's own would be idle
TEST(ServerConnection, RetriesAUrlUntilItsClientListens) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = test_config();
    config.timing.handshake_backoff_min_us = 20000;
    config.timing.handshake_backoff_max_us = 20000;
    config.approve_all_clients = true;
    std::mutex mutex;
    std::vector<std::string> activations;
    config.trace = [&](const TracedMessage& message) {
        const std::string text(message.plaintext.begin(), message.plaintext.end());
        if (message.direction == TraceDirection::OUTBOUND &&
            text.find("server/activate") != std::string::npos) {
            const std::lock_guard<std::mutex> lock(mutex);
            activations.push_back(text);
        }
    };
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const uint16_t port = sendspin::server::test::free_port();
    const std::string url = sendspin::server::test::peer_url(port);
    sendspin::server::ConnectOptions options;
    options.retry_until_connected = true;
    ASSERT_TRUE(server.connect_url(url, options));
    recorder.wait_for("error " + std::to_string(static_cast<int>(ErrorKind::DIAL_FAILED)) + " " +
                      url);
    PeerClient peer(port);
    const std::string id = peer.client().client_id();
    peer.run_until([&] { return recorder.contains("connected " + id); });
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_FALSE(activations.empty());
    EXPECT_NE(activations.front().find(R"("activities":["playback"])"), std::string::npos)
        << activations.front();
}

// SendspinServer::disconnect(): the client goes with DISCONNECTED_BY_HOST and stays listed as one
// the server does not dial on its own, though a drop of the connection would have it reconnect
TEST(ServerConnection, DisconnectsAClientAndStopsDialingIt) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(test_config(), &provider, &recorder), StartResult::OK);
    PeerClient peer;
    EXPECT_FALSE(server.disconnect("not-a-client"));
    ASSERT_TRUE(server.connect_url(peer.url()));
    const std::string id = peer.client().client_id();
    peer.run_until([&] { return recorder.contains("connected " + id); });
    ASSERT_TRUE(server.disconnect(id));
    peer.run_until([&] {
        return recorder.contains(
            "disconnected " + id + " " +
            std::to_string(static_cast<int>(DisconnectReason::DISCONNECTED_BY_HOST)));
    });
    const std::vector<ClientInfo> clients = server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_FALSE(clients.front().connected);
    EXPECT_FALSE(clients.front().reconnecting);
    EXPECT_FALSE(server.disconnect(id));
}

TEST(ServerConnection, ReportsADialThatCannotConnect) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    EXPECT_FALSE(server.connect_url("ws://127.0.0.1:1/sendspin"));
    ASSERT_EQ(server.start(test_config(), &provider, &recorder), StartResult::OK);
    EXPECT_FALSE(server.connect_url("http://127.0.0.1:1/sendspin"));
    EXPECT_FALSE(server.connect_url("ws://"));
    // Nothing listens on a port that was free a moment ago
    const std::string url =
        "ws://127.0.0.1:" + std::to_string(sendspin::server::test::free_port()) + "/sendspin";
    ASSERT_TRUE(server.connect_url(url));
    recorder.wait_for("error " + std::to_string(static_cast<int>(ErrorKind::DIAL_FAILED)) + " " +
                      url);
    EXPECT_TRUE(server.clients().empty());
}

}  // namespace
