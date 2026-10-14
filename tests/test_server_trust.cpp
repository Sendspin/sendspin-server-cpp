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

/// @file test_server_trust.cpp
/// @brief Trust end to end: a paired sendspin-cpp client plays under its long-term PSK, one that
/// lost its record falls back to the Sentinel and is held, newer clients are reported, and
/// through ServerCore on its own core thread a pairing PSK is referenced, a long-term PSK is
/// used only once it is durable, one change at a time, and holds the client's unpaired sessions,
/// a save may outlive the core, and re-handshakes switch keys under playback and end a mismatch.
///
/// White-box: the ServerCore tests set ServerCore::pairing_psks_, the state an operator-started
/// Pairing PSK pairing sets, call ServerCore::rekey() and handshake_psk(), which pairing drives,
/// and read ServerCore::records_ for the record (see tests/CMakeLists.txt).

#include "client_records.h"
#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "dispatcher.h"
#include "host_view.h"
#include "peer.h"
#include "player_sink.h"
#include "raw_client.h"
#include "server_core.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "transport.h"
#include "trust_fixtures.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::ClientRecord;
using sendspin::server::HandshakePsk;
using sendspin::server::Identity;
using sendspin::server::Psk;
using sendspin::server::PskCategory;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TraceDirection;
using sendspin::server::TracedMessage;
using sendspin::server::test::Bytes;
using sendspin::server::test::ClientStore;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::psk_of;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::stored_record;
namespace persistence_keys = sendspin::server::persistence_keys;

Bytes record_blob(const ClientRecord& record) {
    sendspin::server::ClientRecordBlob blob{};
    EXPECT_TRUE(sendspin::server::encode_client_record(record, blob));
    return {blob.begin(), blob.end()};
}

constexpr uint32_t RATE = 48000;

/// The trust a sendspin-cpp client reports for its connection: USER for a long-term PSK, NONE for
/// the Sentinel or a pairing PSK
class TrustListener : public sendspin::SendspinClientListener {
public:
    void on_trust_changed(sendspin::ConnectionTrust trust) override {
        this->trust = trust;
        ++this->reports;
    }

    std::atomic<sendspin::ConnectionTrust> trust{sendspin::ConnectionTrust::NONE};
    std::atomic<int> reports{0};
};

/// A sendspin-cpp player listening on a loopback port for the server to dial, with a real-time
/// audio output. Its main loop runs on the test thread through run_until().
class Peer {
public:
    struct Options {
        bool unpaired_access{true};
        std::optional<Psk> pairing_psk;
    };

    explicit Peer(ClientStore& store, const Options& options)
        : port_(sendspin::server::test::free_port()), client_(make_config(port_, options)) {
        sendspin::PlayerRoleConfig player;
        // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list
        // assignment
        player.audio_formats.assign(1, sendspin::AudioSupportedFormatObject{
                                           sendspin::SendspinCodecFormat::PCM, 2, RATE, 16});
        player.min_buffer_ms = 150;
        sendspin::PlayerRole& role = this->client_.add_player(player);
        this->sink_ = std::make_unique<sendspin::server::test::Sink>(role, 4, RATE);
        role.set_listener(this->sink_.get());
        this->client_.add_metadata();
        this->client_.set_network_provider(&this->network_);
        this->client_.set_persistence_provider(&store);
        this->client_.set_listener(&this->trust_);
        this->client_.set_unpaired_access_enabled(options.unpaired_access);
        EXPECT_TRUE(this->client_.start());
    }

    ~Peer() {
        this->client_.stop();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::string url() const {
        return sendspin::server::test::peer_url(this->port_);
    }

    sendspin::SendspinClient& client() {
        return this->client_;
    }

    const sendspin::server::test::Sink& sink() const {
        return *this->sink_;
    }

    const TrustListener& trust() const {
        return this->trust_;
    }

    /// Runs the client's main loop until done() holds, with no timeout
    template <typename Predicate>
    void run_until(Predicate done) {
        sendspin::server::test::run_until(this->client_, done);
    }

private:
    static sendspin::SendspinClientConfig make_config(uint16_t port, const Options& options) {
        sendspin::SendspinClientConfig config =
            sendspin::server::test::peer_config("sendspin-cpp peer", port);
        if (options.pairing_psk) {
            config.pairing_psk = sendspin::SendspinPsk(*options.pairing_psk);
        }
        return config;
    }

    uint16_t port_;
    sendspin::server::test::AlwaysReady network_;
    TrustListener trust_;
    sendspin::SendspinClient client_;
    std::unique_ptr<sendspin::server::test::Sink> sink_;
};

/// Collects the plaintext of the server/activate messages and the log lines a server produces
class Capture {
public:
    void trace(const TracedMessage& message) {
        const std::string text(message.plaintext.begin(), message.plaintext.end());
        if (message.direction == TraceDirection::OUTBOUND &&
            text.find(R"("type":"server/activate")") != std::string::npos) {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->activations_.push_back(text.substr(1));
        }
    }

    void log(std::string_view line) {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->lines_.emplace_back(line);
    }

    std::vector<std::string> activations() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->activations_;
    }

