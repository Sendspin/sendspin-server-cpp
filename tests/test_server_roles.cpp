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

/// @file test_server_roles.cpp
/// @brief Approval, the controller and metadata roles, availability and disconnects end to end:
/// the raw client checks the exact messages, and in-process sendspin-cpp clients show what a
/// client library makes of them.

#include "raw_client.h"
#include "test_ports.h"
#include "test_recorder.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/client.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>
#include <sendspin/player_role.h>
#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ApprovalScope;
using sendspin::server::ClientDisconnect;
using sendspin::server::ClientInfo;
using sendspin::server::CommandArgs;
using sendspin::server::ControllerCaps;
using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::GoodbyeReason;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::Metadata;
using sendspin::server::MetadataProgress;
using sendspin::server::RepeatMode;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TransportCommand;
using sendspin::server::test::MemoryProvider;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
namespace persistence_keys = sendspin::server::persistence_keys;

// ==============================================================================
// Raw client messages
// ==============================================================================

/// A client/hello listing roles, with the player's support object when it lists player@v1
std::string hello(const std::string& name, const std::vector<std::string>& roles, bool unpaired) {
    JsonDocument doc;
    doc["type"] = "client/hello";
    JsonObject payload = doc["payload"].to<JsonObject>();
    payload["name"] = name;
    JsonArray listed = payload["supported_roles"].to<JsonArray>();
    for (const std::string& role : roles) {
        listed.add(role);
    }
    if (std::find(roles.begin(), roles.end(), "player@v1") != roles.end()) {
        JsonObject format = payload["player@v1_support"]["supported_formats"].add<JsonObject>();
        format["codec"] = "pcm";
        format["sample_rate"] = 48000;
        format["channels"] = 2;
        format["bit_depth"] = 16;
        payload["player@v1_support"]["buffer_capacity"] = 200000;
    }
    payload["supported_pair_methods"]["pairing_psk"].to<JsonObject>();
    payload["unpaired_access"]["enabled"] = unpaired;
    std::string out;
    serializeJson(doc, out);
    return out;
}

/// A client/state with a player object offering commands, reporting volume and muted when set
std::string player_state(bool available, const std::vector<std::string>& commands,
                         std::optional<uint32_t> volume, std::optional<bool> muted) {
    JsonDocument doc;
    doc["type"] = "client/state";
    doc["payload"]["available"] = available;
    JsonObject player = doc["payload"]["player"].to<JsonObject>();
    player["output_delay_ms"] = 0;
    player["required_lead_time_ms"] = 0;
    player["min_buffer_ms"] = 0;
    JsonArray offered = player["supported_commands"].to<JsonArray>();
    for (const std::string& command : commands) {
        offered.add(command);
    }
    if (volume) {
        player["volume"] = *volume;
    }
    if (muted) {
        player["muted"] = *muted;
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string controller_command(const std::string& object) {
    return R"({"type":"client/command","payload":{"controller":)" + object + "}}";
}

/// The next JSON message, parsed
JsonDocument next(RawClient& client) {
    JsonDocument doc;
    const std::string json = client.receive_json();
    EXPECT_FALSE(json.empty()) << "the connection closed";
    deserializeJson(doc, json);
    return doc;
}

/// The next JSON message of type, skipping others
JsonDocument next(RawClient& client, std::string_view type) {
    for (;;) {
        const std::string json = client.receive_json();
        if (json.empty()) {
            ADD_FAILURE() << "the connection closed while waiting for " << type;
            return JsonDocument();
        }
        JsonDocument doc;
        deserializeJson(doc, json);
        if (doc["type"].as<std::string_view>() == type) {
            return doc;
        }
    }
}

std::string payload(const JsonDocument& doc) {
    std::string out;
    serializeJson(doc["payload"], out);
    return out;
}

std::string type(const JsonDocument& doc) {
    return doc["type"].as<std::string>();
}

/// Dials client and runs the handshake through client/hello
void connect(SendspinServer& server, RawClient& client, const std::string& hello_json) {
    ASSERT_TRUE(server.connect_url(client.url()));
    ASSERT_NO_FATAL_FAILURE(client.handshake());
    client.send_json(hello_json);
}

std::string client_id_of(const RawClient& client) {
    return client.noise.identity().peer_id();
}

std::string group_id(const GroupId& id) {
    return std::to_string(id.value);
}

constexpr const char* ALL_ROLES = R"(["player@v1","controller@v1","metadata@v1"])";

// ==============================================================================
// Approval (pairing.md "Unpaired Access")
// ==============================================================================

// pairing.md "Unpaired Access": an unapproved client is held at no activities and presented to the
// operator; the operator's approval activates it with every implemented role, declaring `playback`
// only while its group plays, and persists across a restart, where a dial for playback declares it
// at once; revoking it activates it with none. The approval's re-activation is followed by the new
// roles' server/state (messaging.md "server/state") and, for the player, a move into the server's
// group.
TEST(ServerApproval, ApprovesAndRevokesAClientOverTheWire) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Roles";
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const std::string default_group = group_id(*server.default_group());
    RawClient client;
    const std::string id = client_id_of(client);
    const std::string hello_json =
        hello("Kitchen", {"player@v1", "controller@v1", "metadata@v1"}, true);
    ASSERT_NO_FATAL_FAILURE(connect(server, client, hello_json));

    ASSERT_EQ(payload(next(client)), R"({"activities":[],"active_roles":[]})");
    JsonDocument update = next(client);
    ASSERT_EQ(type(update), "group/update");
    EXPECT_NE(update["payload"]["group_id"].as<std::string>(), default_group);
    recorder.wait_for("approval needed " + id);
    ASSERT_EQ(server.clients().size(), 1U);
    EXPECT_FALSE(server.clients()[0].approval);
    EXPECT_TRUE(server.clients()[0].unpaired_access);

    ASSERT_TRUE(server.approve(id, ApprovalScope::PLAYBACK));
    EXPECT_EQ(payload(next(client)),
              std::string(R"({"activities":[],"active_roles":)") + ALL_ROLES + "}");
    update = next(client);
    ASSERT_EQ(type(update), "group/update");
    EXPECT_EQ(update["payload"]["group_id"].as<std::string>(), default_group);
    JsonDocument state = next(client);
    ASSERT_EQ(type(state), "server/state");
    std::string controller;
    serializeJson(state["payload"]["controller"], controller);
    EXPECT_EQ(controller,
              R"({"supported_commands":["switch"],"volume":100,"muted":false,"repeat":"off",)"
              R"("shuffle":false})");
    // metadata/v1.md: with nothing set, a state of its timestamp alone, now or past
    EXPECT_LE(state["payload"]["metadata"]["timestamp"].as<int64_t>(), server.now_us());
    EXPECT_EQ(state["payload"]["metadata"].size(), 1U);
    // The player's client/state makes its volume the group's
    client.send_json(player_state(true, {"volume", "mute"}, 30, false));
    // The mute command it then sends answers with a server/command behind the state
    client.send_json(controller_command(R"({"command":"mute","mute":true})"));
    state = next(client);
    ASSERT_EQ(type(state), "server/state");
    controller.clear();
    serializeJson(state["payload"]["controller"], controller);
    EXPECT_EQ(controller,
              R"({"supported_commands":["volume","mute","switch"],"volume":30,"muted":false,)"
              R"("repeat":"off","shuffle":false})");
    EXPECT_EQ(payload(next(client)), R"({"player":{"command":"mute","mute":true}})");
    EXPECT_FALSE(server.approve("not a client id", ApprovalScope::PLAYBACK));

    // The approval persists: a new run activates the client for playback at once
    server.stop();
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(server, client, hello_json));
    EXPECT_EQ(payload(next(client)),
              std::string(R"({"activities":["playback"],"active_roles":)") + ALL_ROLES + "}");
    // Connected once it takes the connection with its client/state
    client.send_json(player_state(true, {}, std::nullopt, std::nullopt));
    recorder.wait_for_count("connected " + id, 2);
    EXPECT_EQ(server.clients()[0].approval, ApprovalScope::PLAYBACK);

    // A revocation: no activities and no roles, and out of the server's group
    ASSERT_TRUE(server.revoke_approval(id));
    EXPECT_EQ(payload(next(client, "server/activate")), R"({"activities":[],"active_roles":[]})");
    update = next(client, "group/update");
    EXPECT_NE(update["payload"]["group_id"].as<std::string>(), group_id(*server.default_group()));
    recorder.wait_for_count("approval needed " + id, 2);
    // Saved through the provider, which the event after it shows is done: the server key, then
    // the approval and its revocation
    EXPECT_EQ(provider.saves, 3);
    EXPECT_EQ(provider.commits, 3);
    const std::vector<uint8_t>& blob = provider.blobs.at(persistence_keys::client_record_key(0));
    ASSERT_EQ(blob.size(), persistence_keys::CLIENT_RECORD_SIZE);
    EXPECT_EQ(blob.back(), 0);
    EXPECT_EQ(provider.blobs.count(persistence_keys::client_record_key(1)), 0U);
}

