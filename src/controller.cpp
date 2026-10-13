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

#include "controller.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace sendspin::server {

namespace {

constexpr double VOLUME_MAX = MAX_VOLUME;

}  // namespace

ControllerState controller_state(std::span<const PlayerLevels> players,
                                 const ControllerCaps& caps) {
    ControllerState state;
    for (const TransportCommand command : caps.commands) {
        if (command == TransportCommand::SEEK && !caps.seek_max_ms) {
            // controller/v1.md: 'seek' is omitted while the seekable range is unknown
            continue;
        }
        state.supported_commands |= command_bit(controller_command(command));
    }
    state.supported_commands |= command_bit(ControllerCommand::SWITCH);
    if ((state.supported_commands & command_bit(ControllerCommand::SEEK)) != 0) {
        state.seek_max_ms = caps.seek_max_ms;
    }
    state.repeat = caps.repeat;
    state.shuffle = caps.shuffle;

    // controller/v1.md "Reading group volume" and "Reading group mute"
    uint64_t volume_sum = 0;
    uint32_t volume_count = 0;
    uint32_t mute_count = 0;
    bool all_muted = true;
    for (const PlayerLevels& player : players) {
        if (player.volume) {
            volume_sum += *player.volume;
            ++volume_count;
        }
        if (player.muted) {
            ++mute_count;
            all_muted = all_muted && *player.muted;
        }
    }
    if (volume_count > 0) {
        state.supported_commands |= command_bit(ControllerCommand::VOLUME);
        // Rounded to the nearest integer, halves up
        state.volume =
            static_cast<uint32_t>((volume_sum * 2 + volume_count) / (uint64_t{2} * volume_count));
    }
    if (mute_count > 0) {
        state.supported_commands |= command_bit(ControllerCommand::MUTE);
        state.muted = all_muted;
    }
    return state;
}

ControllerCommand controller_command(TransportCommand command) {
    switch (command) {
        case TransportCommand::PLAY:
            return ControllerCommand::PLAY;
        case TransportCommand::PAUSE:
            return ControllerCommand::PAUSE;
        case TransportCommand::STOP:
            return ControllerCommand::STOP;
        case TransportCommand::NEXT:
            return ControllerCommand::NEXT;
        case TransportCommand::PREVIOUS:
            return ControllerCommand::PREVIOUS;
        case TransportCommand::REPEAT_OFF:
            return ControllerCommand::REPEAT_OFF;
        case TransportCommand::REPEAT_ONE:
            return ControllerCommand::REPEAT_ONE;
        case TransportCommand::REPEAT_ALL:
            return ControllerCommand::REPEAT_ALL;
        case TransportCommand::SHUFFLE:
            return ControllerCommand::SHUFFLE;
        case TransportCommand::UNSHUFFLE:
            return ControllerCommand::UNSHUFFLE;
        case TransportCommand::SEEK:
            return ControllerCommand::SEEK;
        case TransportCommand::SEEK_RELATIVE:
            break;
    }
    return ControllerCommand::SEEK_RELATIVE;
}

std::optional<TransportCommand> transport_command(ControllerCommand command) {
    switch (command) {
        case ControllerCommand::PLAY:
            return TransportCommand::PLAY;
        case ControllerCommand::PAUSE:
            return TransportCommand::PAUSE;
        case ControllerCommand::STOP:
            return TransportCommand::STOP;
        case ControllerCommand::NEXT:
            return TransportCommand::NEXT;
        case ControllerCommand::PREVIOUS:
            return TransportCommand::PREVIOUS;
        case ControllerCommand::REPEAT_OFF:
            return TransportCommand::REPEAT_OFF;
        case ControllerCommand::REPEAT_ONE:
            return TransportCommand::REPEAT_ONE;
        case ControllerCommand::REPEAT_ALL:
            return TransportCommand::REPEAT_ALL;
        case ControllerCommand::SHUFFLE:
            return TransportCommand::SHUFFLE;
        case ControllerCommand::UNSHUFFLE:
            return TransportCommand::UNSHUFFLE;
        case ControllerCommand::SEEK:
            return TransportCommand::SEEK;
        case ControllerCommand::SEEK_RELATIVE:
            return TransportCommand::SEEK_RELATIVE;
        case ControllerCommand::VOLUME:
        case ControllerCommand::MUTE:
        case ControllerCommand::SWITCH:
            break;
    }
    return std::nullopt;
}