    std::vector<std::string> lines() const {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        return this->lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> activations_;
    std::vector<std::string> lines_;
};

/// A server whose key is known before it starts, so a client can hold a record for it
struct KnownServer {
    KnownServer() : identity(*Identity::generate()) {
        this->provider.blobs[persistence_keys::SERVER_KEY] =
            Bytes(this->identity.private_bytes.begin(), this->identity.private_bytes.end());
    }

    ServerConfig config() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Test server";
        // The capture first, so that an activation the recorder lists is captured
        config.trace = [this](const TracedMessage& message) {
            this->capture.trace(message);
            this->recorder.trace(message);
        };
        config.log = [this](sendspin::server::LogLevel /*level*/, std::string_view line) {
            this->capture.log(line);
        };
        config.log_level = sendspin::server::LogLevel::INFO;
        return config;
    }

    Identity identity;
    MemoryProvider provider;
    Recorder recorder;
    Capture capture;
    SendspinServer server;
};

// ==============================================================================
// Through the public API
// ==============================================================================

// connection.md "Pre-Shared Key": a client the server holds a long-term PSK for is referenced
// under it, and a client holding the same PSK keys a paired session: it plays with neither the
// operator's approval nor unpaired access (messaging.md "server/activate"). Its record keeps the
// name of its client/hello.
TEST(ServerTrust, PlaysAPairedClientUnderItsLongTermPsk) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    const Psk long_term = psk_of(0x30);
    ClientStore store;
    store.hold_record(known.identity.peer_id(), long_term);
    Peer peer(store, {false, std::nullopt});
    const std::string id = peer.client().client_id();
    ClientRecord record{id, std::nullopt};
    record.long_term_psk = long_term;
    known.provider.blobs[persistence_keys::client_record_key(0)] = record_blob(record);

    ASSERT_EQ(known.server.start(known.config(), &known.provider, &known.recorder),
              StartResult::OK);
    ASSERT_TRUE(known.server.connect_url(peer.url()));
    peer.run_until([&] { return known.recorder.contains("connected " + id); });
    const std::vector<std::string> activations = known.capture.activations();
    ASSERT_FALSE(activations.empty());
    EXPECT_NE(activations.front().find(R"("activities":["playback"])"), std::string::npos)
        << activations.front();
    EXPECT_NE(activations.front().find("player@v1"), std::string::npos) << activations.front();
    EXPECT_FALSE(known.recorder.contains("pairing required " + id));
    EXPECT_FALSE(known.recorder.contains("approval needed " + id));
    const std::vector<ClientInfo> clients = known.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].paired);
    EXPECT_FALSE(clients[0].credential_mismatch);
    EXPECT_FALSE(clients[0].approval);
    EXPECT_EQ(peer.trust().trust, sendspin::ConnectionTrust::USER);

    // Saved before the connection was reported
    const std::optional<ClientRecord> stored = stored_record(known.provider, 0);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->name, "sendspin-cpp peer");
    EXPECT_EQ(stored->long_term_psk, long_term);
}

// connection.md "Sentinel Fallback": a client that lost its record answers the long-term PSK
// under the Sentinel. The server reports the authenticated mismatch, keeps the record unchanged,
// and activates neither roles nor 'playback' while the record exists, although the client enables
// unpaired access and the record approves it. No key, PSK or client_id reaches the log.
TEST(ServerTrust, HoldsAClientThatLostItsRecordAndReportsTheMismatch) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    const Psk long_term = psk_of(0x50);
    ClientStore store;
    Peer peer(store, {true, std::nullopt});
    const std::string id = peer.client().client_id();
    ClientRecord record{id, sendspin::server::ApprovalScope::PLAYBACK};
    record.long_term_psk = long_term;
    record.name = "sendspin-cpp peer";
    const Bytes blob = record_blob(record);
    known.provider.blobs[persistence_keys::client_record_key(0)] = blob;

    ASSERT_EQ(known.server.start(known.config(), &known.provider, &known.recorder),
              StartResult::OK);
    ASSERT_TRUE(known.server.connect_url(peer.url()));
    // A client held with no roles is connected before its activation is written
    peer.run_until([&] {
        return known.recorder.contains("connected " + id) &&
               known.recorder.contains("out server/activate");
    });
    // Reported as the activation is decided, ahead of the connection
    EXPECT_TRUE(known.recorder.contains("mismatch " + id));
    const std::vector<std::string> activations = known.capture.activations();
    ASSERT_EQ(activations.size(), 1U);
    EXPECT_EQ(activations.front(),
              R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");
    const std::vector<ClientInfo> clients = known.server.clients();
    ASSERT_EQ(clients.size(), 1U);
    EXPECT_TRUE(clients[0].credential_mismatch);
    EXPECT_TRUE(clients[0].paired);
    // The signal alone never removes or replaces the record
    EXPECT_EQ(known.provider.blobs[persistence_keys::client_record_key(0)], blob);
    EXPECT_EQ(known.provider.saves, 0);

    known.server.stop();
    const std::vector<std::string> lines = known.capture.lines();
    ASSERT_FALSE(lines.empty());
    const std::vector<std::string> secrets = {
        id,
        known.identity.peer_id(),
        sendspin::server::psk_id_for(long_term),
        sendspin::server::b64url_encode(long_term.data(), long_term.size()),
        sendspin::server::b64url_encode(known.identity.private_bytes.data(),
                                        known.identity.private_bytes.size()),
    };
    for (const std::string& line : lines) {
        for (const std::string& secret : secrets) {
            EXPECT_EQ(line.find(secret), std::string::npos) << line;
        }
    }
}

