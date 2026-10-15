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
/// @brief Approval and the controller and metadata roles end to end: the raw client checks the
/// exact messages, and in-process sendspin-cpp clients show what a client library makes of them.

#include "peer.h"
#include "raw_client.h"
#include "raw_roles.h"
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ApprovalScope;
using sendspin::server::CommandArgs;
using sendspin::server::ControllerCaps;
using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::GroupId;
using sendspin::server::Metadata;
using sendspin::server::MetadataProgress;
using sendspin::server::RepeatMode;
using sendspin::server::SendspinServer;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::TransportCommand;
using sendspin::server::test::AlwaysReady;
using sendspin::server::test::client_at;
using sendspin::server::test::client_id_of;
using sendspin::server::test::connect;
using sendspin::server::test::controller_command;
using sendspin::server::test::group_id;
using sendspin::server::test::hello;
using sendspin::server::test::LocalServer;
using sendspin::server::test::next;
using sendspin::server::test::payload;
using sendspin::server::test::player_state;
using sendspin::server::test::RawClient;
using sendspin::server::test::Recorder;
using sendspin::server::test::type;
namespace persistence_keys = sendspin::server::persistence_keys;

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
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.name = "Roles";
    ASSERT_EQ(s.start(config), StartResult::OK);
    const std::string default_group = group_id(*s.server.default_group());
    RawClient client;
    const std::string id = client_id_of(client);
    const std::string hello_json =
        hello("Kitchen", {"player@v1", "controller@v1", "metadata@v1"}, true);
    ASSERT_NO_FATAL_FAILURE(connect(s.server, client, hello_json));

    ASSERT_EQ(payload(next(client)), R"({"activities":[],"active_roles":[]})");
    JsonDocument update = next(client);
    ASSERT_EQ(type(update), "group/update");
    EXPECT_NE(update["payload"]["group_id"].as<std::string>(), default_group);
    s.recorder.wait_for("approval needed " + id);
    ASSERT_EQ(s.server.clients().size(), 1U);
    EXPECT_FALSE(s.server.clients()[0].approval);
    EXPECT_TRUE(s.server.clients()[0].unpaired_access);

    ASSERT_TRUE(s.server.approve(id, ApprovalScope::PLAYBACK));
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
    EXPECT_LE(state["payload"]["metadata"]["timestamp"].as<int64_t>(), s.server.now_us());
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
    EXPECT_FALSE(s.server.approve("not a client id", ApprovalScope::PLAYBACK));

    // The approval persists: a new run activates the client for playback at once
    s.server.stop();
    ASSERT_EQ(s.start(config), StartResult::OK);
    ASSERT_NO_FATAL_FAILURE(connect(s.server, client, hello_json));
    EXPECT_EQ(payload(next(client)),
              std::string(R"({"activities":["playback"],"active_roles":)") + ALL_ROLES + "}");
    // Connected once it takes the connection with its client/state
    client.send_json(player_state(true, {}, std::nullopt, std::nullopt));
    s.recorder.wait_for_count("connected " + id, 2);
    EXPECT_EQ(s.server.clients()[0].approval, ApprovalScope::PLAYBACK);

    // A revocation: no activities and no roles, and out of the server's group
    ASSERT_TRUE(s.server.revoke_approval(id));
    EXPECT_EQ(payload(next(client, "server/activate")), R"({"activities":[],"active_roles":[]})");
    update = next(client, "group/update");
    EXPECT_NE(update["payload"]["group_id"].as<std::string>(), group_id(*s.server.default_group()));
    s.recorder.wait_for_count("approval needed " + id, 2);
    // Saved through the provider, which the event after it shows is done: the server key, then
    // the approval and its revocation
    EXPECT_EQ(s.provider.saves, 3);
    EXPECT_EQ(s.provider.commits, 3);
    const std::vector<uint8_t>& blob = s.provider.blobs.at(persistence_keys::client_record_key(0));
    ASSERT_EQ(blob.size(), persistence_keys::CLIENT_RECORD_SIZE);
    EXPECT_EQ(blob.back(), 0);
    EXPECT_EQ(s.provider.blobs.count(persistence_keys::client_record_key(1)), 0U);
}

