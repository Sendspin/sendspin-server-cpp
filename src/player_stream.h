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

/// @file player_stream.h
/// @brief One session's player stream: what its messages have told the client, what the server
/// wants it to have, the cursor over the group's frames, its switch to another format in place and
/// the ledger. Its markers and its cursor read the group through StreamSource. Core thread only.

#pragma once

#include "messages.h"
#include "sched/chunk.h"
#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/leads.h"
#include "sched/ledger.h"
#include "sched/lifecycle.h"
#include "sched/selection.h"
#include "sched/types.h"
#include "send_queues.h"
#include "sendspin/server/types.h"
#include "stream_source.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin::server {

/// @brief A chunk packed for writing; the header's send_ahead waits for the stamp
struct PackedChunk {
    OutboundPlaintext plaintext;
    int64_t ts_us{0};
    /// The encoded audio of the frames packed, the plaintext less its header
    size_t audio_bytes{0};
};

/// @brief The player object of a stream/start announcing format (spec player/v1.md "stream/start
/// player object")
StreamStart player_start(const sched::AudioFormat& format);

class PlayerStream {
public:
    // ------------------------------------------------------------------------------
    // Inputs
    // ------------------------------------------------------------------------------

    /// @brief The client/hello's player support object; sets the ledger's capacity
    void set_support(const PlayerSupport& support);

    /// @brief An activation was taken for writing
    /// @return a trigger kind when it added the role, which needs one
    std::optional<sched::TriggerKind> on_activation_written(bool player_active);

    /// @brief A validated client/state
    /// @return a trigger kind when it made the player eligible: a report of `available: true`
    /// that is the first or follows `false`, or the role's first object since its activation
    std::optional<sched::TriggerKind> on_client_state(const ClientState& state);

    /// @brief Records the kind of the latest kind-setting event
    void set_pending_kind(sched::TriggerKind kind) {
        this->pending_kind_ = kind;
    }

    /// @brief A marker popped
    void clear_pending_kind() {
        this->pending_kind_.reset();
    }

    // ------------------------------------------------------------------------------
    // The pump
    // ------------------------------------------------------------------------------

    /// @brief The cursor's step before every selection, with the switch in place a changed
    /// preference asks for
    /// @return the drain watch wants a marker
    bool prepare(StreamSource* source, uint32_t markers_queued, int64_t now_us);

    /// @brief The cursor's entry in the pump snapshot; after prepare(). A chunk that could still
    /// grow waits for its frames or room while it has the slack.
    /// @param max_message_bytes `max_message_bytes` for the format's message cap
    sched::AudioCandidate candidate(const StreamSource* source, size_t max_message_bytes,
                                    int64_t now_us) const;

    /// @brief When the drain watch wants a marker
    int64_t drain_watch_us(const StreamSource* source, uint32_t markers_queued) const;

    /// @brief What a marker at the head wants for the role
    sched::Want want(const StreamSource* source, sched::RoleSet roles_ahead, int64_t now_us) const;

    /// @brief What a start from empty needs besides the desired state
    sched::StartPreconditions preconditions() const;

    /// @brief What the messages taken for writing told the client
    const sched::StreamSide& sent() const {
        return this->sent_;
    }

    /// @brief A marker's message for the role, with want, taken for writing at stamp_us: its
    /// effects in the same step on sent state, ledger and cursor
    void commit_marker(StreamSource* source, sched::StreamAction action, const sched::Want& want,
                       int64_t stamp_us);

    /// @brief Whether the cursor's next item is an in-place stream/start: the format a clear
    /// changed, or a switch at its switch point whose target holds the frame there; after prepare()
    bool announce_due(const StreamSource* source) const;

    /// @brief Builds the in-place stream/start with the stamp; the cursor moves to its format,
    /// with no ledger reset and no startup lead (spec player/v1.md "Player Buffer Accounting",
    /// "Server Audio Send Constraints"). nullopt when none is due.
    std::optional<OutboundPlaintext> commit_announce(StreamSource* source, int64_t stamp_us);

    /// @brief Packs the next chunk from the cursor and counts it in the ledger; only when
    /// candidate() reports a chunk ready
    /// @param max_message_bytes `max_message_bytes` for the format's message cap
    PackedChunk take_chunk(const StreamSource* source, size_t max_message_bytes, int64_t now_us);

    // ------------------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------------------