// pairing.md "Unpaired Access": a client without unpaired access stays at no activities, approved
// or not, and the host hears that it needs pairing rather than approval. An approval that cannot
// be saved is reported.
TEST(ServerApproval, HoldsAClientWithoutUnpairedAccess) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    provider.refuse_save = true;
    RawClient client;
    const std::string id = client_id_of(client);
    ASSERT_NO_FATAL_FAILURE(connect(server, client, hello("Locked", {"player@v1"}, false)));
    ASSERT_EQ(payload(next(client)), R"({"activities":[],"active_roles":[]})");
    recorder.wait_for("pairing required " + id);
    EXPECT_FALSE(recorder.contains("approval needed " + id));
    EXPECT_FALSE(server.clients()[0].unpaired_access);

    ASSERT_TRUE(server.approve(id, ApprovalScope::PLAYBACK));
    const sendspin::server::ServerError error = recorder.wait_for_error("");
    EXPECT_EQ(error.kind, ErrorKind::PERSISTENCE_FAILED);
    EXPECT_EQ(error.client_id, id);
    // The approval has run on core: the next message is the time reply, no activation. A
    // controller object, even a malformed one, is ignored while the role is not active
    // (messaging.md "server/activate").
    next(client, "group/update");
    client.send_json(controller_command(R"({"command":"volume"})"));
    client.send_json(sendspin::server::test::client_time_json(5));
    EXPECT_EQ(type(next(client)), "server/time");
}

// ==============================================================================
// Controller (controller/v1.md)
// ==============================================================================

// controller/v1.md: the state carries the host's caps; a command absent from supported_commands
// is ignored before its parameters are looked at, and so is a seek outside 0 to seek_max_ms;
// seek_relative is clamped from the metadata's position to the host's seek_max_ms, even when
// seek is not offered, else to the track's length; the rest reach the host in order. An offered
// command with an invalid parameter is a protocol error.
TEST(ServerController, FiltersCommandsAndForwardsTransportCommands) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    RawClient client;
    const std::string id = client_id_of(client);
    ASSERT_NO_FATAL_FAILURE(connect(server, client, hello("Remote", {"controller@v1"}, true)));
    EXPECT_EQ(payload(next(client)),
              R"({"activities":["playback"],"active_roles":["controller@v1"]})");
    // The client takes the connection (connection.md "Multiple servers (server-initiated)")
    client.send_json(R"({"type":"client/state","payload":{"available":true}})");
    EXPECT_EQ(type(next(client)), "group/update");
    EXPECT_EQ(payload(next(client)),
              R"({"controller":{"supported_commands":["switch"],"volume":100,"muted":false,)"
              R"("repeat":"off","shuffle":false}})");
    const std::string relative = "command " + group_id(group) + " " +
                                 std::to_string(static_cast<int>(TransportCommand::SEEK_RELATIVE)) +
                                 " " + id;

    // seek_relative without seek: no seek_max_ms on the wire, the host's still clamps
    ControllerCaps caps;
    caps.commands = {TransportCommand::PLAY, TransportCommand::SEEK_RELATIVE};
    caps.repeat = RepeatMode::ALL;
    caps.shuffle = true;
    caps.seek_max_ms = 60000;
    ASSERT_TRUE(server.set_controller_caps(group, caps));
    EXPECT_EQ(payload(next(client)),
              R"({"controller":{"supported_commands":["play","switch","seek_relative"],)"
              R"("volume":100,"muted":false,"repeat":"all","shuffle":true}})");
    client.send_json(controller_command(R"({"command":"pause"})"));
    // Not offered, so ignored though malformed
    client.send_json(controller_command(R"({"command":"volume"})"));
    client.send_json(controller_command(R"({"command":"seek","position_ms":100})"));
    client.send_json(controller_command(R"({"command":"rewind"})"));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":-5000})"));
    // Answered only once the commands before it are taken in: the connection is still open
    client.send_json(sendspin::server::test::client_time_json(1));
    ASSERT_EQ(type(next(client)), "server/time");
    recorder.wait_for(relative);
    // Posted before the commands below are sent, so core takes it in first
    Metadata metadata;
    metadata.progress = MetadataProgress{10000, 0, 1000};
    ASSERT_TRUE(server.set_metadata(group, metadata, server.now_us()));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":100000})"));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":-15000})"));
    // Taken in while seek is not offered
    recorder.wait_for_count(relative, 3);

    caps.commands.push_back(TransportCommand::SEEK);
    ASSERT_TRUE(server.set_controller_caps(group, caps));
    EXPECT_EQ(payload(next(client)),
              R"({"controller":{"supported_commands":["play","switch","seek","seek_relative"],)"
              R"("volume":100,"muted":false,"repeat":"all","shuffle":true,"seek_max_ms":60000}})");
    client.send_json(controller_command(R"({"command":"seek","position_ms":60001})"));
    client.send_json(controller_command(R"({"command":"seek","position_ms":-1})"));
    client.send_json(controller_command(R"({"command":"seek","position_ms":60000})"));
    client.send_json(controller_command(R"({"command":"play"})"));
    recorder.wait_for("command " + group_id(group) + " " +
                      std::to_string(static_cast<int>(TransportCommand::PLAY)) + " " + id);

    // No seek_max_ms: the track's length bounds it
    ControllerCaps relative_only;
    relative_only.commands = {TransportCommand::SEEK_RELATIVE};
    ASSERT_TRUE(server.set_controller_caps(group, relative_only));
    next(client, "server/state");
    metadata.progress = MetadataProgress{10000, 30000, 1000};
    ASSERT_TRUE(server.set_metadata(group, metadata, server.now_us()));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":100000})"));
    recorder.wait_for_count(relative, 4);

    const std::vector<std::pair<TransportCommand, CommandArgs>> commands = recorder.commands();
    ASSERT_EQ(commands.size(), 6U);
    // No position known yet: the host clamps
    EXPECT_EQ(commands[0].first, TransportCommand::SEEK_RELATIVE);
    EXPECT_EQ(commands[0].second.offset_ms, -5000);
    EXPECT_FALSE(commands[0].second.position_ms);
    EXPECT_EQ(commands[0].second.client_id, id);
    EXPECT_EQ(commands[1].second.position_ms, 60000U);
    EXPECT_EQ(commands[2].second.position_ms, 0U);
    EXPECT_EQ(commands[3].first, TransportCommand::SEEK);
    EXPECT_EQ(commands[3].second.position_ms, 60000U);
    EXPECT_EQ(commands[4].first, TransportCommand::PLAY);
    EXPECT_EQ(commands[5].first, TransportCommand::SEEK_RELATIVE);
    EXPECT_EQ(commands[5].second.position_ms, 30000U);

    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":"x"})"));
    client.expect_silent_close();
    recorder.wait_for("disconnected " + id + " " +
                      std::to_string(static_cast<int>(DisconnectReason::CLOSED_BY_SERVER)));
}

