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

/// @file controller.h
/// @brief A group's controller decisions over plain data (spec controller/v1.md): its
/// server/state controller object, the group volume algorithm, command filtering, the seek
/// checks and the switch cycle. Pure functions.

#pragma once

#include "messages.h"
#include "sendspin/server/types.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief One player member's volume and mute as its latest client/state reports them, each set
/// only when its supported_commands offers setting it (roles/player/v1.md "Supported commands")
struct PlayerLevels {
    std::optional<uint32_t> volume;
    std::optional<bool> muted;
};

/// @brief The group's controller object: the host's commands (SEEK only with seek_max_ms),
/// `volume` and `mute` while a player supports them, and `switch`; the volume is the rounded
/// average of the players that support volume, else 100; muted only when every player that
/// supports mute is muted
ControllerState controller_state(std::span<const PlayerLevels> players, const ControllerCaps& caps);

/// @brief The controller command a TransportCommand is
ControllerCommand controller_command(TransportCommand command);

/// @brief The TransportCommand a controller command is; nullopt for `volume`, `mute` and
/// `switch`, which the server carries out
std::optional<TransportCommand> transport_command(ControllerCommand command);

enum class CommandVerdict : uint8_t {
    ACCEPT,
    IGNORE,
    /// An offered command with its parameter missing or invalid
    PROTOCOL_ERROR,
};

/// @brief What the group does with request (controller/v1.md "client/command controller
/// object"): it ignores a command absent from the current supported_commands before looking at
/// its parameter, and a SEEK outside 0 to seek_max_ms
CommandVerdict judge_command(const ControllerRequest& request, const ControllerState& state);

/// @brief Each player's volume after a group volume of requested: the players' common delta,
/// what clamping at 0 or 100 loses spread over the players not clamped until none is lost or
/// all are clamped, then each rounded to the nearest integer, halves up (controller/v1.md
/// "Setting group volume")
/// @param volumes the volumes of the players that support volume, each at most 100
std::vector<uint32_t> group_volumes(std::span<const uint32_t> volumes, uint32_t requested);

/// @brief The target of a seek_relative: current_ms plus offset_ms, clamped to 0 and, when
/// known, the end of the seekable range
uint32_t clamp_relative_seek(int64_t current_ms, int64_t offset_ms,
                             std::optional<uint32_t> seek_max_ms);

/// @brief One group as the switch cycle sees it
struct SwitchGroup {
    GroupId id;
    size_t members{0};
    bool playing{false};
};

/// @brief Where a switch moves a client: a group, or a new group of its own
struct SwitchTarget {
    std::optional<GroupId> group;
    bool new_solo{false};
};

/// @brief The next group in the client's switch cycle (controller/v1.md "Switch command
/// cycle"): its previous group first while it is still in the solo group it was moved to, then
/// the playing groups of several clients, then the playing groups of one other client, then,
/// for a player, a group of its own. The entry after its current group, wrapping, or the first
/// when its group is not in the cycle; nullopt group and no new_solo when that is where it is.
/// @param groups every group, in a stable order
/// @param previous set only while the client is in the solo group it was moved to
SwitchTarget switch_target(std::span<const SwitchGroup> groups, GroupId current,
                           std::optional<GroupId> previous, bool has_player);

}  // namespace sendspin::server