// pairing.md "Unpaired Access": a client without unpaired access stays at no activities, approved
// or not, and the host hears that it needs pairing rather than approval. An approval that cannot
// be saved is reported.
TEST(ServerApproval, HoldsAClientWithoutUnpairedAccess) {
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(s.start(config), StartResult::OK);
    s.provider.refuse_save = true;
    RawClient client;
    const std::string id = client_id_of(client);
    ASSERT_NO_FATAL_FAILURE(connect(s.server, client, hello("Locked", {"player@v1"}, false)));
    ASSERT_EQ(payload(next(client)), R"({"activities":[],"active_roles":[]})");
    s.recorder.wait_for("pairing required " + id);
    EXPECT_FALSE(s.recorder.contains("approval needed " + id));
    EXPECT_FALSE(s.server.clients()[0].unpaired_access);

    ASSERT_TRUE(s.server.approve(id, ApprovalScope::PLAYBACK));
    const sendspin::server::ServerError error = s.recorder.wait_for_error("");
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
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(s.start(config), StartResult::OK);
    const GroupId group = *s.server.default_group();
    RawClient client;
    const std::string id = client_id_of(client);
    ASSERT_NO_FATAL_FAILURE(connect(s.server, client, hello("Remote", {"controller@v1"}, true)));
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
    ASSERT_TRUE(s.server.set_controller_caps(group, caps));
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
    s.recorder.wait_for(relative);
    // Posted before the commands below are sent, so core takes it in first
    Metadata metadata;
    metadata.progress = MetadataProgress{10000, 0, 1000};
    ASSERT_TRUE(s.server.set_metadata(group, metadata, s.server.now_us()));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":100000})"));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":-15000})"));
    // Taken in while seek is not offered
    s.recorder.wait_for_count(relative, 3);

    caps.commands.push_back(TransportCommand::SEEK);
    ASSERT_TRUE(s.server.set_controller_caps(group, caps));
    EXPECT_EQ(payload(next(client)),
              R"({"controller":{"supported_commands":["play","switch","seek","seek_relative"],)"
              R"("volume":100,"muted":false,"repeat":"all","shuffle":true,"seek_max_ms":60000}})");
    client.send_json(controller_command(R"({"command":"seek","position_ms":60001})"));
    client.send_json(controller_command(R"({"command":"seek","position_ms":-1})"));
    client.send_json(controller_command(R"({"command":"seek","position_ms":60000})"));
    client.send_json(controller_command(R"({"command":"play"})"));
    s.recorder.wait_for("command " + group_id(group) + " " +
                        std::to_string(static_cast<int>(TransportCommand::PLAY)) + " " + id);

    // No seek_max_ms: the track's length bounds it
    ControllerCaps relative_only;
    relative_only.commands = {TransportCommand::SEEK_RELATIVE};
    ASSERT_TRUE(s.server.set_controller_caps(group, relative_only));
    next(client, "server/state");
    metadata.progress = MetadataProgress{10000, 30000, 1000};
    ASSERT_TRUE(s.server.set_metadata(group, metadata, s.server.now_us()));
    client.send_json(controller_command(R"({"command":"seek_relative","offset_ms":100000})"));
    s.recorder.wait_for_count(relative, 4);

    const std::vector<std::pair<TransportCommand, CommandArgs>> commands = s.recorder.commands();
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
    s.recorder.wait_for("disconnected " + id + " " +
                        std::to_string(static_cast<int>(DisconnectReason::CLOSED_BY_SERVER)));
}

