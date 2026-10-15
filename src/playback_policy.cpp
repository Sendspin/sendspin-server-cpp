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

#include "playback_policy.h"

#include "frame_encoder.h"
#include "pipeline_stages.h"
#include "sched/chunk.h"
#include "sched/frames.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace sendspin::server {

std::vector<std::string> implemented_roles(const ClientHello& hello) {
    std::vector<std::string> roles;
    for (const std::string& role : hello.supported_roles) {
        // One version per family is implemented, so its first match is the listed one
        const bool implemented = (role == PLAYER_ROLE && hello.player_v1_support) ||
                                 (role == VISUALIZER_ROLE && hello.visualizer_v1_support) ||
                                 (role == SOURCE_ROLE && hello.source_v1_support) ||
                                 role == CONTROLLER_ROLE || role == METADATA_ROLE ||
                                 role == ARTWORK_ROLE || role == COLOR_ROLE;
        if (implemented && std::find(roles.begin(), roles.end(), role) == roles.end()) {
            roles.push_back(role);
        }
    }
    return roles;
}

ActivationDecision decide_activation(const PskMatch& psk, bool paired, const ClientHello& hello,
                                     std::optional<ApprovalScope> approval, bool playback) {
    ActivationDecision decision{{false, std::vector<std::string>{}, std::nullopt},
                                ActivationNotice::NONE};
    if (psk.fallback_from == PskCategory::PAIRING) {
        decision.notice = ActivationNotice::PAIRING_PSK_MISMATCH;
        return decision;
    }
    if (psk.category != PskCategory::LONG_TERM) {
        // connection.md "Sentinel Fallback": no roles and no 'playback' while the record exists
        if (paired) {
            if (psk.fallback_from == PskCategory::LONG_TERM) {
                decision.notice = ActivationNotice::CREDENTIAL_MISMATCH;
            }
            return decision;
        }
        if (!playback_capable(psk.category, {}, hello.unpaired_access_enabled)) {
            decision.notice = ActivationNotice::PAIRING_REQUIRED;
            return decision;
        }
        if (!approval) {
            decision.notice = ActivationNotice::APPROVAL_NEEDED;
            return decision;
        }
    }
    decision.playable = true;
    decision.activation.playback = playback;
    std::vector<std::string> roles = implemented_roles(hello);
    // roles/source/v1.md "Unpaired access": only an explicit approval covers the source role
    if (psk.category != PskCategory::LONG_TERM && approval != ApprovalScope::PLAYBACK_AND_SOURCE) {
        std::erase(roles, SOURCE_ROLE);
    }
    decision.activation.active_roles = std::move(roles);
    return decision;
}

std::optional<sched::AudioFormat> to_sched(const AudioFormat& format) {
    constexpr uint32_t BYTE_MAX = std::numeric_limits<uint8_t>::max();
    if (format.channels > BYTE_MAX || format.bit_depth > BYTE_MAX) {
        return std::nullopt;
    }
    sched::AudioFormat converted;
    converted.codec = format.codec;
    converted.sample_rate = format.sample_rate;
    converted.channels = static_cast<uint8_t>(format.channels);
    converted.bit_depth = static_cast<uint8_t>(format.bit_depth);
    // Spec player/v1.md: bit_depth is ignored for opus. The server encodes Opus from 16-bit PCM,
    // so every Opus entry is 16-bit and entries differing only in depth are one format
    if (converted.codec == sched::Codec::OPUS) {
        converted.bit_depth = OPUS_BIT_DEPTH;
    }
    return converted;
}

AudioFormat from_sched(const sched::AudioFormat& format) {
    AudioFormat converted;
    converted.codec = format.codec;
    converted.sample_rate = format.sample_rate;
    converted.channels = format.channels;
    converted.bit_depth = format.bit_depth;
    return converted;
}

StreamCodec stream_codec(Codec codec) {
    switch (codec) {
        case Codec::OPUS:
            return StreamCodec::OPUS;
        case Codec::FLAC:
            return StreamCodec::FLAC;
        case Codec::PCM:
            break;
    }
    return StreamCodec::PCM;
}

StreamFormat stream_format(const sched::AudioFormat& format) {
    StreamFormat converted;
    converted.codec = stream_codec(format.codec);
    converted.sample_rate = format.sample_rate;
    converted.channels = format.channels;
    converted.bit_depth = format.bit_depth;
    return converted;
}