// controller/v1.md "Setting group volume" and "Setting group mute": one command per player that
// supports it, computed from the volumes the players report; a player that reports a volume it
// does not offer to set is left out of the group's volume and gets no command
// (roles/player/v1.md "Supported commands")
TEST(ServerController, SetsGroupVolumeAndMuteOnThePlayers) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    config.trace = [&recorder](const sendspin::server::TracedMessage& message) {
        recorder.trace(message);
    };
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    RawClient a;
    RawClient b;
    RawClient fixed;
    ASSERT_NO_FATAL_FAILURE(connect(server, a, hello("A", {"player@v1", "controller@v1"}, true)));
    next(a, "server/state");
    ASSERT_NO_FATAL_FAILURE(connect(server, b, hello("B", {"player@v1"}, true)));
    next(b, "group/update");
    ASSERT_NO_FATAL_FAILURE(connect(server, fixed, hello("Fixed", {"player@v1"}, true)));
    next(fixed, "group/update");

    a.send_json(player_state(true, {"volume", "mute"}, 50, false));
    EXPECT_EQ(payload(next(a, "server/state")),
              R"({"controller":{"supported_commands":["volume","mute","switch"],"volume":50,)"
              R"("muted":false,"repeat":"off","shuffle":false}})");
    b.send_json(player_state(true, {"volume", "mute"}, 90, true));
    EXPECT_EQ(payload(next(a, "server/state")),
              R"({"controller":{"supported_commands":["volume","mute","switch"],"volume":70,)"
              R"("muted":false,"repeat":"off","shuffle":false}})");
    fixed.send_json(player_state(true, {}, 0, true));
    recorder.wait_for_count("in client/state", 3);

    // From 50 and 90 to 100: 80 and 120, the clamped 20 to the other
    a.send_json(controller_command(R"({"command":"volume","volume":100})"));
    EXPECT_EQ(payload(next(a, "server/command")),
              R"({"player":{"command":"volume","volume":100}})");
    EXPECT_EQ(payload(next(b, "server/command")),
              R"({"player":{"command":"volume","volume":100}})");
    a.send_json(controller_command(R"({"command":"mute","mute":true})"));
    EXPECT_EQ(payload(next(a, "server/command")), R"({"player":{"command":"mute","mute":true}})");
    EXPECT_EQ(payload(next(b, "server/command")), R"({"player":{"command":"mute","mute":true}})");
    // The host's group volume: the players have reported no change, so from 50 and 90 again
    ASSERT_TRUE(server.set_group_volume(*server.default_group(), 30));
    EXPECT_EQ(payload(next(a, "server/command")), R"({"player":{"command":"volume","volume":10}})");
    EXPECT_EQ(payload(next(b, "server/command")), R"({"player":{"command":"volume","volume":50}})");
    EXPECT_FALSE(server.set_group_volume(*server.default_group(), 101));
    EXPECT_FALSE(server.set_group_volume(GroupId{999}, 30));
    // The player without the commands got none: its next message answers its time request
    fixed.send_json(sendspin::server::test::client_time_json(1));
    EXPECT_EQ(type(next(fixed)), "server/time");
}

// ==============================================================================
// Metadata (metadata/v1.md)
// ==============================================================================

/// The metadata objects client receives until the controller state a change of the host's caps
/// sends, which core queues behind everything queued before the call; each call alternates the
/// caps so they change
std::vector<std::string> metadata_until_caps(SendspinServer& server, GroupId group,
                                             RawClient& client, bool& toggle) {
    ControllerCaps caps;
    caps.commands = {toggle ? TransportCommand::PLAY : TransportCommand::PAUSE};
    toggle = !toggle;
    EXPECT_TRUE(server.set_controller_caps(group, caps));
    std::vector<std::string> states;
    for (;;) {
        const JsonDocument state = next(client, "server/state");
        if (state["payload"]["controller"]) {
            return states;
        }
        std::string metadata;
        serializeJson(state["payload"]["metadata"], metadata);
        states.push_back(std::move(metadata));
    }
}

// metadata/v1.md: a client joining gets the current state, then the scheduled update it would
// hold; a pause sends the state at speed 0 with the position reached, and the resume the same
// position at the speed the pause took away, from the run's first sample on
TEST(ServerMetadata, JoinsWithCurrentThenScheduledAndFollowsPauseAndResume) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Metadata current;
    current.title = "A";
    current.progress = MetadataProgress{1000, 100000, 1000};
    const int64_t set_at = server.now_us();
    ASSERT_TRUE(server.set_metadata(group, current, set_at));
    Metadata scheduled;
    scheduled.title = "B";
    constexpr int64_t AHEAD_US = 10000000;
    ASSERT_TRUE(server.set_metadata(group, scheduled, set_at + AHEAD_US));

    RawClient client;
    ASSERT_NO_FATAL_FAILURE(
        connect(server, client, hello("Display", {"controller@v1", "metadata@v1"}, true)));
    ASSERT_EQ(type(next(client)), "server/activate");
    ASSERT_EQ(type(next(client)), "group/update");
    JsonDocument state = next(client);
    ASSERT_EQ(type(state), "server/state");
    EXPECT_EQ(state["payload"]["metadata"]["title"], "A");
    EXPECT_EQ(state["payload"]["metadata"]["timestamp"].as<int64_t>(), set_at);
    bool toggle = true;
    EXPECT_EQ(metadata_until_caps(server, group, client, toggle),
              std::vector<std::string>{R"({"timestamp":)" + std::to_string(set_at + AHEAD_US) +
                                       R"(,"title":"B"})"});

    sendspin::server::InputOptions options;
    options.format = {48000, 2, sendspin::server::SampleFormat::S16};
    const std::shared_ptr<sendspin::server::GroupInput> input =
        server.open_input(group, options).input;
    ASSERT_TRUE(input);
    const std::vector<uint8_t> silence(size_t{48000} * 4, 0);
    while (input->write(silence.data(), 48000).frames == 0) {
        input->wait_writable(std::numeric_limits<int64_t>::max());
    }
    const std::string name = "group " + std::to_string(group.value);
    recorder.wait_for(name + " playing");
    const size_t stops = recorder.count(name + " stopped");
    input->pause();
    // The run's end queues the state in the handler that reports the group stopped
    recorder.wait_for_count(name + " stopped", stops + 1);
    std::vector<std::string> states = metadata_until_caps(server, group, client, toggle);
    ASSERT_EQ(states.size(), 1U);
    JsonDocument paused;
    deserializeJson(paused, states[0]);
    EXPECT_EQ(paused["title"], "A");
    EXPECT_EQ(paused["progress"]["playback_speed"], 0);
    const uint32_t position = paused["progress"]["track_progress"].as<uint32_t>();
    EXPECT_GE(position, 1000U);
    const int64_t paused_at = paused["timestamp"].as<int64_t>();

    const size_t plays = recorder.count(name + " playing");
    input->resume();
    recorder.wait_for_count(name + " playing", plays + 1);
    states = metadata_until_caps(server, group, client, toggle);
    ASSERT_EQ(states.size(), 1U);
    JsonDocument resumed;
    deserializeJson(resumed, states[0]);
    EXPECT_EQ(resumed["title"], "A");
    EXPECT_EQ(resumed["progress"]["playback_speed"], 1000);
    EXPECT_EQ(resumed["progress"]["track_progress"].as<uint32_t>(), position);
    EXPECT_GT(resumed["timestamp"].as<int64_t>(), paused_at);
}