// controller/v1.md "Setting group volume" and "Setting group mute": one command per player that
// supports it, computed from the volumes the players report; a player that reports a volume it
// does not offer to set is left out of the group's volume and gets no command
// (roles/player/v1.md "Supported commands")
TEST(ServerController, SetsGroupVolumeAndMuteOnThePlayers) {
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    config.trace = [&s](const sendspin::server::TracedMessage& message) {
        s.recorder.trace(message);
    };
    ASSERT_EQ(s.start(config), StartResult::OK);
    RawClient a;
    RawClient b;
    RawClient fixed;
    ASSERT_NO_FATAL_FAILURE(connect(s.server, a, hello("A", {"player@v1", "controller@v1"}, true)));
    next(a, "server/state");
    ASSERT_NO_FATAL_FAILURE(connect(s.server, b, hello("B", {"player@v1"}, true)));
    next(b, "group/update");
    ASSERT_NO_FATAL_FAILURE(connect(s.server, fixed, hello("Fixed", {"player@v1"}, true)));
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
    s.recorder.wait_for_count("in client/state", 3);

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
    ASSERT_TRUE(s.server.set_group_volume(*s.server.default_group(), 30));
    EXPECT_EQ(payload(next(a, "server/command")), R"({"player":{"command":"volume","volume":10}})");
    EXPECT_EQ(payload(next(b, "server/command")), R"({"player":{"command":"volume","volume":50}})");
    EXPECT_FALSE(s.server.set_group_volume(*s.server.default_group(), 101));
    EXPECT_FALSE(s.server.set_group_volume(GroupId{999}, 30));
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
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(s.start(config), StartResult::OK);
    const GroupId group = *s.server.default_group();
    Metadata current;
    current.title = "A";
    current.progress = MetadataProgress{1000, 100000, 1000};
    const int64_t set_at = s.server.now_us();
    ASSERT_TRUE(s.server.set_metadata(group, current, set_at));
    Metadata scheduled;
    scheduled.title = "B";
    constexpr int64_t AHEAD_US = 10000000;
    ASSERT_TRUE(s.server.set_metadata(group, scheduled, set_at + AHEAD_US));

    RawClient client;
    ASSERT_NO_FATAL_FAILURE(
        connect(s.server, client, hello("Display", {"controller@v1", "metadata@v1"}, true)));
    ASSERT_EQ(type(next(client)), "server/activate");
    ASSERT_EQ(type(next(client)), "group/update");
    JsonDocument state = next(client);
    ASSERT_EQ(type(state), "server/state");
    EXPECT_EQ(state["payload"]["metadata"]["title"], "A");
    EXPECT_EQ(state["payload"]["metadata"]["timestamp"].as<int64_t>(), set_at);
    bool toggle = true;
    EXPECT_EQ(metadata_until_caps(s.server, group, client, toggle),
              std::vector<std::string>{R"({"timestamp":)" + std::to_string(set_at + AHEAD_US) +
                                       R"(,"title":"B"})"});

    sendspin::server::InputOptions options;
    options.format = {48000, 2, sendspin::server::SampleFormat::S16};
    const std::shared_ptr<sendspin::server::GroupInput> input =
        s.server.open_input(group, options).input;
    ASSERT_TRUE(input);
    const std::vector<uint8_t> silence(size_t{48000} * 4, 0);
    while (input->write(silence.data(), 48000).frames == 0) {
        input->wait_writable(std::numeric_limits<int64_t>::max());
    }
    const std::string name = "group " + std::to_string(group.value);
    s.recorder.wait_for(name + " playing");
    const size_t stops = s.recorder.count(name + " stopped");
    input->pause();
    // The run's end queues the state in the handler that reports the group stopped
    s.recorder.wait_for_count(name + " stopped", stops + 1);
    std::vector<std::string> states = metadata_until_caps(s.server, group, client, toggle);
    ASSERT_EQ(states.size(), 1U);
    JsonDocument paused;
    deserializeJson(paused, states[0]);
    EXPECT_EQ(paused["title"], "A");
    EXPECT_EQ(paused["progress"]["playback_speed"], 0);
    const uint32_t position = paused["progress"]["track_progress"].as<uint32_t>();
    EXPECT_GE(position, 1000U);
    const int64_t paused_at = paused["timestamp"].as<int64_t>();

    const size_t plays = s.recorder.count(name + " playing");
    input->resume();
    s.recorder.wait_for_count(name + " playing", plays + 1);
    states = metadata_until_caps(s.server, group, client, toggle);
    ASSERT_EQ(states.size(), 1U);
    JsonDocument resumed;
    deserializeJson(resumed, states[0]);
    EXPECT_EQ(resumed["title"], "A");
    EXPECT_EQ(resumed["progress"]["playback_speed"], 1000);
    EXPECT_EQ(resumed["progress"]["track_progress"].as<uint32_t>(), position);
    EXPECT_GT(resumed["timestamp"].as<int64_t>(), paused_at);
}

// ==============================================================================
// In-process sendspin-cpp clients
// ==============================================================================

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
        : port_(sendspin::server::test::free_port()),
          client_(sendspin::server::test::peer_config(name, this->port_)) {
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
        return sendspin::server::test::peer_url(this->port_);
    }

    void loop() {
        this->client_.loop();
    }

    Events events;
    sendspin::PlayerRole* player{nullptr};
    sendspin::ControllerRole* controller{nullptr};

private:
    uint16_t port_;
    AlwaysReady network_;
    sendspin::SendspinClient client_;
};

