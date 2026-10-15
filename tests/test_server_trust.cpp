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
/// through ServerCore on its own core thread a long-term PSK holds the client's unpaired sessions,
/// a save may outlive the core, a failed removal or put-back is kept and retried, an unconfirmed
/// long-term PSK stays off disk and an attempt whose client stops answering ends.

#include "client_records.h"
#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "dispatcher.h"
#include "peer.h"
#include "raw_client.h"
#include "server_core.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "transport.h"
#include "trust_fixtures.h"
#include "trust_harness.h"
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::ClientRecord;
using sendspin::server::Identity;
using sendspin::server::Psk;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TracedMessage;
using sendspin::server::test::Bytes;
using sendspin::server::test::Capture;
using sendspin::server::test::ClientStore;
using sendspin::server::test::Core;
using sendspin::server::test::HeldProvider;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::Peer;
using sendspin::server::test::player_hello;
using sendspin::server::test::psk_of;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::ScriptedProvider;
using sendspin::server::test::stored_record;
namespace persistence_keys = sendspin::server::persistence_keys;

Bytes record_blob(const ClientRecord& record) {
    sendspin::server::ClientRecordBlob blob{};
    EXPECT_TRUE(sendspin::server::encode_client_record(record, blob));
    return {blob.begin(), blob.end()};
}

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

    StartResult start(const ServerConfig& config) {
        return this->server.start(config, &this->provider, &this->recorder);
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

    ASSERT_EQ(known.start(known.config()), StartResult::OK);
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

    ASSERT_EQ(known.start(known.config()), StartResult::OK);
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
    ASSERT_EQ(known.start(known.config()), StartResult::OK);
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

/// A provider whose saves wait for release(): the first lands but its commit is refused, and the
/// second is refused
class PutBackProvider : public MemoryProvider {
public:
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        std::unique_lock<std::mutex> lock(this->mutex_);
        this->changed_.wait(lock, [this] { return this->released_; });
        lock.unlock();
        if (this->saves == 1) {
            ++this->saves;
            return false;
        }
        return MemoryProvider::save_blob(key, data, len);
    }

    bool commit() override {
        ++this->commits;
        return this->commits != 1;
    }

    void release() {
        {
            const std::lock_guard<std::mutex> lock(this->mutex_);
            this->released_ = true;
        }
        this->changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool released_{false};
};

/// The records a start loads from provider
sendspin::server::ClientRecords load_records(MemoryProvider& provider) {
    sendspin::server::Dispatcher dispatcher;
    const sendspin::server::Logger log(
        dispatcher, [](sendspin::server::LogLevel, std::string_view) {},
        sendspin::server::LogLevel::ERROR);
    return sendspin::server::load_client_records(provider, log);
}

// An unpairing or a revocation whose write the provider fails is not undone by a restart: the
// write goes again before the next record write and at shutdown until it lands, and a later write
// to its slot replaces it. A long-term PSK whose save failed is put back, never written again
// (pairing.md "server/pair-finalize").
TEST(ServerCoreTrust, KeepsARemovalWhoseWriteFailed) {
    using sendspin::server::ApprovalScope;
    using sendspin::server::ServerCore;
    const std::string a = Identity::generate()->peer_id();
    const std::string b = Identity::generate()->peer_id();
    const std::string c = Identity::generate()->peer_id();
    ClientRecord paired_a{a, std::nullopt};
    paired_a.long_term_psk = psk_of(0x10);
    ClientRecord paired_approved_a{a, ApprovalScope::PLAYBACK};
    paired_approved_a.long_term_psk = psk_of(0x11);
    ClientRecord paired_b{b, std::nullopt};
    paired_b.long_term_psk = psk_of(0x20);
    const ClientRecord approved_a{a, ApprovalScope::PLAYBACK};
    struct Row {
        const char* name;
        /// Saved in slots 0, 1 and so on before the start
        std::vector<ClientRecord> stored;
        std::function<void(ServerCore&)> change;
        /// The outcomes of the first saves
        std::vector<bool> accepted;
        /// What the next start loads
        std::vector<ClientRecord> restored;
    };
    const Row rows[] = {
        {"Control: an unpairing saved",
         {paired_a},
         [&](ServerCore& core) { core.unpair(a); },
         {},
         {}},
        {"an unpairing's erase refused, written again at shutdown",
         {paired_a},
         [&](ServerCore& core) { core.unpair(a); },
         {false},
         {}},
        {"an unpairing that keeps the approval, its save refused",
         {paired_approved_a},
         [&](ServerCore& core) { core.unpair(a); },
         {false},
         {approved_a}},
        {"a revocation's save refused",
         {approved_a},
         [&](ServerCore& core) { core.set_approval(a, std::nullopt); },
         {false},
         {ClientRecord{a, std::nullopt}}},
        // a's erase lands before b's, and b's fails at shutdown too
        {"an erase refused, written again before the next write",
         {paired_a, paired_b},
         [&](ServerCore& core) {
             core.unpair(a);
             core.unpair(b);
         },
         {false, true, false, false},
         {paired_b}},
        // c takes the slot a's erase freed, and its save lands only at shutdown
        {"a refused erase replaced by a later write to its slot",
         {paired_a},
         [&](ServerCore& core) {
             core.unpair(a);
             core.set_approval(c, ApprovalScope::PLAYBACK);
         },
         {false, false},
         {ClientRecord{c, ApprovalScope::PLAYBACK}}},
        // b's erase goes between the pairing's failed save and the save that puts a back
        {"a long-term PSK whose save was refused",
         {approved_a, paired_b},
         [&](ServerCore& core) {
             core.store_long_term_psk(a, psk_of(0x30), [](bool, const auto& /*unsaved*/) {});
             core.unpair(b);
         },
         {false, true, true, false, false},
         {approved_a}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        ScriptedProvider provider(row.accepted);
        for (size_t slot = 0; slot < row.stored.size(); ++slot) {
            sendspin::server::ClientRecordBlob blob{};
            ASSERT_TRUE(sendspin::server::encode_client_record(row.stored[slot], blob));
            provider.blobs[persistence_keys::client_record_key(slot)] =
                Bytes(blob.begin(), blob.end());
        }
        {
            Core core(load_records(provider), &provider);
            core.on_core([&] { row.change(*core.core); });
        }
        const sendspin::server::ClientRecords restored = load_records(provider);
        EXPECT_EQ(restored.size(), row.restored.size());
        for (const ClientRecord& expected : row.restored) {
            const ClientRecord* loaded = restored.find(expected.client_id);
            ASSERT_NE(loaded, nullptr);
            EXPECT_EQ(*loaded, expected);
        }
    }
}

// A long-term PSK whose save the core did not outlive is put back at shutdown, and the shutdown's
// retry goes behind that put-back. Here the PSK's save lands uncommitted and the put-back's save
// fails, so only the retry leaves the next start the record as it was (pairing.md
// "server/pair-finalize").
TEST(ServerCoreTrust, RetriesAPutBackMadeAtShutdown) {
    const std::string id = Identity::generate()->peer_id();
    const ClientRecord approved{id, sendspin::server::ApprovalScope::PLAYBACK};
    sendspin::server::ClientRecords records;
    records.add_loaded(0, approved);
    PutBackProvider provider;
    {
        Core core(std::move(records), &provider);
        core.on_core([&] {
            core.core->store_long_term_psk(id, psk_of(0xE0), [](bool, const auto& /*unsaved*/) {});
        });
        core.stop();
        provider.release();
    }
    const sendspin::server::ClientRecords restored = load_records(provider);
    const ClientRecord* loaded = restored.find(id);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(*loaded, approved);
}

// A record saved while a long-term PSK's save is in flight leaves that PSK out, so a refused PSK
// save never reaches disk through a save queued behind it (pairing.md "server/pair-finalize"),
// even when the put-back's write is refused too. Meanwhile the client connects under a new name,
// which saves its record, the operator approves it, or both. The change is durable only once a
// save of it lands with none made meanwhile, so a refused save of it again puts it back. Control:
// a PSK save that lands is saved again behind them, and durable.
TEST(ServerCoreTrust, KeepsAnUnconfirmedLongTermPskOffDisk) {
    ASSERT_EQ(noise_init_framework(), NOISE_ERROR_NONE);
    using sendspin::server::ApprovalScope;
    using sendspin::server::ServerCore;
    // Each row's connection is the same client
    const Identity identity = *Identity::generate();
    const std::string a = identity.peer_id();
    const std::string b = Identity::generate()->peer_id();
    const Psk psk = psk_of(0xF0);
    ClientRecord named_a{a, std::nullopt};
    named_a.name = "Old";
    ClientRecord paired_b{b, std::nullopt};
    paired_b.long_term_psk = psk_of(0xF1);
    const auto renamed = [&](std::optional<ApprovalScope> approval, std::optional<Psk> long_term) {
        ClientRecord record{a, approval};
        record.name = "New";
        record.long_term_psk = long_term;
        return record;
    };
    ClientRecord approved_a = named_a;
    approved_a.approval = ApprovalScope::PLAYBACK;
    struct Row {
        const char* name;
        /// Saved in slots 0, 1 and so on before the start
        std::vector<ClientRecord> stored;
        /// The client connects under the name "New" while the PSK is being saved
        bool connect;
        std::function<void(ServerCore&)> meanwhile;
        /// The outcomes of the first saves
        std::vector<bool> accepted;
        /// What the next start loads
        std::vector<ClientRecord> restored;
        /// The change's outcome
        bool durable;
    };
    const auto approve = [&](ServerCore& core) { core.set_approval(a, ApprovalScope::PLAYBACK); };
    const Row rows[] = {
        {"Control: the PSK saved, a name and an approval behind it",
         {named_a},
         true,
         approve,
         {},
         {renamed(ApprovalScope::PLAYBACK, psk)},
         true},
        {"Control: the PSK saved for a new record, a name behind it",
         {},
         true,
         nullptr,
         {},
         {renamed(std::nullopt, psk)},
         true},
        // The PSK's, the name's, the approval's, the put-back's and its retry at shutdown
        {"the PSK's save refused, a name's and an approval's saved, the put-back's refused",
         {named_a},
         true,
         approve,
         {false, true, true, false, false},
         {renamed(ApprovalScope::PLAYBACK, std::nullopt)},
         false},
        // Nothing is saved meanwhile for a record the change created, so the erase that puts it
        // back is the second save; a name saved second would leave the erase and its retry refused
        {"the PSK's save for a new record refused, a name meanwhile",
         {},
         true,
         nullptr,
         {false, true, false, false},
         {},
         false},
        // The PSK's, the approval's, the PSK's again, refused, and the put-back's
        {"the PSK's saved, an approval's behind it, the PSK's again refused",
         {named_a},
         false,
         approve,
         {true, true, false},
         {approved_a},
         false},
        // The approval's save, refused, goes again ahead of b's erase
        {"the PSK's and an approval's saves refused, the approval's written again",
         {named_a, paired_b},
         false,
         [&](ServerCore& core) {
             approve(core);
             core.unpair(b);
         },
         {false, false, true, true, false, false},
         {approved_a},
         false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        HeldProvider provider(row.accepted);
        for (size_t slot = 0; slot < row.stored.size(); ++slot) {
            provider.blobs[persistence_keys::client_record_key(slot)] =
                record_blob(row.stored[slot]);
        }
        {
            Core core(load_records(provider), &provider);
            std::promise<bool> done;
            core.on_core([&] {
                core.core->store_long_term_psk(
                    a, psk, [&](bool saved, const auto& /*unsaved*/) { done.set_value(saved); });
            });
            RawClient client(sendspin::server::CipherSuite::CHACHAPOLY_SHA256,
                             *Identity::from_private_bytes(identity.private_bytes));
            if (row.connect) {
                core.dial(client.url());
                ASSERT_NO_FATAL_FAILURE(client.activate(player_hello("New")));
            }
            if (row.meanwhile) {
                core.on_core([&] { row.meanwhile(*core.core); });
            }
            constexpr int EVERY_SAVE = 8;
            provider.release(EVERY_SAVE);
            // The outcome before the core stops
            EXPECT_EQ(done.get_future().get(), row.durable);
        }
        const sendspin::server::ClientRecords restored = load_records(provider);
        EXPECT_EQ(restored.size(), row.restored.size());
        for (const ClientRecord& expected : row.restored) {
            const ClientRecord* loaded = restored.find(expected.client_id);
            ASSERT_NE(loaded, nullptr);
            EXPECT_EQ(*loaded, expected);
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

}  // namespace