// ==============================================================================
// Availability and disconnects
// ==============================================================================

// messaging.md "client/goodbye": the reason reaches the host with the disconnect; one this
// server does not recognize is reported as none. Control: a drop without a goodbye.
TEST(ServerDisconnect, ReportsTheGoodbyeReason) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ASSERT_EQ(server.start(sendspin::server::test::local_config(), &provider, &recorder),
              StartResult::OK);
    RawClient client;
    const std::string id = client_id_of(client);
    const std::string reasons[] = {"another_server", "moving", ""};
    for (size_t i = 0; i < std::size(reasons); ++i) {
        SCOPED_TRACE(reasons[i]);
        ASSERT_NO_FATAL_FAILURE(connect(server, client, hello("Leaving", {}, true)));
        next(client, "group/update");
        if (reasons[i].empty()) {
            client.drop();
        } else {
            client.send_json(sendspin::server::test::client_goodbye_json(reasons[i]));
        }
        recorder.wait_for_disconnects(i + 1);
    }
    const std::vector<ClientDisconnect> disconnects = recorder.disconnects();
    ASSERT_EQ(disconnects.size(), 3U);
    EXPECT_EQ(disconnects[0].reason, DisconnectReason::GOODBYE);
    EXPECT_EQ(disconnects[0].goodbye_reason, GoodbyeReason::ANOTHER_SERVER);
    EXPECT_EQ(disconnects[1].reason, DisconnectReason::GOODBYE);
    EXPECT_FALSE(disconnects[1].goodbye_reason);
    EXPECT_EQ(disconnects[2].reason, DisconnectReason::CONNECTION_LOST);
    EXPECT_FALSE(disconnects[2].goodbye_reason);
}

