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

/// @file test_playback_policy.cpp
/// @brief The initial activation, a player's stream format and the group parameters

#include "frame_encoder.h"
#include "playback_policy.h"
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::AudioFormat;
using sendspin::server::ClientHello;
using sendspin::server::Codec;
using sendspin::server::InputFormat;
using sendspin::server::PlayerSupport;
using sendspin::server::SampleFormat;
using sendspin::server::ServerActivate;
namespace sched = sendspin::server::sched;

ClientHello hello(std::vector<std::string> roles, bool support, bool unpaired) {
    ClientHello hello;
    hello.supported_roles = std::move(roles);
    if (support) {
        hello.player_v1_support = PlayerSupport{{{Codec::PCM, 2, 48000, 16}}, 100000};
    }
    hello.unpaired_access_enabled = unpaired;
    return hello;
}

// messaging.md "server/activate" allows `['playback']` under the Sentinel or a pairing PSK only
// with unpaired access enabled, and pairing.md "Unpaired Access" only with the operator's approval;
// a long-term PSK needs neither. connection.md "Sentinel Fallback": no roles and no `'playback'` on
// an unpaired session while the server holds a long-term PSK for the client, whether or not the
// session fell back. README "Priority and Activation": each listed role this server implements, at
// most one version per family and never the player without its support object (messaging.md
// "client/hello"). A refusal declares an empty active_roles.
TEST(Activation, DeclaresPlaybackAndRolesOnlyWhenAllowed) {
    using sendspin::server::ActivationNotice;
    using sendspin::server::ApprovalScope;
    using sendspin::server::PskCategory;
    using sendspin::server::PskMatch;
    struct Row {
        const char* name;
        PskMatch psk;
        bool paired;
        ClientHello hello;
        std::optional<ApprovalScope> approval;
        bool playback;
        std::vector<std::string> roles;
        ActivationNotice notice;
    };
    const std::vector<std::string> all = {"player@v2",     "controller@v1", "player@v1",
                                          "metadata@v1",   "artwork@v1",    "metadata@v1",
                                          "visualizer@v1", "color@v1"};
    const std::vector<std::string> implemented = {"controller@v1", "player@v1", "metadata@v1",
                                                  "artwork@v1", "color@v1"};
    const PskMatch sentinel{PskCategory::SENTINEL, std::nullopt};
    const PskMatch pairing{PskCategory::PAIRING, std::nullopt};
    const PskMatch long_term{PskCategory::LONG_TERM, std::nullopt};
    const PskMatch long_term_missed{PskCategory::SENTINEL, PskCategory::LONG_TERM};
    const PskMatch pairing_missed{PskCategory::SENTINEL, PskCategory::PAIRING};
    const Row rows[] = {
        {"Control: Sentinel, unpaired access, approved", sentinel, false, hello(all, true, true),
         ApprovalScope::PLAYBACK, true, implemented, ActivationNotice::NONE},
        {"Sentinel, not approved",
         sentinel,
         false,
         hello(all, true, true),
         std::nullopt,
         false,
         {},
         ActivationNotice::APPROVAL_NEEDED},
        {"Sentinel, unpaired access off, approved",
         sentinel,
         false,
         hello(all, true, false),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::PAIRING_REQUIRED},
        {"Sentinel, unpaired access off, not approved",
         sentinel,
         false,
         hello(all, true, false),
         std::nullopt,
         false,
         {},
         ActivationNotice::PAIRING_REQUIRED},
        {"pairing PSK, not approved",
         pairing,
         false,
         hello(all, true, true),
         std::nullopt,
         false,
         {},
         ActivationNotice::APPROVAL_NEEDED},
        {"pairing PSK, unpaired access off",
         pairing,
         false,
         hello(all, true, false),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::PAIRING_REQUIRED},
        {"Control: pairing PSK, approved", pairing, false, hello(all, true, true),
         ApprovalScope::PLAYBACK, true, implemented, ActivationNotice::NONE},
        {"Control: long-term PSK needs neither", long_term, true, hello(all, true, false),
         std::nullopt, true, implemented, ActivationNotice::NONE},
        {"Sentinel fallback from the long-term PSK, approved and unpaired access on",
         long_term_missed,
         true,
         hello(all, true, true),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::CREDENTIAL_MISMATCH},
        {"Sentinel fallback from the long-term PSK, unpaired access off",
         long_term_missed,
         true,
         hello(all, true, false),
         std::nullopt,
         false,
         {},
         ActivationNotice::CREDENTIAL_MISMATCH},
        {"Sentinel without a fallback while the record holds a long-term PSK",
         sentinel,
         true,
         hello(all, true, true),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::NONE},
        {"pairing PSK while the record holds a long-term PSK",
         pairing,
         true,
         hello(all, true, true),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::NONE},
        {"Sentinel fallback from a pairing PSK, approved",
         pairing_missed,
         false,
         hello(all, true, true),
         ApprovalScope::PLAYBACK,
         false,
         {},
         ActivationNotice::PAIRING_PSK_MISMATCH},
        {"the player's support object missing",
         sentinel,
         false,
         hello(all, false, true),
         ApprovalScope::PLAYBACK,
         true,
         {"controller@v1", "metadata@v1", "artwork@v1", "color@v1"},
         ActivationNotice::NONE},
        {"no implemented role listed",
         sentinel,
         false,
         hello({"visualizer@v1"}, true, true),
         ApprovalScope::PLAYBACK,
         true,
         {},
         ActivationNotice::NONE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const sendspin::server::ActivationDecision decision =
            sendspin::server::decide_activation(row.psk, row.paired, row.hello, row.approval, true);
        EXPECT_EQ(decision.activation.playback, row.playback);
        EXPECT_EQ(decision.playable, row.playback);
        // Always present: required on the first activation, and explicit when empty
        ASSERT_TRUE(decision.activation.active_roles);
        EXPECT_EQ(*decision.activation.active_roles, row.roles);
        EXPECT_EQ(decision.notice, row.notice);
        // Without playback asked for, the same roles and no activities
        const sendspin::server::ActivationDecision idle = sendspin::server::decide_activation(
            row.psk, row.paired, row.hello, row.approval, false);
        EXPECT_FALSE(idle.activation.playback);
        EXPECT_EQ(idle.playable, row.playback);
        ASSERT_TRUE(idle.activation.active_roles);
        EXPECT_EQ(*idle.activation.active_roles, row.roles);
        EXPECT_EQ(idle.notice, row.notice);
        // messaging.md "server/activate": never an inadmissible activation for the matched PSK
        for (const sendspin::server::ActivationDecision& made : {decision, idle}) {
            EXPECT_TRUE(sendspin::server::activity_set_allowed(row.psk.category,
                                                               {made.activation.playback, false},
                                                               row.hello.unpaired_access_enabled));
            if (!made.activation.active_roles->empty()) {
                EXPECT_TRUE(sendspin::server::playback_capable(row.psk.category,
                                                               {made.activation.playback, false},
                                                               row.hello.unpaired_access_enabled));
            }
        }
    }
}

// roles/source/v1.md "Unpaired access": an unpaired session gets the source role only with an
// approval that names it, never with approval for playback alone; a paired one needs none. Like
// the player, only with its support object (messaging.md "client/hello").
TEST(Activation, GivesTheSourceRoleOnlyWithItsScope) {
    using sendspin::server::ApprovalScope;
    using sendspin::server::PskCategory;
    using sendspin::server::PskMatch;
    using sendspin::server::SourceSupport;
    struct Row {
        const char* name;
        PskCategory psk;
        std::optional<ApprovalScope> approval;
        bool support;
        std::vector<std::string> roles;
    };
    const Row rows[] = {
        {"Control: long-term PSK",
         PskCategory::LONG_TERM,
         std::nullopt,
         true,
         {"source@v1", "player@v1"}},
        {"Control: Sentinel, approved with the source",
         PskCategory::SENTINEL,
         ApprovalScope::PLAYBACK_AND_SOURCE,
         true,
         {"source@v1", "player@v1"}},
        {"Sentinel, approved for playback",
         PskCategory::SENTINEL,
         ApprovalScope::PLAYBACK,
         true,
         {"player@v1"}},
        {"pairing PSK, approved for playback",
         PskCategory::PAIRING,
         ApprovalScope::PLAYBACK,
         true,
         {"player@v1"}},
        {"Control: pairing PSK, approved with the source",
         PskCategory::PAIRING,
         ApprovalScope::PLAYBACK_AND_SOURCE,
         true,
         {"source@v1", "player@v1"}},
        {"no support object", PskCategory::LONG_TERM, std::nullopt, false, {"player@v1"}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        ClientHello client = hello({"source@v1", "player@v1"}, true, true);
        if (row.support) {
            client.source_v1_support = SourceSupport{};
        }
        const sendspin::server::ActivationDecision decision = sendspin::server::decide_activation(
            PskMatch{row.psk, std::nullopt}, row.psk == PskCategory::LONG_TERM, client,
            row.approval, true);
        ASSERT_TRUE(decision.activation.active_roles);
        EXPECT_EQ(*decision.activation.active_roles, row.roles);
    }
}

// spec player/v1.md "client/hello player@v1 support object": the preferred format when it can be
// produced, else the first entry at the input's rate that can, else the first that can; here PCM at
// 16, 24 or 32 bits and FLAC at 16 or 24, at any rate whose frame spans at most seven cells, with
// the input's channel count or a mono one
TEST(PlayerFormat, PrefersTheClientsChoiceAmongWhatTheInputGives) {
    const InputFormat input{48000, 2, SampleFormat::S16};
    const AudioFormat flac16{Codec::FLAC, 2, 48000, 16};
    const AudioFormat flac24{Codec::FLAC, 2, 48000, 24};
    const AudioFormat flac32{Codec::FLAC, 2, 48000, 32};
    const AudioFormat pcm16{Codec::PCM, 2, 48000, 16};
    const AudioFormat pcm24{Codec::PCM, 2, 48000, 24};
    const AudioFormat other_rate{Codec::PCM, 2, 44100, 16};
    const AudioFormat flac_other_rate{Codec::FLAC, 2, 44100, 16};
    const AudioFormat two_cells{Codec::PCM, 2, 11025, 16};
    // gcd(44,101, 50) = 1: a frame of 50 cells, longer than a chunk
    const AudioFormat long_frame{Codec::PCM, 2, 44101, 16};
    const AudioFormat mono{Codec::PCM, 1, 48000, 16};
    const AudioFormat flac_mono{Codec::FLAC, 1, 44100, 16};
    const AudioFormat six{Codec::PCM, 6, 48000, 16};
    const AudioFormat odd_depth{Codec::PCM, 2, 48000, 20};
    const AudioFormat opus{Codec::OPUS, 2, 48000, 16};
    const AudioFormat opus_44{Codec::OPUS, 2, 44100, 16};
    const AudioFormat opus_24{Codec::OPUS, 2, 48000, 24};
    const AudioFormat opus_mono_16k{Codec::OPUS, 1, 16000, 16};
    const AudioFormat opus_mono_16k_24{Codec::OPUS, 1, 16000, 24};
    // One FLAC frame at its bound plus the 13-byte header
    const auto flac16_message = static_cast<uint32_t>(
        sendspin::server::output_shape(*sendspin::server::to_sched(flac16)).max_frame_bytes + 13);
    struct Row {
        const char* name;
        std::vector<AudioFormat> formats;
        uint32_t capacity;
        std::optional<AudioFormat> preferred;
        std::optional<AudioFormat> selected;
        std::vector<sched::AudioFormat> excluded;
        /// ServerConfig::enable_opus
        bool opus{false};
    };
    const Row rows[] = {
        {"Control: the first entry", {pcm16, pcm24}, 100000, std::nullopt, pcm16, {}},
        {"the preferred entry", {pcm16, pcm24}, 100000, pcm24, pcm24, {}},
        {"FLAC listed first", {flac16, pcm16}, 100000, std::nullopt, flac16, {}},
        {"FLAC preferred, listed second", {pcm16, flac16}, 100000, flac16, flac16, {}},
        {"FLAC at 24 bits", {flac24, pcm16}, 100000, std::nullopt, flac24, {}},
        {"FLAC at 32 bits is skipped", {flac32, flac24}, 100000, std::nullopt, flac24, {}},
        {"a preference of FLAC at 32 bits falls back", {pcm16, flac32}, 100000, flac32, pcm16, {}},
        {"a preference not listed is ignored", {pcm16}, 100000, flac16, pcm16, {}},
        {"a preference at another rate is resampled",
         {pcm16, other_rate},
         100000,
         other_rate,
         other_rate,
         {}},
        {"the input's rate before an earlier entry at another rate",
         {other_rate, flac_other_rate, pcm24},
         100000,
         std::nullopt,
         pcm24,
         {}},
        {"another rate when no entry has the input's, frames of two cells included",
         {long_frame, two_cells, flac_other_rate, other_rate},
         100000,
         std::nullopt,
         two_cells,
         {}},
        {"one channel from two", {mono}, 100000, std::nullopt, mono, {}},
        {"one channel at another rate", {flac_mono}, 100000, std::nullopt, flac_mono, {}},
        {"entries the input cannot give are skipped",
         {long_frame, six, odd_depth, opus, pcm24},
         100000,
         std::nullopt,
         pcm24,
         {}},
        {"none produced", {flac32, long_frame, six, opus}, 100000, std::nullopt, std::nullopt, {}},
        // Spec player/v1.md "client/hello player@v1 support object": Opus where the player lists
        // it and the server streams it, at the rates and channel counts it takes
        {"Opus listed first", {opus, pcm16}, 100000, std::nullopt, opus, {}, true},
        {"Opus preferred", {pcm16, opus}, 100000, opus, opus, {}, true},
        // An Opus stream is 16-bit whatever depth the entry lists, so a preference that differs
        // only in depth is the listed entry
        {"Opus mono at 16 kHz, listed at 24 bits",
         {opus_mono_16k_24},
         100000,
         std::nullopt,
         opus_mono_16k,
         {},
         true},
        {"Opus preferred at another depth", {pcm16, opus}, 100000, opus_24, opus, {}, true},
        {"Opus at a rate it does not take",
         {opus_44, pcm16},
         100000,
         std::nullopt,
         pcm16,
         {},
         true},
        // One Opus packet at its bound, 1,276 bytes, plus the 13-byte header
        {"a capacity that holds an Opus packet", {opus}, 1289, std::nullopt, opus, {}, true},
        {"a capacity below an Opus packet", {opus}, 1288, std::nullopt, std::nullopt, {}, true},
        {"an excluded format is skipped",
         {flac16, pcm16},
         100000,
         flac16,
         pcm16,
         {*sendspin::server::to_sched(flac16)}},
        // One 20 ms frame of 48 kHz 16-bit stereo is 3,840 bytes, plus the 13-byte header
        {"a capacity that holds one frame", {pcm16}, 3853, std::nullopt, pcm16, {}},
        {"a capacity below one frame", {pcm16}, 3852, std::nullopt, std::nullopt, {}},
        {"a capacity that holds FLAC's bound",
         {flac16, pcm16},
         flac16_message,
         std::nullopt,
         flac16,
         {}},
        {"a capacity below FLAC's bound",
         {flac16, pcm16},
         flac16_message - 1,
         std::nullopt,
         pcm16,
         {}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<sched::AudioFormat> selected = sendspin::server::select_player_format(
            PlayerSupport{row.formats, row.capacity}, row.preferred, input, row.excluded,
            std::nullopt, row.opus);
        ASSERT_EQ(selected.has_value(), row.selected.has_value());
        if (selected) {
            EXPECT_EQ(sendspin::server::from_sched(*selected), *row.selected);
        }
    }
    // SendspinServer::set_player_format(): the operator's format first, while the server can give
    // it to the player, then the player's own preference
    struct OperatorRow {
        const char* name;
        std::vector<AudioFormat> formats;
        std::optional<AudioFormat> operator_format;
        std::optional<AudioFormat> preferred;
        AudioFormat selected;
        std::vector<sched::AudioFormat> excluded;
    };
    const OperatorRow operator_rows[] = {
        {"Control: the operator's format before the player's",
         {pcm16, pcm24, flac16},
         flac16,
         pcm24,
         flac16,
         {}},
        {"an operator's format not listed leaves the player's",
         {pcm16, pcm24},
         flac16,
         pcm24,
         pcm24,
         {}},
        {"an operator's format the server cannot give leaves the player's",
         {pcm16, flac32, pcm24},
         flac32,
         pcm24,
         pcm24,
         {}},
        {"an excluded operator's format leaves the first entry",
         {pcm16, flac16},
         flac16,
         std::nullopt,
         pcm16,
         {*sendspin::server::to_sched(flac16)}},
    };
    for (const OperatorRow& row : operator_rows) {
        SCOPED_TRACE(row.name);
        const std::optional<sched::AudioFormat> selected = sendspin::server::select_player_format(
            PlayerSupport{row.formats, 100000}, row.preferred, input, row.excluded,
            row.operator_format);
        ASSERT_TRUE(selected);
        EXPECT_EQ(sendspin::server::from_sched(*selected), row.selected);
    }
    // 44,100 Hz frames are 882 samples, so that rate streams from a 44,100 Hz input
    EXPECT_TRUE(sendspin::server::select_player_format(PlayerSupport{{other_rate}, 100000},
                                                       std::nullopt,
                                                       InputFormat{44100, 2, SampleFormat::S16}));
    // 11,025 Hz frames span two cells: an input at that rate streams at it, and is resampled to
    // another rate though its cells do not end on whole samples
    const InputFormat input_11025{11025, 2, SampleFormat::S16};
    EXPECT_EQ(
        sendspin::server::select_player_format(
            PlayerSupport{{{Codec::FLAC, 2, 11025, 16}, pcm16}, 100000}, std::nullopt, input_11025),
        (sched::AudioFormat{sched::Codec::FLAC, 11025, 2, 16}));
    EXPECT_EQ(sendspin::server::select_player_format(PlayerSupport{{pcm16}, 100000}, std::nullopt,
                                                     input_11025),
              *sendspin::server::to_sched(pcm16));
    // Control: mono to any channel count
    EXPECT_TRUE(sendspin::server::select_player_format(PlayerSupport{{six}, 100000}, std::nullopt,
                                                       InputFormat{48000, 1, SampleFormat::S16}));
}

// `G` over the eligible players' leads, `od_max`, and `W`: for a buffered input from the largest
// capacity in time, clamped; for a live input each lead leaves out the required lead and adds its
// pipeline's latency, and `W` is `W_live`
TEST(GroupParameters, FollowTheEligiblePlayers) {
    const sched::SchedTiming timing;
    // 1.5 s and 10.38 s lie inside and above the clamp in either profile (profile.h)
    static_assert(sched::timing_defaults::WRITE_AHEAD_MIN_US < 1500000 &&
                  1500000 < sched::timing_defaults::WRITE_AHEAD_MAX_US &&
                  sched::timing_defaults::WRITE_AHEAD_MAX_US < 10380000);
    const sched::AudioFormat pcm16{sched::Codec::PCM, 48000, 2, 16};
    constexpr auto LIVE = sendspin::server::InputKind::LIVE;
    // No resampler, J_in 10 ms, so L_p is 30 ms
    constexpr int64_t L_P = 30000;
    struct Row {
        const char* name;
        std::vector<sendspin::server::EligiblePlayer> players;
        sendspin::server::GroupParams params;
        sendspin::server::InputKind kind{sendspin::server::InputKind::BUFFERED};
    };
    const Row rows[] = {
        {"Control: none gives the defaults", {}, {250000, 0, 1000000}},
        {"one player's required lead", {{{100, 300, 0}, 100000, pcm16}}, {310000, 0, 1000000}},
        {"J_min under a small lead", {{{20, 0, 0}, 100000, pcm16}}, {110000, 0, 1000000}},
        {"the output delay in the lead and od_max",
         {{{100, 300, 0}, 100000, pcm16}, {{200, 0, 50}, 100000, pcm16}},
         {310000, 50000, 1000000}},
        // 192,650 B/s: 2,000,000 B is 10.38 s, clamped to W_max; 288,975 B is 1.5 s
        {"a capacity past W_max",
         {{{100, 0, 0}, 2000000, pcm16}},
         {110000, 0, timing.write_ahead_max_us}},
        {"a capacity inside the clamp", {{{100, 0, 0}, 288975, pcm16}}, {110000, 0, 1500000}},
        {"live: D under J_min", {{{60, 0, 0}, 64000, pcm16, L_P}}, {140000, 0, 200000}, LIVE},
        {"live: E raises G",
         {{{60, 0, 0}, 64000, pcm16, L_P}, {{150, 0, 40}, 64000, pcm16, L_P}},
         {230000, 40000, 200000},
         LIVE},
        {"live leaves out the required lead",
         {{{100, 300, 0}, 100000, pcm16, L_P}},
         {140000, 0, 200000},
         LIVE},
        // The higher lead plus latency wins, not the higher lead
        {"live: the larger lead plus its own latency",
         {{{150, 0, 0}, 100000, pcm16, L_P}, {{140, 0, 0}, 100000, pcm16, 60000}},
         {210000, 0, 200000},
         LIVE},
        {"live: W_live whatever the capacity",
         {{{100, 0, 0}, 2000000, pcm16, L_P}},
         {140000, 0, 200000},
         LIVE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::group_params(row.players, timing, row.kind), row.params);
    }
}

// A host's move takes a connected client that may play out of its group, and the place held for a
// client not connected within its rejoin grace; a client held with no roles stays alone, and a
// client already in the target stays
TEST(GroupMove, MovesWhatTheHostCanMove) {
    using sendspin::server::MoveAction;
    using sendspin::server::MoveFacts;
    struct Row {
        const char* name;
        MoveFacts facts;
        MoveAction action;
    };
    // connected, playable, in_target, held, held_in_target
    const Row rows[] = {
        {"Control: a connected client that may play",
         {true, true, false, false, false},
         MoveAction::MOVE},
        {"held with no roles", {true, false, false, false, false}, MoveAction::NONE},
        {"in the target already", {true, true, true, false, false}, MoveAction::NONE},
        {"a connection outranks a held place", {true, true, false, true, false}, MoveAction::MOVE},
        {"Control: not connected, its place held",
         {false, false, false, true, false},
         MoveAction::MOVE_HELD},
        {"its place held in the target already",
         {false, false, false, true, true},
         MoveAction::NONE},
        {"not connected, no place held", {false, true, false, false, false}, MoveAction::NONE},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::move_action(row.facts), row.action);
    }
}

}  // namespace