// README "Detecting Outdated Servers", pairing.md "client/hello pair-method descriptor": each role
// version and pairing method the server does not recognize is reported once per connection, a
// repeated one included, up to the first 8 distinct; application-specific ones are not
TEST(ServerTrust, ReportsTheIdentifiersOfANewerClient) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    KnownServer known;
    ASSERT_EQ(known.server.start(known.config(), &known.provider, &known.recorder),
              StartResult::OK);
    RawClient client;
    ASSERT_TRUE(known.server.connect_url(client.url()));
    const std::string id = client.noise.identity().peer_id();
    ASSERT_NO_FATAL_FAILURE(client.activate(
        R"({"type":"client/hello","payload":{"name":"Newer","supported_roles":)"
        R"(["metadata@v1","metadata@v2","metadata@v2","_acme_lamp@v1","a@v2","b@v2","c@v2",)"
        R"("d@v2","e@v2","f@v2"],"supported_pair_methods":{"pairing_psk":{},"future_method":{},)"
        R"("later_method":{}},"unpaired_access":{"enabled":true}}})"));
    // Traced after every report the client/hello caused
    known.recorder.wait_for("out server/activate");
    const std::string prefix = "newer " + id + " ";
    std::vector<std::string> reported;
    for (const auto& entry : known.recorder.entries()) {
        if (entry.what.starts_with(prefix)) {
            reported.push_back(entry.what.substr(prefix.size()));
        }
    }
    const std::vector<std::string> expected = {"metadata@v2", "a@v2", "b@v2", "c@v2",
                                               "d@v2",        "e@v2", "f@v2", "future_method"};
    EXPECT_EQ(reported, expected);
}

// ==============================================================================
// ServerCore on its own core thread
// ==============================================================================

/// A ServerCore with every client approved, running on its own thread until stopped, which then
/// destroys it as a run's core thread does
class Core {
public:
    explicit Core(sendspin::server::ClientRecords records = {},
                  sendspin::server::PersistenceProvider* provider = nullptr)
        : dispatcher(std::make_shared<sendspin::server::Dispatcher>()),
          identity(*Identity::generate()) {
        this->settings.approve_all_clients = true;
        // The capture first, so that an activation the recorder lists is captured
        this->settings.trace = [this](const TracedMessage& message) {
            this->capture.trace(message);
            this->recorder.trace(message);
        };
        this->core = std::make_unique<sendspin::server::ServerCore>(
            this->loop, this->workers, *this->dispatcher, &this->recorder, this->identity,
            this->settings, this->snapshot, this->groups, this->inputs, this->group_ids, provider,
            std::move(records));
        this->thread = std::thread([this] {
            this->loop.run();
            this->core.reset();
        });
    }

    ~Core() {
        this->stop();
        this->dispatcher->finish();
        this->dispatcher->join();
    }

    /// Shuts the core down and waits until its thread destroyed it; the dispatcher runs on
    void stop() {
        if (this->thread.joinable()) {
            this->loop.post([this] { this->core->shutdown(0); });
            this->thread.join();
        }
    }

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    /// Runs fn on the core thread and returns its result
    template <typename Fn>
    auto on_core(Fn fn) {
        std::promise<decltype(fn())> result;
        this->loop.post([&] {
            if constexpr (std::is_void_v<decltype(fn())>) {
                fn();
                result.set_value();
            } else {
                result.set_value(fn());
            }
        });
        return result.get_future().get();
    }

    void dial(const std::string& url) {
        this->loop.post(
            [this, url] { this->core->connect_url(url, *sendspin::server::parse_ws_url(url)); });
    }

    /// Opens an input on the server's group
    std::shared_ptr<sendspin::server::HostView> open_input(uint32_t rate) {
        auto view = std::make_shared<sendspin::server::HostView>(
            this->core->default_group(), sendspin::server::InputId{this->next_input++},
            sendspin::server::InputOptions{
                sendspin::server::InputFormat{rate, 2, sendspin::server::SampleFormat::S16}},
            sendspin::server::GroupParams{250000, 0, 0}, this->dispatcher, &this->recorder);
        EXPECT_TRUE(this->inputs.add(view));
        this->loop.post([this, view] { this->core->attach_input(view); });
        return view;
    }

    sendspin::server::EventLoop loop;
    sendspin::server::AudioWorkers workers{1};
    std::shared_ptr<sendspin::server::Dispatcher> dispatcher;
    Identity identity;
    sendspin::server::CoreSettings settings;
    sendspin::server::ClientSnapshot snapshot;
    sendspin::server::GroupSnapshot groups;
    sendspin::server::InputRegistry inputs;
    std::atomic<uint64_t> group_ids{1};
    Recorder recorder;
    Capture capture;
    std::unique_ptr<sendspin::server::ServerCore> core;
    std::thread thread;
    uint64_t next_input{1};
};