// A player that reconnects within the grace rejoins the group it left, which is kept for it; here
// the solo group it moved to on turning unavailable (messaging.md "Server behavior when a client
// becomes unavailable"), so it is not put back in the server's group. Control: past the grace its
// group is gone, and an approved player joins the server's group; so too after a client/goodbye
// `another_server`, which says it is not coming back.
TEST(ServerGroups, AClientBackWithinTheGraceRejoinsItsGroup) {
    enum class Leaves : uint8_t { WITHIN, PAST, ANOTHER_SERVER };
    for (const Leaves leaves : {Leaves::WITHIN, Leaves::PAST, Leaves::ANOTHER_SERVER}) {
        SCOPED_TRACE(static_cast<int>(leaves));
        const bool within = leaves == Leaves::WITHIN;
        MemoryProvider provider;
        Recorder recorder;
        SendspinServer server;
        ServerConfig config = sendspin::server::test::local_config();
        config.approve_all_clients = true;
        if (leaves == Leaves::PAST) {
            config.timing.group_rejoin_grace_us = 1;
        }
        ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
        const std::string default_group = group_id(*server.default_group());
        RawClient a;
        RawClient b;
        const std::string hello_json = hello("A", {"player@v1"}, true);
        ASSERT_NO_FATAL_FAILURE(connect(server, a, hello_json));
        EXPECT_EQ(next(a, "group/update")["payload"]["group_id"].as<std::string>(), default_group);
        ASSERT_NO_FATAL_FAILURE(connect(server, b, hello("B", {"player@v1"}, true)));
        next(b, "group/update");
        // The first client/state is an initial value; the second a transition
        a.send_json(player_state(true, {}, std::nullopt, std::nullopt));
        b.send_json(player_state(true, {}, std::nullopt, std::nullopt));
        a.send_json(player_state(false, {}, std::nullopt, std::nullopt));
        const JsonDocument moved = next(a, "group/update");
        const std::string solo = moved["payload"]["group_id"].as<std::string>();
        EXPECT_NE(solo, default_group);
        EXPECT_EQ(moved["payload"]["playback_state"].as<std::string>(), "stopped");

        if (leaves == Leaves::ANOTHER_SERVER) {
            a.send_json(sendspin::server::test::client_goodbye_json("another_server"));
        }
        a.close();
        recorder.wait_for_disconnects(1);
        const auto has_solo = [&] {
            const std::vector<GroupInfo> groups = server.groups();
            return std::any_of(groups.begin(), groups.end(),
                               [&](const GroupInfo& group) { return group_id(group.id) == solo; });
        };
        if (leaves == Leaves::PAST) {
            while (has_solo()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        ASSERT_NO_FATAL_FAILURE(connect(server, a, hello_json));
        EXPECT_EQ(type(next(a)), "server/activate");
        const JsonDocument rejoined = next(a);
        ASSERT_EQ(type(rejoined), "group/update");
        EXPECT_EQ(rejoined["payload"]["group_id"].as<std::string>(), within ? solo : default_group);
    }
}

// ==============================================================================
// The host's groups (create_group(), rename_group(), move_client())
// ==============================================================================

/// A started server that approves every client
class GroupsServer {
public:
    GroupsServer() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Living room";
        config.approve_all_clients = true;
        EXPECT_EQ(this->server.start(config, &this->provider, &this->recorder), StartResult::OK);
    }

    /// Connects client with every implemented role, reporting a volume of 50 it offers to set, and
    /// reads its activation, group/update and roles' server/state
    /// @return the group_id its group/update carries
    std::string join(RawClient& client, const std::string& name) {
        connect(this->server, client,
                hello(name, {"player@v1", "controller@v1", "metadata@v1"}, true));
        EXPECT_EQ(type(next(client)), "server/activate");
        client.send_json(player_state(true, {"volume", "mute"}, 50, false));
        const JsonDocument update = next(client);
        EXPECT_EQ(type(update), "group/update");
        // The state its group had, then the controller's once its own volume counts
        while (next(client, "server/state")["payload"]["controller"]["volume"] != 50) {}
        return update["payload"]["group_id"].as<std::string>();
    }

    /// The group's entry in groups(), sampled until whether it is listed is listed; with no
    /// timeout, so a regression hangs into the suite watchdog
    std::optional<GroupInfo> wait_until(GroupId group, bool listed) {
        for (;;) {
            const std::vector<GroupInfo> groups = this->server.groups();
            const auto it = std::find_if(groups.begin(), groups.end(),
                                         [&](const GroupInfo& info) { return info.id == group; });
            if ((it != groups.end()) == listed) {
                return it == groups.end() ? std::nullopt : std::optional<GroupInfo>(*it);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    /// The renames expect_nothing_queued() made
    int renames{0};
};

/// A host writing silence to a group's input until destroyed; constructed once the group plays
class Silence {
public:
    Silence(GroupsServer& s, GroupId group) {
        sendspin::server::InputOptions options;
        options.format = {48000, 2, sendspin::server::SampleFormat::S16};
        this->input = s.server.open_input(group, options).input;
        EXPECT_TRUE(this->input);
        this->thread_ = std::thread([this] {
            constexpr size_t FRAMES = 960;
            const std::vector<uint8_t> block(FRAMES * 4, 0);
            while (!this->stop_.load()) {
                const sendspin::server::WriteResult result =
                    this->input->write(block.data(), FRAMES);
                if (result.state == sendspin::server::InputState::FULL) {
                    this->input->wait_writable(20000);
                } else if (result.frames == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });
        s.recorder.wait_for("group " + group_id(group) + " playing");
    }

    ~Silence() {
        this->stop_ = true;
        this->thread_.join();
    }

    Silence(const Silence&) = delete;
    Silence& operator=(const Silence&) = delete;

    std::shared_ptr<sendspin::server::GroupInput> input;

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// The next JSON message of type, skipping others and audio chunks
JsonDocument next_json(RawClient& client, std::string_view type) {
    for (;;) {
        const std::optional<std::vector<uint8_t>> plaintext = client.receive_plaintext();
        if (!plaintext) {
            ADD_FAILURE() << "the connection closed while waiting for " << type;
            return JsonDocument();
        }
        if (plaintext->empty() || plaintext->front() != 0) {
            continue;
        }
        JsonDocument doc;
        deserializeJson(doc, plaintext->data() + 1, plaintext->size() - 1);
        if (doc["type"].as<std::string_view>() == type) {
            return doc;
        }
    }
}

/// The group/update a move into a playing group gives, read with the stream/start that follows it:
/// a marker queued before the move emits the start when it reaches the head, so the start can come
/// first
JsonDocument next_join(RawClient& client) {
    std::optional<JsonDocument> update;
    bool started = false;
    while (!update || !started) {
        const std::optional<std::vector<uint8_t>> plaintext = client.receive_plaintext();
        if (!plaintext) {
            ADD_FAILURE() << "the connection closed while waiting for the join";
            return JsonDocument();
        }
        if (plaintext->empty() || plaintext->front() != 0) {
            continue;
        }
        JsonDocument doc;
        deserializeJson(doc, plaintext->data() + 1, plaintext->size() - 1);
        const std::string_view type = doc["type"].as<std::string_view>();
        if (type == "group/update") {
            update = std::move(doc);
        } else if (type == "stream/start") {
            started = true;
        }
    }
    return *update;
}

/// Proves client, in group, was sent nothing since the last message read from it: a rename of
/// the group queues a group/update behind anything queued for it, so that update comes next (a
/// time reply would not do: the pump takes one ahead of what is queued)
void expect_nothing_queued(GroupsServer& s, RawClient& client, GroupId group) {
    const std::string name = "Renamed " + std::to_string(++s.renames);
    ASSERT_TRUE(s.server.rename_group(group, name));
    const JsonDocument update = next(client);
    EXPECT_EQ(type(update), "group/update");
    EXPECT_EQ(update["payload"]["group_name"], name);
}

// messaging.md "group/update": a client the host moves into a group it creates is told with a
// group/update carrying every field, then gets the group's controller and metadata state
// (messaging.md "server/state"; metadata/v1.md: the new group's current state); a rename is told
// to the members the same way; moved back, it gets its old group's state, and the group it left,
// empty and with no input, goes. The member that stays is sent nothing.
TEST(ServerGroups, MovesAClientIntoAGroupTheHostCreatesAndBack) {
    GroupsServer s;
    const GroupId living = *s.server.default_group();
    RawClient a;
    RawClient b;
    EXPECT_EQ(s.join(a, "A"), group_id(living));
    EXPECT_EQ(s.join(b, "B"), group_id(living));
    const std::string b_id = client_id_of(b);
    Metadata metadata;
    metadata.title = "Living room's";
    ASSERT_TRUE(s.server.set_metadata(living, metadata, s.server.now_us()));
    EXPECT_EQ(next(a, "server/state")["payload"]["metadata"]["title"], "Living room's");
    EXPECT_EQ(next(b, "server/state")["payload"]["metadata"]["title"], "Living room's");

    const std::optional<GroupId> kitchen = s.server.create_group("Kitchen", b_id);
    ASSERT_TRUE(kitchen);
    EXPECT_EQ(payload(next(b)), R"({"playback_state":"stopped","group_id":")" + group_id(*kitchen) +
                                    R"(","group_name":"Kitchen"})");
    JsonDocument state = next(b);
    ASSERT_EQ(type(state), "server/state");
    EXPECT_EQ(state["payload"]["controller"]["volume"], 50);
    // Nothing set in the kitchen: its timestamp alone
    EXPECT_EQ(state["payload"]["metadata"].size(), 1U);
    // A stopped group's member declares no activities, keeping its roles
    EXPECT_EQ(payload(next(b)), R"({"activities":[]})");
    s.recorder.wait_for("group " + group_id(*kitchen) + " stopped");
    const std::optional<GroupInfo> listed = s.wait_until(*kitchen, true);
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->name, "Kitchen");
    EXPECT_EQ(listed->client_ids, std::vector<std::string>{b_id});
    // The controller state A sees is unchanged by B leaving, both at 50
    expect_nothing_queued(s, a, living);

    ASSERT_TRUE(s.server.rename_group(*kitchen, "Den"));
    EXPECT_EQ(payload(next(b)), R"({"playback_state":"stopped","group_id":")" + group_id(*kitchen) +
                                    R"(","group_name":"Den"})");
    expect_nothing_queued(s, a, living);

    ASSERT_TRUE(s.server.move_client(b_id, living));
    EXPECT_EQ(next(b)["payload"]["group_id"].as<std::string>(), group_id(living));
    state = next(b);
    ASSERT_EQ(type(state), "server/state");
    EXPECT_EQ(state["payload"]["metadata"]["title"], "Living room's");
    EXPECT_FALSE(s.wait_until(*kitchen, false));
    // Moving into the group it is in changes nothing
    ASSERT_TRUE(s.server.move_client(b_id, living));
    expect_nothing_queued(s, b, living);
}

// server.h: what create_group(), rename_group(), move_client() and set_player_format() refuse at
// once, without waiting for core: a client_id that is not one, a group not in groups(), a name
// past config_limits::MAX_NAME_BYTES, and every call while stopped
TEST(ServerGroups, RefusesCallsItCanJudgeAtOnce) {
    GroupsServer s;
    RawClient a;
    s.join(a, "A");
    const std::string a_id = client_id_of(a);
    const GroupId living = *s.server.default_group();
    const GroupId none{999};
    const std::string longest(sendspin::server::config_limits::MAX_NAME_BYTES, 'x');
    const std::string too_long = longest + "x";
    const StreamFormat flac{StreamCodec::FLAC, 48000, 2, 16};
    struct Row {
        const char* name;
        std::function<bool()> call;
        bool accepted;
    };
    const std::vector<Row> rows = {
        {"Control: create_group(), a name at the limit",
         [&] { return s.server.create_group(longest, a_id).has_value(); }, true},
        {"create_group(), no client_id",
         [&] { return s.server.create_group("Hall", "not a client id").has_value(); }, false},
        {"create_group(), a name too long",
         [&] { return s.server.create_group(too_long, a_id).has_value(); }, false},
        {"Control: rename_group()", [&] { return s.server.rename_group(living, longest); }, true},
        {"rename_group(), no group", [&] { return s.server.rename_group(none, "Gone"); }, false},
        {"rename_group(), a name too long", [&] { return s.server.rename_group(living, too_long); },
         false},
        {"Control: move_client()", [&] { return s.server.move_client(a_id, living); }, true},
        {"move_client(), no group", [&] { return s.server.move_client(a_id, none); }, false},
        {"move_client(), no client_id",
         [&] { return s.server.move_client("not a client id", living); }, false},
        {"Control: set_player_format()", [&] { return s.server.set_player_format(a_id, flac); },
         true},
        {"set_player_format(), no client_id",
         [&] { return s.server.set_player_format("not a client id", flac); }, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(row.call(), row.accepted);
    }
    s.server.stop();
    for (const Row& row : rows) {
        SCOPED_TRACE(std::string("stopped: ") + row.name);
        EXPECT_FALSE(row.call());
    }
}

// A client held with no roles (here one without unpaired access) stays in a group of its own: the
// host's create_group() makes no group for it and move_client() leaves it
TEST(ServerGroups, LeavesAClientHeldWithNoRolesInItsOwnGroup) {
    GroupsServer s;
    RawClient a;
    RawClient held;
    s.join(a, "A");
    connect(s.server, held, hello("Held", {"player@v1"}, false));
    EXPECT_EQ(payload(next(held)), R"({"activities":[],"active_roles":[]})");
    const std::string own = next(held, "group/update")["payload"]["group_id"].as<std::string>();
    const std::string held_id = client_id_of(held);
    s.recorder.wait_for("pairing required " + held_id);

    const std::optional<GroupId> refused = s.server.create_group("Refused", held_id);
    ASSERT_TRUE(refused);
    ASSERT_TRUE(s.server.move_client(held_id, *s.server.default_group()));
    // Control: A, which may play, moves; core takes the calls in order, so A's move shows the two
    // before it were taken
    const std::optional<GroupId> taken = s.server.create_group("Taken", client_id_of(a));
    ASSERT_TRUE(taken);
    EXPECT_EQ(next(a, "group/update")["payload"]["group_id"].as<std::string>(), group_id(*taken));
    ASSERT_TRUE(s.wait_until(*taken, true));
    for (const GroupInfo& group : s.server.groups()) {
        EXPECT_NE(group.id, *refused);
        if (group_id(group.id) == own) {
            EXPECT_EQ(group.client_ids, std::vector<std::string>{held_id});
        }
    }
    expect_nothing_queued(s, held, GroupId{std::stoull(own)});
}

// controller/v1.md: a controller acts on its own group: its volume command sets its group's
// players only, a transport command reaches the host with its group, which offers it with that
// group's caps, and metadata set for a group reaches that group's members only (metadata/v1.md)
TEST(ServerGroups, ScopesControllerCommandsAndMetadataToTheirGroup) {
    GroupsServer s;
    const GroupId living = *s.server.default_group();
    RawClient a;
    RawClient b;
    s.join(a, "A");
    s.join(b, "B");
    const std::string b_id = client_id_of(b);
    const GroupId kitchen = *s.server.create_group("Kitchen", b_id);
    next(b, "group/update");
    next(b, "server/state");

    a.send_json(controller_command(R"({"command":"volume","volume":80})"));
    EXPECT_EQ(payload(next(a, "server/command")), R"({"player":{"command":"volume","volume":80}})");
    b.send_json(controller_command(R"({"command":"mute","mute":true})"));
    EXPECT_EQ(payload(next(b, "server/command")), R"({"player":{"command":"mute","mute":true}})");
    expect_nothing_queued(s, a, living);
    expect_nothing_queued(s, b, kitchen);

    ControllerCaps caps;
    caps.commands = {TransportCommand::NEXT};
    ASSERT_TRUE(s.server.set_controller_caps(kitchen, caps));
    EXPECT_EQ(next(b, "server/state")["payload"]["controller"]["supported_commands"][0], "next");
    // Not offered in A's group: ignored, taken in before the time reply that follows it, while
    // B's reaches the host after it, with the kitchen's id
    a.send_json(controller_command(R"({"command":"next"})"));
    a.send_json(sendspin::server::test::client_time_json(1));
    next(a, "server/time");
    b.send_json(controller_command(R"({"command":"next"})"));
    const std::string next_command = std::to_string(static_cast<int>(TransportCommand::NEXT));
    s.recorder.wait_for("command " + group_id(kitchen) + " " + next_command + " " + b_id);
    EXPECT_EQ(s.recorder.count("command " + group_id(living) + " " + next_command + " " +
                               client_id_of(a)),
              0U);

    Metadata metadata;
    metadata.title = "Kitchen's";
    ASSERT_TRUE(s.server.set_metadata(kitchen, metadata, s.server.now_us()));
    EXPECT_EQ(next(b, "server/state")["payload"]["metadata"]["title"], "Kitchen's");
    expect_nothing_queued(s, a, living);
}

// The host's move of a client that went away without a goodbye moves the place kept for it in its
// group, so it rejoins the group it was moved to, and the group it left, empty, goes; into a group
// that plays, it joins that group's playback; create_group() with it as the first member makes a
// group for its place
TEST(ServerGroups, MovesThePlaceKeptForADisconnectedClient) {
    enum class Into : uint8_t { STOPPED, PLAYING, CREATED };
    struct Row {
        const char* name;
        Into into;
    };
    const Row rows[] = {
        {"Control: move_client() into a stopped group", Into::STOPPED},
        {"move_client() into a playing group", Into::PLAYING},
        {"create_group() for it", Into::CREATED},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        GroupsServer s;
        RawClient a;
        RawClient b;
        s.join(a, "A");
        s.join(b, "B");
        const std::string a_id = client_id_of(a);
        const GroupId kitchen = *s.server.create_group("Kitchen", client_id_of(b));
        next(b, "group/update");
        const GroupId solo = *s.server.create_group("Solo", a_id);
        EXPECT_EQ(next(a, "group/update")["payload"]["group_id"].as<std::string>(), group_id(solo));
        std::optional<Silence> playing;
        if (row.into == Into::PLAYING) {
            playing.emplace(s, kitchen);
        }
        a.close();
        s.recorder.wait_for_disconnects(1);
        GroupId joined = kitchen;
        if (row.into == Into::CREATED) {
            joined = *s.server.create_group("Created", a_id);
            ASSERT_TRUE(s.wait_until(joined, true));
        } else {
            ASSERT_TRUE(s.server.move_client(a_id, kitchen));
        }
        EXPECT_FALSE(s.wait_until(solo, false));

        connect(s.server, a, hello("A", {"player@v1", "controller@v1", "metadata@v1"}, true));
        EXPECT_EQ(type(next(a)), "server/activate");
        a.send_json(player_state(true, {}, std::nullopt, std::nullopt));
        const JsonDocument update =
            row.into == Into::PLAYING ? next_join(a) : next_json(a, "group/update");
        EXPECT_EQ(update["payload"]["group_id"].as<std::string>(), group_id(joined));
        if (row.into == Into::PLAYING) {
            EXPECT_EQ(update["payload"]["playback_state"], "playing");
        }
    }
}

// messaging.md "External Source Handling": only a solo member that turns unavailable or leaves
// stops its group. The host moving the last member out of a playing group leaves it playing, with
// its input open, until the host releases the input, when the group goes. Control: the same
// member's client/leave stops the group and latches the input.
TEST(ServerGroups, KeepsPlayingAGroupItsLastMemberIsMovedOutOf) {
    for (const bool leave : {false, true}) {
        SCOPED_TRACE(leave ? "Control: client/leave" : "move_client()");
        GroupsServer s;
        RawClient b;
        s.join(b, "B");
        const std::string b_id = client_id_of(b);
        const GroupId kitchen = *s.server.create_group("Kitchen", b_id);
        next(b, "group/update");
        std::optional<Silence> playing(std::in_place, s, kitchen);
        if (leave) {
            const std::string stopped = "group " + group_id(kitchen) + " stopped";
            const size_t stops = s.recorder.count(stopped);
            b.send_json(R"({"type":"client/leave","payload":{}})");
            s.recorder.wait_for_count(stopped, stops + 1);
            EXPECT_EQ(playing->input->state(), sendspin::server::InputState::STOP_LATCHED);
            continue;
        }
        ASSERT_TRUE(s.server.move_client(b_id, *s.server.default_group()));
        // The move and any stop it caused are published together, with B listed in its new group
        for (;;) {
            const std::vector<GroupInfo> groups = s.server.groups();
            const auto living = std::find_if(groups.begin(), groups.end(), [&](const GroupInfo& g) {
                return g.id == *s.server.default_group();
            });
            if (living != groups.end() && !living->client_ids.empty()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Barrier: core answers the client/time after any handler the move posted, so a stop
        // posted for later is published before groups() is sampled
        b.send_json(sendspin::server::test::client_time_json(1));
        next_json(b, "server/time");
        const std::optional<GroupInfo> left = s.wait_until(kitchen, true);
        ASSERT_TRUE(left);
        EXPECT_TRUE(left->client_ids.empty());
        EXPECT_EQ(left->playback_state, sendspin::server::PlaybackState::PLAYING);
        EXPECT_NE(playing->input->state(), sendspin::server::InputState::STOP_LATCHED);
        playing.reset();
        EXPECT_FALSE(s.wait_until(kitchen, false));
    }
}

// messaging.md "Server behavior when a client becomes unavailable" forbids the server rejoining an
// unavailable client to a group, not the host: moved into a playing group while its first
// client/state reports it unavailable, a client gets the group's group/update and no stream until
// it reports available. Control: one available on arrival starts at once.
TEST(ServerGroups, StartsAClientMovedWhileUnavailableOnceAvailable) {
    for (const bool available : {true, false}) {
        SCOPED_TRACE(available ? "Control: available" : "unavailable");
        GroupsServer s;
        RawClient a;
        RawClient b;
        s.join(b, "B");
        const GroupId kitchen = *s.server.create_group("Kitchen", client_id_of(b));
        next(b, "group/update");
        Silence playing(s, kitchen);
        connect(s.server, a, hello("A", {"player@v1", "controller@v1", "metadata@v1"}, true));
        EXPECT_EQ(type(next(a)), "server/activate");
        // The first client/state is an initial value, so A stays in the server's group
        a.send_json(player_state(available, {}, std::nullopt, std::nullopt));
        next(a, "group/update");
        ASSERT_TRUE(s.server.move_client(client_id_of(a), kitchen));
        const JsonDocument update = available ? next_join(a) : next_json(a, "group/update");
        EXPECT_EQ(update["payload"]["group_id"].as<std::string>(), group_id(kitchen));
        EXPECT_EQ(update["payload"]["playback_state"], "playing");
        if (!available) {
            next(a, "server/state");
            expect_nothing_queued(s, a, kitchen);
            a.send_json(player_state(true, {}, std::nullopt, std::nullopt));
            next_json(a, "stream/start");
        }
    }
}

// messaging.md "client/leave", controller/v1.md "Switch command cycle": a client that leaves its
// shared group is moved to a stopped group of its own; its switch then takes it back to the group
// it left first, and the next switch, with no group playing, to a new group of its own
TEST(ServerGroups, SwitchesBackToTheGroupItLeft) {
    GroupsServer s;
    const std::string living = group_id(*s.server.default_group());
    RawClient a;
    RawClient b;
    s.join(a, "A");
    s.join(b, "B");

    b.send_json(R"({"type":"client/leave","payload":{}})");
    const JsonDocument left = next(b, "group/update");
    const std::string solo = left["payload"]["group_id"].as<std::string>();
    EXPECT_NE(solo, living);
    EXPECT_EQ(left["payload"]["playback_state"], "stopped");
    b.send_json(controller_command(R"({"command":"switch"})"));
    EXPECT_EQ(next(b, "group/update")["payload"]["group_id"].as<std::string>(), living);
    b.send_json(controller_command(R"({"command":"switch"})"));
    const std::string own = next(b, "group/update")["payload"]["group_id"].as<std::string>();
    EXPECT_NE(own, living);
    EXPECT_NE(own, solo);
}

// ==============================================================================
// In-process sendspin-cpp clients
// ==============================================================================

class AlwaysReady : public sendspin::SendspinNetworkProvider {
public:
    bool is_network_ready() override {
        return true;
    }
};

/// What a sendspin-cpp client's roles report; on its main loop, the test's thread, except
/// on_audio_write()
class Events : public sendspin::PlayerRoleListener,
               public sendspin::ControllerRoleListener,
               public sendspin::MetadataRoleListener {
public:
    size_t on_audio_write(uint8_t* /*data*/, size_t length, uint32_t /*timeout_ms*/) override {
        // An output that takes a little while, so the sync task does not spin
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return length;
    }

    void on_stream_start() override {
        ++this->starts;
    }

    void on_stream_end() override {
        ++this->ends;
    }

    void on_controller_state(const sendspin::ServerStateControllerObject& state) override {
        this->controller = state;
    }

    void on_controller_state_clear() override {
        this->controller.reset();
    }

    void on_metadata(const sendspin::ServerMetadataStateObject& metadata) override {
        this->metadata = metadata;
    }

    void on_metadata_clear() override {
        this->metadata.reset();
    }

    bool offers(sendspin::SendspinControllerCommand command) const {
        return this->controller && std::find(this->controller->supported_commands.begin(),
                                             this->controller->supported_commands.end(),
                                             command) != this->controller->supported_commands.end();
    }

    int starts{0};
    int ends{0};
    std::optional<sendspin::ServerStateControllerObject> controller;
    std::optional<sendspin::ServerMetadataStateObject> metadata;
};

/// A sendspin-cpp client with a player and, when asked, the controller and metadata roles,
/// listening for the server to dial
class Peer {
public:
    Peer(const std::string& name, bool controller, uint8_t volume)
        : port_(sendspin::server::test::free_port()), client_(make_config(name, this->port_)) {
        sendspin::PlayerRoleConfig config;
        // One format, assigned by count: GCC 13 -Warray-bounds misreads an initializer-list
        // assignment
        config.audio_formats.assign(1, sendspin::AudioSupportedFormatObject{
                                           sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
        this->player = &this->client_.add_player(config);
        this->player->set_listener(&this->events);
        this->player->update_volume(volume);
        if (controller) {
            this->controller = &this->client_.add_controller();
            this->controller->set_listener(&this->events);
            this->client_.add_metadata().set_listener(&this->events);
        }
        this->client_.set_network_provider(&this->network_);
        this->client_.set_unpaired_access_enabled(true);
        EXPECT_TRUE(this->client_.start());
    }

    ~Peer() {
        this->client_.stop();
    }

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(this->port_) + "/sendspin";
    }

    void loop() {
        this->client_.loop();
    }

    Events events;
    sendspin::PlayerRole* player{nullptr};
    sendspin::ControllerRole* controller{nullptr};

private:
    static sendspin::SendspinClientConfig make_config(const std::string& name, uint16_t port) {
        sendspin::SendspinClientConfig config;
        config.name = name;
        config.server_port = port;
        config.time_burst_interval_ms = 50;
        return config;
    }

    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
};

/// Runs each peer's main loop and step on this thread until done() holds, with no timeout
template <typename Done, typename Step>
void run_until(const std::vector<Peer*>& peers, Done done, Step step) {
    while (!done()) {
        for (Peer* peer : peers) {
            peer->loop();
        }
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

template <typename Done>
void run_until(const std::vector<Peer*>& peers, Done done) {
    run_until(peers, done, [] {});
}

/// The client_id of the client at url, or empty
std::string client_at(const SendspinServer& server, const std::string& url) {
    for (const ClientInfo& client : server.clients()) {
        if (client.url == url) {
            return client.client_id;
        }
    }
    return {};
}

/// Index of the last entry equal to what, or -1
std::ptrdiff_t last_index(const std::vector<Recorder::Entry>& entries, const std::string& what) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->what == what) {
            return entries.rend() - it - 1;
        }
    }
    return -1;
}

// pairing.md "Unpaired Access" with a sendspin-cpp client: held while unapproved, its approval
// starts its player's stream in the playing server's group and brings its controller and metadata
// state; revoking it ends the stream, with stream/end ahead of the activation that removes the
// role (messaging.md "server/activate"), and the client drops its controller and metadata state
TEST(ServerRolesInProcess, ApprovalStartsAndRevocationEndsAPlayersStream) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.trace = [&recorder](const sendspin::server::TracedMessage& message) {
        recorder.trace(message);
    };
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Peer peer("Kitchen", true, 40);
    ASSERT_TRUE(server.connect_url(peer.url()));
    std::string id;
    run_until({&peer}, [&] {
        id = client_at(server, peer.url());
        return !id.empty() && recorder.contains("approval needed " + id);
    });

    sendspin::server::InputOptions options;
    options.format = {48000, 2, sendspin::server::SampleFormat::S16};
    const std::shared_ptr<sendspin::server::GroupInput> input =
        server.open_input(group, options).input;
    ASSERT_TRUE(input);
    const std::vector<uint8_t> silence(size_t{960} * 4, 0);
    const auto write = [&] { input->write(silence.data(), 960); };
    const std::string playing = "group " + std::to_string(group.value) + " playing";
    run_until({&peer}, [&] { return recorder.contains(playing); }, write);
    EXPECT_EQ(peer.events.starts, 0);
    EXPECT_FALSE(peer.events.controller);

    ASSERT_TRUE(server.approve(id, ApprovalScope::PLAYBACK));
    run_until(
        {&peer},
        [&] {
            return peer.events.starts == 1 && peer.events.controller && peer.events.metadata &&
                   peer.events.controller->volume == 40;
        },
        write);
    EXPECT_TRUE(peer.events.offers(sendspin::SendspinControllerCommand::VOLUME));

    ASSERT_TRUE(server.revoke_approval(id));
    run_until(
        {&peer},
        [&] { return peer.events.ends >= 1 && !peer.events.controller && !peer.events.metadata; },
        write);
    const std::vector<Recorder::Entry> entries = recorder.entries();
    const std::ptrdiff_t end = last_index(entries, "out stream/end");
    ASSERT_GE(end, 0);
    EXPECT_LT(end, last_index(entries, "out server/activate"));
}

// controller/v1.md with sendspin-cpp clients: the controller shows the players' average volume,
// its volume command sets each player's, a transport command the host offers reaches it, and the
// metadata role shows the group's metadata, a scheduled update included (metadata/v1.md)
TEST(ServerRolesInProcess, ControllerAndMetadataReachASendspinCppClient) {
    MemoryProvider provider;
    Recorder recorder;
    SendspinServer server;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
    const GroupId group = *server.default_group();
    Peer a("A", true, 20);
    Peer b("B", false, 80);
    ASSERT_TRUE(server.connect_url(a.url()));
    ASSERT_TRUE(server.connect_url(b.url()));
    const std::vector<Peer*> peers = {&a, &b};
    run_until(peers, [&] { return a.events.controller && a.events.controller->volume == 50; });

    // From 20 and 80 to 100: 70 and 130, the clamped 30 to the other
    ASSERT_TRUE(a.controller->send_command(
        {.command = sendspin::SendspinControllerCommand::VOLUME, .volume = 100}));
    run_until(peers, [&] {
        return a.player->get_volume() == 100 && b.player->get_volume() == 100 &&
               a.events.controller->volume == 100;
    });

    ControllerCaps caps;
    caps.commands = {TransportCommand::PLAY};
    ASSERT_TRUE(server.set_controller_caps(group, caps));
    run_until(peers, [&] { return a.events.offers(sendspin::SendspinControllerCommand::PLAY); });
    ASSERT_TRUE(a.controller->send_command({.command = sendspin::SendspinControllerCommand::PLAY}));
    const std::string played = "command " + std::to_string(group.value) + " " +
                               std::to_string(static_cast<int>(TransportCommand::PLAY)) + " " +
                               client_at(server, a.url());
    run_until(peers, [&] { return recorder.contains(played); });

    Metadata now;
    now.title = "Now";
    ASSERT_TRUE(server.set_metadata(group, now, server.now_us()));
    run_until(peers, [&] { return a.events.metadata && a.events.metadata->title == "Now"; });
    Metadata next_track;
    next_track.title = "Next";
    constexpr int64_t AHEAD_US = 300000;
    ASSERT_TRUE(server.set_metadata(group, next_track, server.now_us() + AHEAD_US));
    run_until(peers, [&] { return a.events.metadata && a.events.metadata->title == "Next"; });
}

// ==============================================================================
// Message trace (ServerConfig::trace_writes)
// ==============================================================================

// ServerConfig::trace_writes: the completed write of each outbound message is traced after it, and
// a server/time's comes before a client/time the client sent once it had read that reply, which is
// what lets a host place what it received against what was on the wire; without the option the
// trace has no such entry
TEST(ServerTrace, TracesCompletedWritesOnlyWhenAsked) {
    struct Seen {
        bool written;
        bool inbound;
        std::string type;
        bool empty;
    };
    for (const bool trace_writes : {false, true}) {
        SCOPED_TRACE(trace_writes ? "trace_writes" : "Control: trace_writes off");
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<Seen> seen;
        MemoryProvider provider;
        Recorder recorder;
        SendspinServer server;
        ServerConfig config = sendspin::server::test::local_config();
        config.approve_all_clients = true;
        config.trace_writes = trace_writes;
        config.trace = [&](const sendspin::server::TracedMessage& message) {
            std::string message_type;
            if (!message.plaintext.empty() && (!message.binary || message.plaintext.front() == 0)) {
                const size_t offset = message.binary ? 1 : 0;
                JsonDocument doc;
                if (deserializeJson(doc, message.plaintext.data() + offset,
                                    message.plaintext.size() - offset) ==
                    DeserializationError::Ok) {
                    message_type = doc["type"].as<std::string>();
                }
            }
            {
                const std::lock_guard<std::mutex> lock(mutex);
                seen.push_back({message.written,
                                message.direction == sendspin::server::TraceDirection::INBOUND,
                                message_type, message.plaintext.empty()});
            }
            changed.notify_all();
        };
        ASSERT_EQ(server.start(config, &provider, &recorder), StartResult::OK);
        RawClient client;
        ASSERT_NO_FATAL_FAILURE(connect(server, client, hello("Timer", {"metadata@v1"}, true)));
        next(client, "group/update");
        client.send_json(sendspin::server::test::client_time_json(1));
        next(client, "server/time");
        client.send_json(sendspin::server::test::client_time_json(2));

        // Waits with no timeout for the second request; a regression hangs into the suite watchdog
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] {
            return std::count_if(seen.begin(), seen.end(), [](const Seen& entry) {
                       return entry.inbound && entry.type == "client/time";
                   }) == 2;
        });
        const auto second_request =
            std::find_if(
                seen.rbegin(), seen.rend(),
                [](const Seen& entry) { return entry.inbound && entry.type == "client/time"; })
                .base() -
            1;
        const auto reply = std::find_if(seen.begin(), seen.end(), [](const Seen& entry) {
            return !entry.inbound && entry.type == "server/time";
        });
        ASSERT_LT(reply, second_request);
        const auto reply_written =
            std::find_if(reply + 1, seen.end(), [](const Seen& entry) { return entry.written; });
        const size_t writes = static_cast<size_t>(std::count_if(
            seen.begin(), seen.end(), [](const Seen& entry) { return entry.written; }));
        if (!trace_writes) {
            EXPECT_EQ(writes, 0U);
            continue;
        }
        // The write invariant: no outbound message is taken between the reply and its write
        ASSERT_NE(reply_written, seen.end());
        EXPECT_TRUE(std::none_of(reply + 1, reply_written,
                                 [](const Seen& entry) { return !entry.inbound; }));
        EXPECT_LT(reply_written, second_request);
        EXPECT_FALSE(reply_written->inbound);
        EXPECT_TRUE(reply_written->empty);
    }
}

}  // namespace