/// Runs each peer's main loop and step on this thread until done() holds, with no timeout
template <typename Done, typename Step>
void run_until(const std::vector<Peer*>& peers, Done done, Step step) {
    sendspin::server::test::poll_until(
        [&] {
            for (Peer* peer : peers) {
                peer->loop();
            }
            step();
        },
        done);
}

template <typename Done>
void run_until(const std::vector<Peer*>& peers, Done done) {
    run_until(peers, done, [] {});
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
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.trace = [&s](const sendspin::server::TracedMessage& message) {
        s.recorder.trace(message);
    };
    ASSERT_EQ(s.start(config), StartResult::OK);
    const GroupId group = *s.server.default_group();
    Peer peer("Kitchen", true, 40);
    ASSERT_TRUE(s.server.connect_url(peer.url()));
    std::string id;
    run_until({&peer}, [&] {
        id = client_at(s.server, peer.url());
        return !id.empty() && s.recorder.contains("approval needed " + id);
    });

    sendspin::server::InputOptions options;
    options.format = {48000, 2, sendspin::server::SampleFormat::S16};
    const std::shared_ptr<sendspin::server::GroupInput> input =
        s.server.open_input(group, options).input;
    ASSERT_TRUE(input);
    const std::vector<uint8_t> silence(size_t{960} * 4, 0);
    const auto write = [&] { input->write(silence.data(), 960); };
    const std::string playing = "group " + std::to_string(group.value) + " playing";
    run_until({&peer}, [&] { return s.recorder.contains(playing); }, write);
    EXPECT_EQ(peer.events.starts, 0);
    EXPECT_FALSE(peer.events.controller);

    ASSERT_TRUE(s.server.approve(id, ApprovalScope::PLAYBACK));
    run_until(
        {&peer},
        [&] {
            return peer.events.starts == 1 && peer.events.controller && peer.events.metadata &&
                   peer.events.controller->volume == 40;
        },
        write);
    EXPECT_TRUE(peer.events.offers(sendspin::SendspinControllerCommand::VOLUME));

    ASSERT_TRUE(s.server.revoke_approval(id));
    run_until(
        {&peer},
        [&] { return peer.events.ends >= 1 && !peer.events.controller && !peer.events.metadata; },
        write);
    const std::vector<Recorder::Entry> entries = s.recorder.entries();
    const std::ptrdiff_t end = last_index(entries, "out stream/end");
    ASSERT_GE(end, 0);
    EXPECT_LT(end, last_index(entries, "out server/activate"));
}

// controller/v1.md with sendspin-cpp clients: the controller shows the players' average volume,
// its volume command sets each player's, a transport command the host offers reaches it, and the
// metadata role shows the group's metadata, a scheduled update included (metadata/v1.md)
TEST(ServerRolesInProcess, ControllerAndMetadataReachASendspinCppClient) {
    LocalServer s;
    ServerConfig config = sendspin::server::test::local_config();
    config.approve_all_clients = true;
    ASSERT_EQ(s.start(config), StartResult::OK);
    const GroupId group = *s.server.default_group();
    Peer a("A", true, 20);
    Peer b("B", false, 80);
    ASSERT_TRUE(s.server.connect_url(a.url()));
    ASSERT_TRUE(s.server.connect_url(b.url()));
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
    ASSERT_TRUE(s.server.set_controller_caps(group, caps));
    run_until(peers, [&] { return a.events.offers(sendspin::SendspinControllerCommand::PLAY); });
    ASSERT_TRUE(a.controller->send_command({.command = sendspin::SendspinControllerCommand::PLAY}));
    const std::string played = "command " + std::to_string(group.value) + " " +
                               std::to_string(static_cast<int>(TransportCommand::PLAY)) + " " +
                               client_at(s.server, a.url());
    run_until(peers, [&] { return s.recorder.contains(played); });

    Metadata now;
    now.title = "Now";
    ASSERT_TRUE(s.server.set_metadata(group, now, s.server.now_us()));
    run_until(peers, [&] { return a.events.metadata && a.events.metadata->title == "Now"; });
    Metadata next_track;
    next_track.title = "Next";
    constexpr int64_t AHEAD_US = 300000;
    ASSERT_TRUE(s.server.set_metadata(group, next_track, s.server.now_us() + AHEAD_US));
    run_until(peers, [&] { return a.events.metadata && a.events.metadata->title == "Next"; });
}
}  // namespace