AudioFormat client_format(const StreamFormat& format) {
    AudioFormat converted;
    switch (format.codec) {
        case StreamCodec::OPUS:
            converted.codec = Codec::OPUS;
            break;
        case StreamCodec::FLAC:
            converted.codec = Codec::FLAC;
            break;
        case StreamCodec::PCM:
            converted.codec = Codec::PCM;
            break;
    }
    converted.sample_rate = format.sample_rate;
    converted.channels = format.channels;
    converted.bit_depth = format.bit_depth;
    return converted;
}

bool pipeline_converts(const InputFormat& input, const sched::AudioFormat& format) {
    return channel_mix_supported(input.channels, format.channels);
}

std::optional<sched::AudioFormat> select_player_format(
    const PlayerSupport& support, const std::optional<AudioFormat>& preferred,
    const InputFormat& input, std::span<const sched::AudioFormat> excluded,
    const std::optional<AudioFormat>& operator_format, bool opus) {
    std::vector<sched::FormatShape> shapes;
    shapes.reserve(support.supported_formats.size());
    for (const AudioFormat& entry : support.supported_formats) {
        const std::optional<sched::AudioFormat> format = to_sched(entry);
        if (!format) {
            continue;
        }
        sched::FormatShape shape = output_shape(*format);
        shape.encoder_available =
            shape.encoder_available && pipeline_converts(input, *format) &&
            (opus || format->codec != sched::Codec::OPUS) &&
            std::find(excluded.begin(), excluded.end(), *format) == excluded.end();
        shapes.push_back(shape);
    }
    // select_format() gives a preference back exactly when it is producible
    if (const std::optional<sched::AudioFormat> wanted =
            operator_format ? to_sched(*operator_format) : std::nullopt) {
        const std::optional<sched::AudioFormat> selected =
            sched::select_format(shapes, wanted, support.buffer_capacity, input.sample_rate);
        if (selected == wanted) {
            return selected;
        }
    }
    std::optional<sched::AudioFormat> wanted;
    if (preferred) {
        wanted = to_sched(*preferred);
    }
    return sched::select_format(shapes, wanted, support.buffer_capacity, input.sample_rate);
}

sched::PlayerTiming player_timing(const PlayerState& state) {
    return {state.min_buffer_ms, state.required_lead_time_ms, state.output_delay_ms};
}

int64_t pcm_bytes_per_second(const sched::AudioFormat& format) {
    if (format.codec == sched::Codec::OPUS) {
        return (OPUS_MAX_PACKET_BYTES + sched::CHUNK_HEADER_BYTES) * sched::CELLS_PER_SECOND;
    }
    const int64_t frame_bytes = int64_t{format.channels} * ((format.bit_depth + 7) / 8);
    return int64_t{format.sample_rate} * frame_bytes +
           sched::CHUNK_HEADER_BYTES * sched::CELLS_PER_SECOND;
}

GroupParams group_params(std::span<const EligiblePlayer> players, const sched::SchedTiming& timing,
                         InputKind kind) {
    std::vector<sched::LeadInput> leads;
    std::vector<sched::CapacityRate> rates;
    leads.reserve(players.size());
    rates.reserve(players.size());
    GroupParams params;
    for (const EligiblePlayer& player : players) {
        leads.push_back({sched::player_lead_us(player.timing, kind, timing.timing_clamp_us),
                         player.pipeline_latency_us});
        rates.push_back({player.capacity_bytes, pcm_bytes_per_second(player.format)});
        params.output_delay_max_us =
            std::max(params.output_delay_max_us, player.timing.output_delay_ms * sched::US_PER_MS);
    }
    params.group_lead_us = sched::group_lead_us(leads, timing);
    // A live input is bounded by W_live
    params.write_ahead_us =
        kind == InputKind::LIVE
            ? timing.write_ahead_live_us
            : sched::write_ahead_us(rates, timing.write_ahead_min_us, timing.write_ahead_max_us);
    return params;
}

MoveAction move_action(const MoveFacts& facts) {
    if (facts.connected) {
        return facts.playable && !facts.in_target ? MoveAction::MOVE : MoveAction::NONE;
    }
    return facts.held && !facts.held_in_target ? MoveAction::MOVE_HELD : MoveAction::NONE;
}

}  // namespace sendspin::server