/// Writes the test signal into an input until stopped, from a thread of its own
class Feeder {
public:
    explicit Feeder(std::shared_ptr<sendspin::server::HostView> view)
        : view_(std::move(view)), thread_([this] { this->run(); }) {}

    ~Feeder() {
        this->stop_ = true;
        this->thread_.join();
    }

    Feeder(const Feeder&) = delete;
    Feeder& operator=(const Feeder&) = delete;

private:
    void run() {
        constexpr uint32_t BLOCK = 960;
        constexpr int64_t WAIT_US = 20000;
        const sendspin::server::test::Signal signal(16);
        Bytes block(size_t{BLOCK} * signal.frame_bytes());
        uint32_t position = 0;
        while (!this->stop_) {
            for (uint32_t i = 0; i < BLOCK; ++i) {
                signal.put(&block[i * signal.frame_bytes()], position + i);
            }
            uint32_t written = 0;
            while (written < BLOCK && !this->stop_) {
                written += static_cast<uint32_t>(
                    this->view_->write(&block[written * signal.frame_bytes()], BLOCK - written)
                        .frames);
                if (written < BLOCK) {
                    this->view_->wait_writable(WAIT_US);
                }
            }
            position += BLOCK;
        }
    }

    std::shared_ptr<sendspin::server::HostView> view_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::string player_hello(const std::string& name) {
    return sendspin::server::test::player_hello_json(
        name, R"([{"codec":"pcm","sample_rate":48000,"channels":2,"bit_depth":16}])", 400000);
}

/// What a raw client player reads: chunks by timestamp and duration, JSON messages by text
struct Reading {
    std::optional<int64_t> ts;
    int64_t duration_us{0};
    std::string json;
};

/// The next message the raw client reads, as a Reading; json empty and ts unset once closed
Reading read_one(RawClient& client) {
    Reading reading;
    const std::optional<Bytes> plaintext = client.receive_plaintext();
    if (!plaintext || plaintext->empty()) {
        return reading;
    }
    if (plaintext->front() == sendspin::server::MESSAGE_ID_JSON) {
        reading.json.assign(plaintext->begin() + 1, plaintext->end());
        return reading;
    }
    if (plaintext->front() == 4 && plaintext->size() > 13) {
        int64_t ts = 0;
        for (size_t i = 1; i < 9; ++i) {
            ts = (ts << 8) | (*plaintext)[i];
        }
        reading.ts = ts;
        const auto frames = static_cast<int64_t>((plaintext->size() - 13) / 4);
        reading.duration_us = frames * 1000000 / RATE;
    }
    return reading;
}

bool is_type(const std::string& json, const std::string& type) {
    return json.find(R"("type":")" + type + '"') != std::string::npos;
}

// While the operator has started a Pairing PSK pairing for a client, message 1 references its
// pairing PSK as 'pr', and a client holding it keys the session with it, which unpaired access and
// approval let play (messaging.md "server/activate"). Control: another client is referenced under
// the Sentinel.
TEST(ServerCoreTrust, ReferencesAPairingPskTheOperatorStarted) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    Core core;
    const Psk pairing = psk_of(0x70);
    RawClient paired;
    RawClient other;
    const std::string id = paired.noise.identity().peer_id();
    core.on_core(
        [&] { core.core->pairing_psks_.emplace(id, HandshakePsk{PskCategory::PAIRING, pairing}); });
    core.dial(paired.url());
    core.dial(other.url());
    paired.noise.psk = pairing;
    ASSERT_NO_FATAL_FAILURE(paired.activate(player_hello("Paired")));
    ASSERT_NO_FATAL_FAILURE(other.activate(player_hello("Other")));
    JsonDocument payload;
    ASSERT_EQ(deserializeJson(payload, paired.noise.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_category"], "pr");
    EXPECT_EQ(payload["psk_id"], sendspin::server::psk_id_for(pairing));
    ASSERT_EQ(deserializeJson(payload, other.noise.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_category"], "sn");
}

// The same with sendspin-cpp, which keeps its pairing PSK among its candidates at all times
// (pairing.md "Pairing PSK Flow"): matched, the session plays as an approved unpaired one. A
// client holding another pairing PSK answers under the Sentinel, and that session is held with no
// roles (a wrong pairing token).
TEST(ServerCoreTrust, KeysASendspinCppSessionWithItsPairingPsk) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Row {
        const char* name;
        Psk client_psk;
        bool roles;
    };
    const Psk pairing = psk_of(0x90);
    const Row rows[] = {
        {"Control: the client holds the pairing PSK", pairing, true},
        {"the client holds another", psk_of(0x91), false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Core core;
        ClientStore store;
        Peer peer(store, {true, row.client_psk});
        const std::string id = peer.client().client_id();
        core.on_core([&] {
            core.core->pairing_psks_.emplace(id, HandshakePsk{PskCategory::PAIRING, pairing});
        });
        core.dial(peer.url());
        // A client held with no roles is connected before its activation is written
        peer.run_until([&] {
            return core.recorder.contains("connected " + id) &&
                   core.recorder.contains("out server/activate");
        });
        const std::vector<std::string> activations = core.capture.activations();
        ASSERT_FALSE(activations.empty());
        EXPECT_EQ(activations.front().find("player@v1") != std::string::npos, row.roles)
            << activations.front();
        EXPECT_EQ(peer.trust().trust, sendspin::ConnectionTrust::NONE);
    }
}

/// A provider whose saves wait for release(), and fail when told to
class HeldProvider : public MemoryProvider {
public:
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [this] { return this->released_ > 0; });
        --this->released_;
        lock.unlock();
        return MemoryProvider::save_blob(key, data, len);
    }

    /// Lets count saves through
    void release(int count) {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ += count;
        }
        this->changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int released_{0};
};

// pairing.md "server/pair-finalize": a long-term PSK is used only once its record is durable.
// While the save is in flight handshakes reference the record as it was; a durable save makes the
// new PSK the one referenced and discards the approval (pairing.md "Unpaired Access"); a failed
// save or commit puts the record back, saves it again and reports the failure.
TEST(ServerCoreTrust, UsesALongTermPskOnlyOnceItIsDurable) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Row {
        const char* name;
        bool refuse_save;
        bool refuse_commit;
        bool durable;
    };
    const Row rows[] = {
        {"Control: saved and committed", false, false, true},
        {"the save refused", true, false, false},
        {"the commit refused", false, true, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::string id = Identity::generate()->peer_id();
        sendspin::server::ClientRecords records;
        records.add_loaded(0, ClientRecord{id, sendspin::server::ApprovalScope::PLAYBACK});
        HeldProvider provider;
        provider.refuse_save = row.refuse_save;
        provider.refuse_commit = row.refuse_commit;
        Core core(std::move(records), &provider);
        const Psk long_term = psk_of(0xA0);
        std::promise<bool> done;
        const PskCategory in_flight = core.on_core([&] {
            core.core->store_long_term_psk(
                id, long_term, [&](bool saved, const auto& /*unsaved*/) { done.set_value(saved); });
            return core.core->handshake_psk(id).category;
        });
        EXPECT_EQ(in_flight, PskCategory::SENTINEL);
        // The pairing's save, and for a failure the save that puts the record back
        provider.release(2);
        EXPECT_EQ(done.get_future().get(), row.durable);
        const HandshakePsk referenced = core.on_core([&] { return core.core->handshake_psk(id); });
        const ClientRecord record = core.on_core([&] { return *core.core->records_.find(id); });
        if (row.durable) {
            EXPECT_EQ(referenced.category, PskCategory::LONG_TERM);
            EXPECT_EQ(referenced.psk, long_term);
            EXPECT_EQ(record.long_term_psk, long_term);
            EXPECT_FALSE(record.approval);
            const std::optional<ClientRecord> stored = stored_record(provider, 0);
            ASSERT_TRUE(stored);
            EXPECT_EQ(stored->long_term_psk, long_term);
            EXPECT_FALSE(stored->approval);
        } else {
            EXPECT_EQ(referenced.category, PskCategory::SENTINEL);
            EXPECT_FALSE(record.long_term_psk);
            EXPECT_EQ(record.approval, sendspin::server::ApprovalScope::PLAYBACK);
            // Both saves fail, each reported
            core.recorder.wait_for_count("error " +
                                             std::to_string(static_cast<int>(
                                                 sendspin::server::ErrorKind::PERSISTENCE_FAILED)) +
                                             " ",
                                         2);
            EXPECT_EQ(provider.saves, 2);
        }
    }
}

// One long-term PSK change per client is in flight: a second store meanwhile is refused at once
// and never applies. A failed save puts back only what the change set, so a name a connection
// refreshed and an approval the operator set meanwhile stay (pairing.md "server/pair-finalize").
// Control: a durable save keeps the first PSK with those changes.
TEST(ServerCoreTrust, HoldsOneLongTermPskChangePerClient) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    struct Row {
        const char* name;
        bool refuse_save;
    };
    const Row rows[] = {
        {"Control: saved", false},
        {"the save refused", true},
    };
    const Psk first = psk_of(0xA0);
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        RawClient client;
        const std::string id = client.noise.identity().peer_id();
        sendspin::server::ClientRecords records;
        ClientRecord loaded{id, std::nullopt};
        loaded.name = "Old";
        records.add_loaded(0, loaded);
        HeldProvider provider;
        provider.refuse_save = row.refuse_save;
        Core core(std::move(records), &provider);
        std::promise<bool> done;
        std::optional<bool> second;
        core.on_core([&] {
            core.core->store_long_term_psk(
                id, first, [&](bool saved, const auto& /*unsaved*/) { done.set_value(saved); });
            core.core->store_long_term_psk(
                id, psk_of(0xB0), [&](bool saved, const auto& /*unsaved*/) { second = saved; });
        });
        EXPECT_EQ(second, std::optional(false));
        // Meanwhile the client connects under a new name and the operator approves it
        core.dial(client.url());
        ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("New")));
        core.on_core(
            [&] { core.core->set_approval(id, sendspin::server::ApprovalScope::PLAYBACK); });
        // The pairing's, the name's, the approval's, and for a failure the restoring save
        provider.release(4);
        EXPECT_EQ(done.get_future().get(), !row.refuse_save);
        const ClientRecord record = core.on_core([&] { return *core.core->records_.find(id); });
        if (row.refuse_save) {
            EXPECT_FALSE(record.long_term_psk);
        } else {
            EXPECT_EQ(record.long_term_psk, first);
        }
        EXPECT_EQ(record.name, "New");
        EXPECT_EQ(record.approval, sendspin::server::ApprovalScope::PLAYBACK);
    }
}

