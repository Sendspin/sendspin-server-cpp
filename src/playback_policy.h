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

/// @file playback_policy.h
/// @brief Playback decisions over plain data: a client's activation, a player's stream format,
/// the group parameters its eligible players give, and what a host's move does with a client.
/// Pure functions.

#pragma once

#include "group_params.h"
#include "messages.h"
#include "sched/constants.h"
#include "sched/leads.h"
#include "sched/types.h"
#include "sendspin/server/types.h"
#include "trust_policy.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief The role versions this server implements, one per family
constexpr std::string_view PLAYER_ROLE = "player@v1";
constexpr std::string_view CONTROLLER_ROLE = "controller@v1";
constexpr std::string_view METADATA_ROLE = "metadata@v1";
constexpr std::string_view ARTWORK_ROLE = "artwork@v1";
constexpr std::string_view COLOR_ROLE = "color@v1";
constexpr std::string_view VISUALIZER_ROLE = "visualizer@v1";
constexpr std::string_view SOURCE_ROLE = "source@v1";
/// @brief The player and visualizer roles' names in stream/* messages
constexpr std::string_view PLAYER_STREAM_ROLE = "player";
constexpr std::string_view VISUALIZER_STREAM_ROLE = "visualizer";

/// @brief What the host is told about a client held with no activities
enum class ActivationNotice : uint8_t {
    NONE,
    /// Unpaired access is on and the operator has not approved the client
    APPROVAL_NEEDED,
    /// Unpaired access is off, so only pairing lets the client play
    PAIRING_REQUIRED,
    /// The client could not use the long-term PSK the server holds for it and completed the
    /// handshake under the Sentinel (connection.md "Sentinel Fallback")
    CREDENTIAL_MISMATCH,
    /// The client could not use the pairing PSK the operator gave for it and completed the
    /// handshake under the Sentinel
    PAIRING_PSK_MISMATCH,
};

struct ActivationDecision {
    ServerActivate activation;
    ActivationNotice notice{ActivationNotice::NONE};
    /// The session may play: it gets every implemented role, and `playback` whenever asked
    bool playable{false};
};

/// @brief The roles to activate on a session that may play: each listed role this server
/// implements, in the client's order, the player, the visualizer and the source only with their
/// support objects (README "Priority and Activation", messaging.md "client/hello")
std::vector<std::string> implemented_roles(const ClientHello& hello);

/// @brief A session's activation. Under a long-term PSK the session may play: every implemented
/// role, and `['playback']` when playback is set, `[]` otherwise. Under the Sentinel or a pairing
/// PSK the session is unpaired: it may play only while it is playback-capable, which needs unpaired
/// access enabled (messaging.md "server/activate"), and with the operator's approval (pairing.md
/// "Unpaired Access"); never while the server holds a long-term PSK for the client (connection.md
/// "Sentinel Fallback"), nor after the client failed the pairing PSK it was referenced under.
/// Otherwise no activities and an explicitly empty `active_roles`, so persisted roles do not carry
/// over. An unpaired session gets the source role only with an approval whose scope covers it
/// (roles/source/v1.md "Unpaired access").
/// @param paired whether the server holds a long-term PSK for the client
/// @param playback whether the activation declares `playback` when the session may play
/// (first_activation_playback(), later_activation_playback())
ActivationDecision decide_activation(const PskMatch& psk, bool paired, const ClientHello& hello,
                                     std::optional<ApprovalScope> approval, bool playback);

/// @brief The send path's form of a client's format; nullopt when a field does not fit it
std::optional<sched::AudioFormat> to_sched(const AudioFormat& format);
AudioFormat from_sched(const sched::AudioFormat& format);
/// @brief A format as the host sees it
StreamFormat stream_format(const sched::AudioFormat& format);
/// @brief A host's format as a client lists one
AudioFormat client_format(const StreamFormat& format);

/// @brief Whether format selection offers format's channel count for input: the input's, or one
/// channel_mix_supported() offers. The resampler takes any rate to any other.
bool pipeline_converts(const InputFormat& input, const sched::AudioFormat& format);

/// @brief The format of a player's next stream (spec player/v1.md "client/hello player@v1
/// support object"): the operator's format when producible, else the player's preferred format
/// when producible, else its first producible entry at the input's rate, which needs no
/// resampler, else its first producible entry; nullopt when none is. Producible:
/// pipeline_converts() and a shape output_shape() has an encoder for, Opus only when opus is set.
/// @param preferred the player's client/state `format`
/// @param excluded formats not to choose: those whose pipeline failed
/// @param operator_format the operator's format (SendspinServer::set_player_format())
/// @param opus whether Opus may be chosen (ServerConfig::enable_opus)
std::optional<sched::AudioFormat> select_player_format(
    const PlayerSupport& support, const std::optional<AudioFormat>& preferred,
    const InputFormat& input, std::span<const sched::AudioFormat> excluded = {},
    const std::optional<AudioFormat>& operator_format = std::nullopt, bool opus = false);

/// @brief A player's timing values from its client/state, as the send path takes them
sched::PlayerTiming player_timing(const PlayerState& state);

/// @brief A PCM stream's bytes per second at format's rate, channels and depth, counting a chunk
/// header per frame: `B_i` for PCM, and for FLAC the PCM rate of the same format, which bounds
/// FLAC's from above and so keeps `W` conservative; for Opus, its largest packet per frame
int64_t pcm_bytes_per_second(const sched::AudioFormat& format);

/// @brief What one eligible member contributes to the group parameters
struct EligiblePlayer {
    sched::PlayerTiming timing;
    int64_t capacity_bytes{0};
    sched::AudioFormat format;
    /// `L_p` of the pipeline producing its format (sched::pipeline_latency_us()); 0 for a
    /// buffered input
    int64_t pipeline_latency_us{0};
};

/// @brief `G`, `od_max` and `W` over the eligible players: a live input's leads leave out
/// `required_lead_time_ms`, add each pipeline's latency and take `W_live`
GroupParams group_params(std::span<const EligiblePlayer> players, const sched::SchedTiming& timing,
                         InputKind kind);

/// @brief What a host's move of a client into a group does (SendspinServer::move_client(),
/// create_group())
enum class MoveAction : uint8_t {
    NONE,
    /// The connection leaves its group for the target, ending its streams there and joining the
    /// target's playback
    MOVE,
    /// The client is not connected: the place held for its rejoin grace moves to the target, so it
    /// rejoins there
    MOVE_HELD,
};

/// @brief What the server knows of the client a move names
struct MoveFacts {
    /// The connection is activated and in a group
    bool connected{false};
    /// Its activations carry the client's roles (ActivationDecision::playable); a client held with
    /// no roles stays in a group of its own
    bool playable{false};
    /// It is in the target already
    bool in_target{false};
    /// No connection: the server holds the client's place in a group
    bool held{false};
    /// That place is in the target already
    bool held_in_target{false};
};

/// @brief A host's move: a connected client that may play moves; a client not connected moves the
/// place held for it; anything else, or a client already in the target, changes nothing
MoveAction move_action(const MoveFacts& facts);

}  // namespace sendspin::server