    /// @brief A player stream is active as of the messages taken for writing
    bool active() const {
        return this->sent_.active;
    }

    /// @brief The operator's format for the player (SendspinServer::set_player_format()), which
    /// selection tries before the player's own preference
    void set_format_override(std::optional<AudioFormat> format) {
        this->format_override_ = format;
    }

    /// @brief The format the cursor reads, which an in-place stream/start may still announce
    sched::AudioFormat cursor_format() const {
        return this->announce_ ? *this->announce_ : this->sent_.format.audio;
    }

    /// @brief In the group's eligible set `P`: the role written, available, its client/state
    /// received since the activation; the format is checked by the caller
    bool eligible() const {
        return this->role_written_ && this->available_ && this->state_since_activation_;
    }

    /// @brief The latest client/state reported `available: true`
    bool available() const {
        return this->available_;
    }

    bool role_written() const {
        return this->role_written_;
    }

    const std::optional<PlayerState>& player_state() const {
        return this->player_state_;
    }

    const std::optional<PlayerSupport>& support() const {
        return this->support_;
    }

    /// @brief The stream format select_player_format() gives with the group's input and the
    /// player's preference. A stream active in the group's run keeps the format it streams or
    /// switches to across set_format(), and derives it again when the preference changes (spec
    /// player/v1.md "client/state player object").
    std::optional<sched::AudioFormat> wanted_format(const StreamSource* source) const;

private:
    /// A switch in place for one player
    struct Switch {
        sched::AudioFormat format;
        /// The index at which the cursor moves to format's instance; nullopt until planned
        std::optional<int64_t> point;
    };

    /// A planned switch whose target can reach its point
    struct SwitchTarget {
        const sched::FrameRing* ring{nullptr};
        int64_t point{0};
    };

    /// The operator's format and the player's client/state `format`, which selection tries in
    /// that order
    struct Preferences {
        std::optional<AudioFormat> operator_format;
        std::optional<AudioFormat> client;

        bool operator==(const Preferences&) const = default;
    };

    std::optional<AudioFormat> client_preference() const;
    Preferences preferences() const;
    /// The format the cursor reads or switches to
    sched::AudioFormat target_format() const {
        return this->switch_ ? this->switch_->format : this->cursor_format();
    }
    /// Plans, retargets or drops the switch in place after a change of the wanted format
    void follow_wanted(StreamSource* source);
    /// The planned switch, while its target's ring has the stream's epoch and no final index
    /// below the point
    std::optional<SwitchTarget> switch_at(const StreamSource* source) const;
    /// `lead_i` and `floor_i` for the group input's kind
    sched::PlayerLead lead(InputKind kind) const;
    /// The ring the cursor reads, when it holds the sent epoch
    const sched::FrameRing* cursor_ring(const StreamSource* source) const;
    /// The frames from the cursor's next index the next chunk may carry, and its limits
    struct ChunkView {
        std::array<sched::FrameView, sched::TARGET_CHUNK_CELLS> frames{};
        size_t held{0};
        sched::PackLimits limits;
    };
    ChunkView chunk_view(const StreamSource* source, const sched::FrameRing& ring,
                         size_t max_message_bytes, int64_t now_us) const;
    /// Points the cursor at a new trigger's first frame: the first at or after `min_ts`, which
    /// prepare() finds
    void bind_cursor(const StreamSource* source, int64_t stamp_us);
    int64_t output_delay_us() const;

    std::optional<PlayerSupport> support_;
    std::optional<PlayerState> player_state_;
    sched::StreamSide sent_;
    sched::Cursor cursor_;
    sched::Ledger ledger_{0, true};
    std::optional<sched::TriggerKind> pending_kind_;
    /// A clear changed the format: the cursor announces it before its next chunk
    std::optional<sched::AudioFormat> announce_;
    /// A switch in place pending for the active stream
    std::optional<Switch> switch_;
    /// The preferences the active stream's format was derived for
    Preferences derived_for_;
    std::optional<AudioFormat> format_override_;
    /// output_shape() of the format the cursor last packed, kept so a chunk builds no encoder
    mutable std::optional<sched::FormatShape> cursor_shape_;
    bool role_written_{false};
    bool state_since_activation_{false};
    bool available_{false};
    bool state_received_{false};
    sched::SchedTiming timing_;
};

}  // namespace sendspin::server
