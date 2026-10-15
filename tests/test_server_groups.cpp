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

/// @file test_server_groups.cpp
/// @brief The host's groups end to end against raw clients: a client's group across a reconnect
/// within the rejoin grace, and the groups the host creates, renames and moves clients between.

#include "raw_client.h"
#include "raw_roles.h"
#include "test_recorder.h"
#include "wire_messages.h"
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <sendspin/server/server.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sendspin::server::ControllerCaps;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::Metadata;
using sendspin::server::ServerConfig;
using sendspin::server::StartResult;
using sendspin::server::StreamCodec;
using sendspin::server::StreamFormat;
using sendspin::server::TransportCommand;
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
using sendspin::server::test::type;
namespace wire = sendspin::server::test::wire;

// ==============================================================================
// A client's group across a reconnect
// ==============================================================================

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
        LocalServer s;
        ServerConfig config = sendspin::server::test::local_config();
        config.approve_all_clients = true;
        if (leaves == Leaves::PAST) {
            config.timing.group_rejoin_grace_us = 1;
        }
        ASSERT_EQ(s.start(config), StartResult::OK);
        const std::string default_group = group_id(*s.server.default_group());
        RawClient a;
        RawClient b;
        const std::string hello_json = hello("A", {"player@v1"}, true);
        ASSERT_NO_FATAL_FAILURE(connect(s.server, a, hello_json));
        EXPECT_EQ(next(a, "group/update")["payload"]["group_id"].as<std::string>(), default_group);
        ASSERT_NO_FATAL_FAILURE(connect(s.server, b, hello("B", {"player@v1"}, true)));
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
        s.recorder.wait_for_disconnects(1);
        const auto has_solo = [&] {
            const std::vector<GroupInfo> groups = s.server.groups();
            return std::any_of(groups.begin(), groups.end(),
                               [&](const GroupInfo& group) { return group_id(group.id) == solo; });
        };
        if (leaves == Leaves::PAST) {
            while (has_solo()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        ASSERT_NO_FATAL_FAILURE(connect(s.server, a, hello_json));
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
class GroupsServer : public LocalServer {
public:
    GroupsServer() {
        ServerConfig config = sendspin::server::test::local_config();
        config.name = "Living room";
        config.approve_all_clients = true;
        EXPECT_EQ(this->start(config), StartResult::OK);
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
        if (plaintext->empty() || plaintext->front() != wire::JSON_ID) {
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
        if (plaintext->empty() || plaintext->front() != wire::JSON_ID) {
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

}  // namespace