// A save that finishes after the core thread destroyed the core reaches only the loop, whose
// handler never runs: done is dropped and nothing touches the core (ASan reports a use after free
// otherwise). The PSK was never finalized, so the stopping core puts the change back on disk
// behind that save: the record as it was, or no record (an all-zero blob) when none existed.
TEST(ServerCoreTrust, FinishesASaveTheCoreDidNotOutlive) {
    for (const bool existed : {true, false}) {
        SCOPED_TRACE(existed ? "a record existed" : "no record before");
        const std::string id = Identity::generate()->peer_id();
        sendspin::server::ClientRecords records;
        if (existed) {
            records.add_loaded(0, ClientRecord{id, sendspin::server::ApprovalScope::PLAYBACK});
        }
        HeldProvider provider;
        std::atomic<bool> called{false};
        {
            Core core(std::move(records), &provider);
            core.on_core([&] {
                core.core->store_long_term_psk(
                    id, psk_of(0xD0), [&](bool, const auto& /*unsaved*/) { called = true; });
            });
            core.stop();
            ASSERT_FALSE(core.core);
            // The save, then the put-back
            provider.release(2);
        }
        EXPECT_FALSE(called);
        const auto blob = provider.blobs.find(persistence_keys::client_record_key(0));
        ASSERT_NE(blob, provider.blobs.end());
        if (existed) {
            const std::optional<ClientRecord> stored = stored_record(provider, 0);
            ASSERT_TRUE(stored);
            EXPECT_FALSE(stored->long_term_psk);
            EXPECT_EQ(stored->approval, sendspin::server::ApprovalScope::PLAYBACK);
        } else {
            EXPECT_TRUE(std::all_of(blob->second.begin(), blob->second.end(),
                                    [](uint8_t byte) { return byte == 0; }));
        }
    }
}

