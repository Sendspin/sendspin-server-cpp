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

/// @file test_controller.cpp
/// @brief The controller's pure decisions as tables: its server/state object, the group volume
/// algorithm, command filtering, the seek checks and the switch cycle (spec controller/v1.md).

#include "controller.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

using sendspin::server::command_bit;
using sendspin::server::ControllerCaps;
using sendspin::server::ControllerCommand;
using sendspin::server::ControllerCommandSet;
using sendspin::server::ControllerRequest;
using sendspin::server::ControllerState;
using sendspin::server::GroupId;
using sendspin::server::PlayerLevels;
using sendspin::server::RepeatMode;
using sendspin::server::SwitchGroup;
using sendspin::server::SwitchTarget;
using sendspin::server::TransportCommand;

ControllerCommandSet commands(std::initializer_list<ControllerCommand> list) {
    ControllerCommandSet set = 0;
    for (const ControllerCommand command : list) {
        set |= command_bit(command);
    }
    return set;
}

// controller/v1.md "server/state controller object", "Reading group volume" and "Reading group
// mute": the host's commands, SEEK only with a seekable range; `volume` and `mute` only while a
// player supports them, 100 and false otherwise; the rounded average volume; muted only when
// every mute-supporting player is
TEST(ControllerState, FollowsThePlayersAndTheHostsCaps) {
    struct Row {
        const char* name;
        std::vector<PlayerLevels> players;
        ControllerCaps caps;
        ControllerCommandSet commands;
        uint32_t volume;
        bool muted;
        std::optional<uint32_t> seek_max_ms;
    };
    ControllerCaps seekable;
    seekable.commands = {TransportCommand::PLAY, TransportCommand::SEEK,
                         TransportCommand::SEEK_RELATIVE};
    seekable.seek_max_ms = 240000;
    ControllerCaps live = seekable;
    live.seek_max_ms.reset();
    using C = ControllerCommand;
    const Row rows[] = {
        {"Control: two players",
         {{30, false}, {61, true}},
         {},
         commands({C::SWITCH, C::VOLUME, C::MUTE}),
         46,
         false,
         std::nullopt},
        {"no player: volume 100, unmuted, neither offered",
         {},
         {},
         commands({C::SWITCH}),
         100,
         false,
         std::nullopt},
        {"players without volume or mute support",
         {{std::nullopt, std::nullopt}},
         {},
         commands({C::SWITCH}),
         100,
         false,
         std::nullopt},
        {"every mute-supporting player muted",
         {{40, true}, {std::nullopt, true}},
         {},
         commands({C::SWITCH, C::VOLUME, C::MUTE}),
         40,
         true,
         std::nullopt},
        {"one player without mute support",
         {{40, true}, {60, std::nullopt}},
         {},
         commands({C::SWITCH, C::VOLUME, C::MUTE}),
         50,
         true,
         std::nullopt},
        {"Control: a seekable range offers seek",
         {},
         seekable,
         commands({C::SWITCH, C::PLAY, C::SEEK, C::SEEK_RELATIVE}),
         100,
         false,
         240000},
        {"no seekable range: seek omitted, seek_relative kept",
         {},
         live,
         commands({C::SWITCH, C::PLAY, C::SEEK_RELATIVE}),
         100,
         false,
         std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ControllerState state = sendspin::server::controller_state(row.players, row.caps);
        EXPECT_EQ(state.supported_commands, row.commands);
        EXPECT_EQ(state.volume, row.volume);
        EXPECT_EQ(state.muted, row.muted);
        EXPECT_EQ(state.seek_max_ms, row.seek_max_ms);
    }
    ControllerCaps modes;
    modes.repeat = RepeatMode::ALL;
    modes.shuffle = true;
    const ControllerState state = sendspin::server::controller_state({}, modes);
    EXPECT_EQ(state.repeat, RepeatMode::ALL);
    EXPECT_TRUE(state.shuffle);
}

// controller/v1.md "Setting group volume": the delta applied to every player, what clamping
// loses spread over the players not clamped, pass after pass, then each final volume rounded to
// the nearest integer. The spec gives no worked numbers; these rows follow its steps.
TEST(GroupVolume, KeepsRelativeLevelsWithinTheBounds) {
    struct Row {
        const char* name;
        std::vector<uint32_t> volumes;
        uint32_t requested;
        std::vector<uint32_t> expected;
    };
    const Row rows[] = {
        {"Control: no clamping", {20, 40}, 50, {40, 60}},
        {"no players", {}, 70, {}},
        {"one clamped at 100, its loss to the other", {50, 90}, 100, {100, 100}},
        {"one clamped at 0, its loss to the other", {10, 90}, 0, {0, 0}},
        {"the delta already met", {0, 100}, 50, {0, 100}},
        {"the loss shared by two", {10, 20, 90}, 80, {65, 75, 100}},
        {"a second pass: one reaches 100 exactly, then clamps", {0, 60, 90}, 90, {70, 100, 100}},
        {"every player clamped", {95, 99, 50}, 100, {100, 100, 100}},
        {"halves round up", {33, 34}, 50, {50, 51}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::group_volumes(row.volumes, row.requested), row.expected);
    }
}

// controller/v1.md "client/command controller object": commands absent from the current
// supported_commands are ignored, whatever their parameters, and so is a seek outside 0 to
// seek_max_ms; an offered command with a parameter missing or invalid is a protocol error
TEST(ControllerCommands, JudgesWhatTheStateOffersFirst) {
    using sendspin::server::CommandVerdict;
    ControllerState state;
    state.supported_commands =
        commands({ControllerCommand::PLAY, ControllerCommand::SEEK, ControllerCommand::VOLUME});
    state.seek_max_ms = 1000;
    struct Row {
        const char* name;
        ControllerRequest request;
        CommandVerdict verdict;
    };
    const auto request = [](ControllerCommand command, bool malformed) {
        ControllerRequest out;
        out.command = command;
        out.malformed = malformed;
        return out;
    };
    const auto seek = [](int64_t position) {
        ControllerRequest out;
        out.command = ControllerCommand::SEEK;
        out.position_ms = position;
        return out;
    };
    ControllerRequest volume = request(ControllerCommand::VOLUME, false);
    volume.volume = 30;
    const Row rows[] = {
        {"Control: play", request(ControllerCommand::PLAY, false), CommandVerdict::ACCEPT},
        {"Control: volume", volume, CommandVerdict::ACCEPT},
        {"pause, not offered", request(ControllerCommand::PAUSE, false), CommandVerdict::IGNORE},
        {"mute not offered and malformed", request(ControllerCommand::MUTE, true),
         CommandVerdict::IGNORE},
        {"volume offered and malformed", request(ControllerCommand::VOLUME, true),
         CommandVerdict::PROTOCOL_ERROR},
        {"an unrecognized command", ControllerRequest{}, CommandVerdict::IGNORE},
        {"Control: seek to 0", seek(0), CommandVerdict::ACCEPT},
        {"Control: seek to seek_max_ms", seek(1000), CommandVerdict::ACCEPT},
        {"seek past seek_max_ms", seek(1001), CommandVerdict::IGNORE},
        {"seek below 0", seek(-1), CommandVerdict::IGNORE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::judge_command(row.request, state), row.verdict);
    }
}

// controller/v1.md: seek_relative applied best effort, the result clamped to the seekable range
TEST(RelativeSeek, ClampsToTheSeekableRange) {
    struct Row {
        const char* name;
        int64_t current;
        int64_t offset;
        std::optional<uint32_t> seek_max_ms;
        uint32_t expected;
    };
    constexpr int64_t MAX = std::numeric_limits<int64_t>::max();
    const Row rows[] = {
        {"Control: within the range", 10000, 5000, 60000, 15000},
        {"before the start", 10000, -15000, 60000, 0},
        {"past the end", 58000, 5000, 60000, 60000},
        {"no end known", 10000, 5000, std::nullopt, 15000},
        {"an offset that would overflow", MAX - 1, MAX, std::nullopt,
         std::numeric_limits<uint32_t>::max()},
        {"an offset that would underflow", -1, -MAX - 1, 60000, 0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::clamp_relative_seek(row.current, row.offset, row.seek_max_ms),
                  row.expected);
    }
}

// controller/v1.md "Switch command cycle": the previous group first while still in the solo group
// the client was moved to, then playing groups of several clients, then other players playing
// alone, then for a player a group of its own; the entry after the current group, wrapping
TEST(SwitchCycle, MovesToTheNextGroup) {
    const GroupId g1{1};
    const GroupId g2{2};
    const GroupId g3{3};
    const GroupId g4{4};
    // g1: two playing; g2: one playing; g3: one stopped; g4: two stopped
    const std::vector<SwitchGroup> groups = {
        {g1, 2, true}, {g2, 1, true}, {g3, 1, false}, {g4, 2, false}};
    struct Row {
        const char* name;
        std::vector<SwitchGroup> groups;
        GroupId current;
        std::optional<GroupId> previous;
        bool player;
        SwitchTarget expected;
    };
    const Row rows[] = {
        {"Control: from a playing shared group to a player playing alone",
         groups,
         g1,
         std::nullopt,
         true,
         {g2, false}},
        {"alone and playing: wraps to the first", groups, g2, std::nullopt, true, {g1, false}},
        {"alone and stopped: wraps to the first", groups, g3, std::nullopt, true, {g1, false}},
        {"a stopped shared group is not in the cycle", groups, g4, std::nullopt, true, {g1, false}},
        {"the previous group first", groups, g3, g4, true, {g4, false}},
        {"from the last shared group to a group of its own",
         {{g1, 2, true}},
         g1,
         std::nullopt,
         true,
         {std::nullopt, true}},
        {"no group of its own without the player role",
         {{g1, 2, true}},
         g1,
         std::nullopt,
         false,
         {std::nullopt, false}},
        {"without the player role, alone: to the first",
         groups,
         g3,
         std::nullopt,
         false,
         {g1, false}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const SwitchTarget target =
            sendspin::server::switch_target(row.groups, row.current, row.previous, row.player);
        EXPECT_EQ(target.group, row.expected.group);
        EXPECT_EQ(target.new_solo, row.expected.new_solo);
    }
}

}  // namespace
