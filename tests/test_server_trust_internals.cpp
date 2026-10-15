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

/// @file test_server_trust_internals.cpp
/// @brief Trust through ServerCore on its own core thread, reached inside: a pairing PSK the
/// operator started is referenced, a long-term PSK is used only once it is durable, one change at a
/// time, and re-handshakes switch keys under playback and end a mismatch. Compiled with
/// -fno-access-control (tests/CMakeLists.txt).

#include "client_records.h"
#include "crypto/base64url.h"
#include "crypto/keys.h"
#include "host_view.h"
#include "peer.h"
#include "raw_client.h"
#include "server_core.h"
#include "test_ports.h"
#include "test_recorder.h"
#include "transport.h"
#include "trust_fixtures.h"
#include "trust_harness.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>
#include <noise/protocol.h>

#include <sendspin/client.h>
#include <sendspin/metadata_role.h>
#include <sendspin/server/server.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ClientInfo;
using sendspin::server::ClientRecord;
using sendspin::server::HandshakePsk;
using sendspin::server::Identity;
using sendspin::server::Psk;
using sendspin::server::PskCategory;
using sendspin::server::test::Bytes;
using sendspin::server::test::ClientStore;
using sendspin::server::test::Core;
using sendspin::server::test::Feeder;
using sendspin::server::test::HeldProvider;
using sendspin::server::test::is_type;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::Peer;
using sendspin::server::test::player_hello;
using sendspin::server::test::PLAYER_RATE;
using sendspin::server::test::psk_of;
using sendspin::server::test::RawClient;
using sendspin::server::test::read_one;
using sendspin::server::test::Reading;
using sendspin::server::test::stored_record;

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
        // The pairing's save, and for a failure the save that puts the record back and its retry
        // at shutdown
        provider.release(row.durable ? 2 : 3);
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
        // The pairing's, the name's, the approval's, then the pairing's again for a durable save,
        // or the restoring save and its retry at shutdown for a failure
        provider.release(row.refuse_save ? 5 : 4);
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
    const Feeder feeder(core.open_input(PLAYER_RATE));
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
    const Feeder feeder(core.open_input(PLAYER_RATE));
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
    const Feeder feeder(core.open_input(PLAYER_RATE));
    peer.run_until([&] { return peer.sink().audible() >= PLAYER_RATE / 5; });

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
        return peer.sink().audible() >= audible + PLAYER_RATE / 5 &&
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
    EXPECT_GE(positions, size_t{PLAYER_RATE} / 5);

    // Saved on the dispatch thread before the activation it led to was traced there
    const std::optional<ClientRecord> saved = stored_record(provider, 0);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->long_term_psk, long_term);
    EXPECT_FALSE(saved->approval);
}

}  // namespace
