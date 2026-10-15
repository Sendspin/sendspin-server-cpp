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

/// @file types.h
/// @brief Plain value types the send path's functions share: ids, formats, roles and the input and
/// drain states

#pragma once

#include "sendspin/server/types.h"

#include <compare>
#include <cstddef>
#include <cstdint>

namespace sendspin::server::sched {

/// @brief A server-unique id; 0 is none. Ids come from server-wide counters, so ids from two groups
/// never compare equal.
template <typename Tag>
struct Id {
    uint64_t value{0};

    constexpr bool valid() const {
        return this->value != 0;
    }

    friend constexpr auto operator<=>(const Id&, const Id&) = default;
};

using RunId = Id<struct RunTag>;
using EpochId = Id<struct EpochTag>;
/// @brief A pipeline instance; never reused, so frames of a removed instance are told apart from
/// those of its replacement
using InstanceId = Id<struct InstanceTag>;

/// @brief A player or source stream's codec, the one enum the wire layer (messages.h) and the send
/// path share
enum class Codec : uint8_t { PCM, FLAC, OPUS };

/// @brief A player audio format (spec player/v1.md "client/hello player@v1 support object")
struct AudioFormat {
    Codec codec{Codec::PCM};
    uint32_t sample_rate{0};
    uint8_t channels{0};
    /// Ignored for Opus, by comparison too (spec player/v1.md "client/hello player@v1 support
    /// object")
    uint8_t bit_depth{0};

    constexpr bool operator==(const AudioFormat& other) const {
        return this->codec == other.codec && this->sample_rate == other.sample_rate &&
               this->channels == other.channels &&
               (this->codec == Codec::OPUS || this->bit_depth == other.bit_depth);
    }
};

/// @brief What a stream's `stream/start` announced: a player's audio format, or a visualizer's
/// configuration
struct FormatKey {
    AudioFormat audio;
    /// Identifies a visualizer stream configuration; 0 for a player
    uint64_t visualizer_config{0};

    bool operator==(const FormatKey&) const = default;
};

using InputKind = ::sendspin::server::InputKind;

/// @brief What caused a trigger: a group event, or an event of one session
enum class TriggerKind : uint8_t { GROUP, JOIN };

/// @brief The roles whose streams the cursors carry
enum class StreamRole : uint8_t { PLAYER, VISUALIZER };
constexpr size_t STREAM_ROLE_COUNT = 2;

constexpr size_t role_index(StreamRole role) {
    return static_cast<size_t>(role);
}

/// @brief A set of stream roles, as listed by an activation or by a stream/* message
struct RoleSet {
    uint8_t bits{0};

    static constexpr RoleSet of(StreamRole role) {
        return {static_cast<uint8_t>(1U << role_index(role))};
    }
    constexpr bool contains(StreamRole role) const {
        return (this->bits & RoleSet::of(role).bits) != 0;
    }
    constexpr RoleSet with(StreamRole role) const {
        return {static_cast<uint8_t>(this->bits | RoleSet::of(role).bits)};
    }
    constexpr bool empty() const {
        return this->bits == 0;
    }

    bool operator==(const RoleSet&) const = default;
};

/// @brief The public enums the send path decides
using DrainOutcome = ::sendspin::server::DrainOutcome;
using InputState = ::sendspin::server::InputState;

/// @brief An input's run as the host view sees it
enum class RunState : uint8_t {
    /// No run: nothing written since the input opened, a stop or a completed drain
    NONE,
    /// A flush began an epoch that no write has anchored
    PENDING,
    PLAYING,
    PAUSED,
};

}  // namespace sendspin::server::sched