// An attempt whose client stops answering mid-round (here after server/pair-auth) ends as
// TIMED_OUT after ServerCore's client-step timeout (PAIRING_CLIENT_STEP_TIMEOUT_US, shortened
// here), with an activation without 'pairing'
TEST(ServerCoreTrust, EndsAnAttemptWhoseClientStopsAnswering) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    Core core;
    core.settings.pairing_client_step_timeout_us = 200000;
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    core.dial(client.url());
    ASSERT_NO_FATAL_FAILURE(client.activate(
        R"({"type":"client/hello","payload":{"name":"Kitchen","supported_roles":["metadata@v1"],)"
        R"("supported_pair_methods":{"dynamic_pairing_code":{"out_channels":["display"],)"
        R"("formats":["digits"]}},"unpaired_access":{"enabled":true}}})"));
    core.on_core([&] {
        core.core->begin_pairing(
            id, sendspin::server::PairingOptions{sendspin::server::PairMethod::DYNAMIC_PAIRING_CODE,
                                                 sendspin::server::CodeFormat::DIGITS});
    });
    const std::string pairing = client.receive_until("server/activate");
    ASSERT_NE(pairing.find("pairing"), std::string::npos) << pairing;
    client.send_json(R"({"type":"client/pair-init","payload":{"pairing_index":1,"commit_B":")" +
                     std::string(43, 'A') + R"("}})");
    ASSERT_FALSE(client.receive_until("server/pair-init").empty());
    core.on_core([&] { core.core->submit_pairing_code(id, "123456"); });
    ASSERT_FALSE(client.receive_until("server/pair-auth").empty());
    core.recorder.wait_for(
        Recorder::pairing_result(id, sendspin::server::PairingResult::TIMED_OUT));
    const std::string ending = client.receive_until("server/activate");
    EXPECT_FALSE(ending.empty());
    EXPECT_EQ(ending.find("pairing"), std::string::npos) << ending;
}

// messaging.md "server/activate": once a client is paired, its unpaired session plays no more
// without approval. An approved player under the Sentinel is held with no roles as soon as its
// long-term PSK is durable, and moved out of the server's group.
TEST(ServerCoreTrust, HoldsTheUnpairedSessionOfAClientOncePaired) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    Core core;
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    core.dial(client.url());
    ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("Player")));
    // A client that may play is in the server's group
    const auto playable = [&] {
        for (const sendspin::server::GroupInfo& group : core.groups.read()) {
            if (group.id == core.core->default_group()) {
                return std::find(group.client_ids.begin(), group.client_ids.end(), id) !=
                       group.client_ids.end();
            }
        }
        return false;
    };
    ASSERT_TRUE(core.on_core(playable));
    std::optional<bool> stored;
    ASSERT_FALSE(core.on_core([&] {
        core.core->store_long_term_psk(
            id, psk_of(0xE0), [&](bool saved, const auto& /*unsaved*/) { stored = saved; });
        return playable();
    }));
    EXPECT_EQ(stored, std::optional(true));
    EXPECT_EQ(client.receive_until("server/activate"),
              R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");
}