CommandVerdict judge_command(const ControllerRequest& request, const ControllerState& state) {
    // controller/v1.md: commands absent from the current supported_commands are ignored
    if (!request.command || (state.supported_commands & command_bit(*request.command)) == 0) {
        return CommandVerdict::IGNORE;
    }
    if (request.malformed) {
        return CommandVerdict::PROTOCOL_ERROR;
    }
    if (*request.command == ControllerCommand::SEEK) {
        // controller/v1.md: a seek outside 0 to seek_max_ms is ignored
        const bool within = request.position_ms && state.seek_max_ms && *request.position_ms >= 0 &&
                            *request.position_ms <= int64_t{*state.seek_max_ms};
        return within ? CommandVerdict::ACCEPT : CommandVerdict::IGNORE;
    }
    return CommandVerdict::ACCEPT;
}

std::vector<uint32_t> group_volumes(std::span<const uint32_t> volumes, uint32_t requested) {
    std::vector<double> levels(volumes.begin(), volumes.end());
    std::vector<uint32_t> result;
    if (levels.empty()) {
        return result;
    }
    const double average =
        std::accumulate(levels.begin(), levels.end(), 0.0) / static_cast<double>(levels.size());
    double delta = static_cast<double>(requested) - average;
    std::vector<size_t> active(levels.size());
    std::iota(active.begin(), active.end(), size_t{0});
    // Each pass that clamps no player has applied the whole delta; any other clamps at least one
    // and leaves it out, so the loop ends within one pass per player
    while (!active.empty()) {
        double lost = 0.0;
        std::vector<size_t> unclamped;
        for (const size_t i : active) {
            const double proposed = levels[i] + delta;
            const double clamped = std::clamp(proposed, 0.0, VOLUME_MAX);
            lost += proposed - clamped;
            levels[i] = clamped;
            if (clamped == proposed) {
                unclamped.push_back(i);
            }
        }
        if (unclamped.size() == active.size() || unclamped.empty()) {
            break;
        }
        delta = lost / static_cast<double>(unclamped.size());
        active = std::move(unclamped);
    }
    result.reserve(levels.size());
    for (const double level : levels) {
        // controller/v1.md: each final volume rounded to the nearest integer
        result.push_back(static_cast<uint32_t>(std::floor(level + 0.5)));
    }
    return result;
}

uint32_t clamp_relative_seek(int64_t current_ms, int64_t offset_ms,
                             std::optional<uint32_t> seek_max_ms) {
    // Saturating, so an offset near the type's range cannot wrap
    int64_t target = current_ms;
    if (offset_ms > 0 && target > INT64_MAX - offset_ms) {
        target = INT64_MAX;
    } else if (offset_ms < 0 && target < INT64_MIN - offset_ms) {
        target = INT64_MIN;
    } else {
        target += offset_ms;
    }
    const int64_t upper = seek_max_ms ? int64_t{*seek_max_ms} : int64_t{UINT32_MAX};
    return static_cast<uint32_t>(std::clamp<int64_t>(target, 0, upper));
}

SwitchTarget switch_target(std::span<const SwitchGroup> groups, GroupId current,
                           std::optional<GroupId> previous, bool has_player) {
    // Each entry: a group, or nullopt for a new group of the client's own
    std::vector<std::optional<GroupId>> cycle;
    const auto add = [&cycle](std::optional<GroupId> entry) {
        if (std::find(cycle.begin(), cycle.end(), entry) == cycle.end()) {
            cycle.push_back(entry);
        }
    };
    const auto find = [&groups](GroupId id) {
        return std::find_if(groups.begin(), groups.end(),
                            [id](const SwitchGroup& group) { return group.id == id; });
    };
    if (previous && find(*previous) != groups.end()) {
        add(previous);
    }
    for (const SwitchGroup& group : groups) {
        if (group.playing && group.members > 1) {
            add(group.id);
        }
    }
    for (const SwitchGroup& group : groups) {
        if (group.playing && group.members == 1 && group.id != current) {
            add(group.id);
        }
    }
    const auto own = find(current);
    const bool alone = own != groups.end() && own->members == 1;
    if (has_player) {
        add(alone ? std::optional<GroupId>(current) : std::nullopt);
    }
    if (cycle.empty()) {
        return {};
    }
    const auto at = std::find(cycle.begin(), cycle.end(), std::optional<GroupId>(current));
    const auto next_at = at == cycle.end() || at + 1 == cycle.end() ? cycle.begin() : at + 1;
    const std::optional<GroupId>& next = *next_at;
    if (next == current) {
        return {};
    }
    return next ? SwitchTarget{next, false} : SwitchTarget{std::nullopt, true};
}

}  // namespace sendspin::server