// connection.md "Sentinel Fallback": a client that fell back from its long-term PSK reports the
// mismatch until a re-handshake completes, not once it is queued
TEST(ServerCoreTrust, ClearsTheMismatchOnceARekeyCompletes) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    sendspin::server::ClientRecords records;
    ClientRecord record{id, std::nullopt};
    record.long_term_psk = psk_of(0xF0);
    records.add_loaded(0, record);
    Core core(std::move(records));
    core.dial(client.url());
    ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("Lost")));
    const auto mismatch = [&] {
        for (const ClientInfo& info : core.snapshot.read()) {
            if (info.client_id == id) {
                return info.credential_mismatch;
            }
        }
        return false;
    };
    ASSERT_TRUE(core.on_core(mismatch));
    EXPECT_TRUE(core.on_core([&] {
        core.core->rekey(id, HandshakePsk{});
        return mismatch();
    }));
    const std::string message_1 = client.receive_until("noise/handshake");
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1));
    client.send_binary(client.noise.rekey_message_2());
    EXPECT_FALSE(client.receive_until("server/activate").empty());
    EXPECT_FALSE(core.on_core(mismatch));
}

// connection.md "Re-handshake", end to end during playback: the raw client reads message 1 between
// chunks, sends a time request under the old keys before message 2, and gets the frozen
// server/activate first under the new keys (no active_roles, which persist), then the time reply,
// then chunks that continue the timeline where they left off, with no stream message in between
TEST(ServerCoreTrust, RekeysDuringPlaybackAndPlaysOn) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    Core core;
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    core.dial(client.url());
    ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("Player")));
    client.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(150, 300, 0)));
    const Feeder feeder(core.open_input(RATE));
    ASSERT_FALSE(client.receive_until("stream/start").empty());
    int64_t next_ts = 0;
    for (int chunks = 0; chunks < 3;) {
        const Reading reading = read_one(client);
        ASSERT_TRUE(reading.ts || !reading.json.empty());
        if (reading.ts) {
            EXPECT_TRUE(chunks == 0 || *reading.ts == next_ts) << *reading.ts << " " << next_ts;
            next_ts = *reading.ts + reading.duration_us;
            ++chunks;
        }
    }

    core.on_core([&] { core.core->rekey(id, HandshakePsk{}); });
    std::string message_1;
    while (message_1.empty()) {
        const Reading reading = read_one(client);
        ASSERT_TRUE(reading.ts || !reading.json.empty());
        if (reading.ts) {
            EXPECT_EQ(*reading.ts, next_ts);
            next_ts = *reading.ts + reading.duration_us;
        } else if (is_type(reading.json, "noise/handshake")) {
            message_1 = reading.json;
        } else {
            EXPECT_FALSE(is_type(reading.json, "stream/end")) << reading.json;
        }
    }
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1));
    client.send_json(sendspin::server::test::client_time_json(77));
    client.send_binary(client.noise.rekey_message_2());

    EXPECT_EQ(client.receive_json(),
              R"({"type":"server/activate","payload":{"activities":["playback"]}})");
    JsonDocument time;
    ASSERT_EQ(deserializeJson(time, client.receive_json()), DeserializationError::Ok);
    EXPECT_EQ(time["type"], "server/time");
    EXPECT_EQ(time["payload"]["client_transmitted"], 77);
    EXPECT_LE(time["payload"]["server_received"].as<int64_t>(),
              time["payload"]["server_transmitted"].as<int64_t>());
    for (int chunks = 0; chunks < 3;) {
        const Reading reading = read_one(client);
        ASSERT_TRUE(reading.ts || !reading.json.empty());
        if (reading.ts) {
            EXPECT_EQ(*reading.ts, next_ts);
            next_ts = *reading.ts + reading.duration_us;
            ++chunks;
        } else {
            EXPECT_FALSE(is_type(reading.json, "stream/start") ||
                         is_type(reading.json, "stream/clear") ||
                         is_type(reading.json, "stream/end"))
                << reading.json;
        }
    }
    JsonDocument payload;
    ASSERT_EQ(deserializeJson(payload, client.noise.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_category"], "sn");
}

// messaging.md "server/activate": when the activation after a re-handshake removes a stream role,
// its stream/end goes before message 1. Here a paired client playing under its long-term PSK is
// re-handshaken to the Sentinel, as before a code pairing: the session becomes unpaired while the
// record exists, so the frozen activation removes every role and 'playback' (connection.md
// "Sentinel Fallback", "Re-handshake": the activation rules apply under the newly matched PSK), and
// the client is moved alone into a stopped group.
TEST(ServerCoreTrust, EndsTheStreamsARekeyRemovesBeforeMessage1) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    const Psk long_term = psk_of(0xC0);
    RawClient client;
    const std::string id = client.noise.identity().peer_id();
    sendspin::server::ClientRecords records;
    ClientRecord record{id, std::nullopt};
    record.long_term_psk = long_term;
    records.add_loaded(0, record);
    Core core(std::move(records));
    core.dial(client.url());
    client.noise.psk = long_term;
    ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("Paired")));
    JsonDocument payload;
    ASSERT_EQ(deserializeJson(payload, client.noise.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_category"], "lt");
    client.send_json(sendspin::server::test::client_state_json(
        true, sendspin::server::test::player_state_json(150, 300, 0)));
    const Feeder feeder(core.open_input(RATE));
    ASSERT_FALSE(client.receive_until("stream/start").empty());

    core.on_core([&] { core.core->rekey(id, HandshakePsk{}); });
    bool ended = false;
    std::string message_1;
    while (message_1.empty()) {
        const Reading reading = read_one(client);
        ASSERT_TRUE(reading.ts || !reading.json.empty());
        if (reading.ts) {
            EXPECT_FALSE(ended) << "a chunk after stream/end";
        } else if (is_type(reading.json, "stream/end")) {
            ended = true;
        } else if (is_type(reading.json, "noise/handshake")) {
            message_1 = reading.json;
        }
    }
    EXPECT_TRUE(ended);
    client.noise.psk = sendspin::server::SENTINEL_PSK;
    ASSERT_TRUE(client.noise.read_rekey_message_1(message_1));
    ASSERT_EQ(deserializeJson(payload, client.noise.message_1_payload), DeserializationError::Ok);
    EXPECT_EQ(payload["psk_category"], "sn");
    client.send_binary(client.noise.rekey_message_2());
    EXPECT_EQ(client.receive_json(),
              R"({"type":"server/activate","payload":{"activities":[],"active_roles":[]}})");
    const std::string update = client.receive_until("group/update");
    EXPECT_NE(update.find(R"("playback_state":"stopped")"), std::string::npos) << update;
}

// The step after a pairing (pairing.md "Pairing"), with sendspin-cpp: an approved player streaming
// under the Sentinel gets a long-term PSK it already holds, the record is saved, and the server
// re-handshakes to it. The player keeps playing the signal in order and stays time-synced, and its
// record, saved, holds the PSK and no approval. The player plays in real time, so the test takes
// the 400 ms of audio it waits for on either side of the switch, and more.
TEST(ServerCoreTrust, PromotesAPlayingSendspinCppClientToItsLongTermPsk) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    MemoryProvider provider;
    Core core({}, &provider);
    const Psk long_term = psk_of(0xE0);
    ClientStore store;
    store.hold_record(core.identity.peer_id(), long_term);
    Peer peer(store, {true, std::nullopt});
    const std::string id = peer.client().client_id();
    core.dial(peer.url());
    peer.run_until([&] { return core.recorder.contains("connected " + id); });
    EXPECT_EQ(peer.trust().trust, sendspin::ConnectionTrust::NONE);
    const Feeder feeder(core.open_input(RATE));
    peer.run_until([&] { return peer.sink().audible() >= RATE / 5; });

    const size_t replies = core.recorder.count("out server/time");
    std::atomic<bool> stored{false};
    core.on_core([&] {
        core.core->store_long_term_psk(id, long_term, [&](bool saved, const auto& /*unsaved*/) {
            EXPECT_TRUE(saved);
            stored = true;
            core.core->rekey(id, HandshakePsk{PskCategory::LONG_TERM, long_term});
        });
    });
    peer.run_until([&] { return core.recorder.count("out server/activate") >= 2; });
    const size_t audible = peer.sink().audible();
    peer.run_until([&] {
        return peer.sink().audible() >= audible + RATE / 5 &&
               core.recorder.count("out server/time") >= replies + 2;
    });
    EXPECT_TRUE(stored);
    EXPECT_TRUE(peer.client().is_connected());
    EXPECT_TRUE(peer.client().is_time_synced());
    EXPECT_EQ(peer.trust().trust, sendspin::ConnectionTrust::USER);
    EXPECT_EQ(core.recorder.count("out stream/start"), 1U);
    EXPECT_EQ(core.recorder.count("out stream/end"), 0U);

    // The signal plays in order across the switch: no frame repeated or out of order
    const sendspin::server::test::Signal signal(16);
    const Bytes played = peer.sink().played();
    std::optional<uint32_t> previous;
    size_t positions = 0;
    for (size_t offset = 0; offset + 4 <= played.size(); offset += 4) {
        const std::optional<uint32_t> position = signal.get(&played[offset]);
        if (!position) {
            continue;
        }
        if (previous) {
            EXPECT_GT(*position, *previous);
        }
        previous = position;
        ++positions;
    }
    EXPECT_GE(positions, size_t{RATE} / 5);

    // Saved on the dispatch thread before the activation it led to was traced there
    const std::optional<ClientRecord> saved = stored_record(provider, 0);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->long_term_psk, long_term);
    EXPECT_FALSE(saved->approval);
}

}  // namespace
