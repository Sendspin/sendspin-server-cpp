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

/// @file lifecycle.h
/// @brief The player and visualizer stream lifecycle as desired-state markers, and the drain
/// predicates. Pure functions.

#pragma once

#include "sched/ledger.h"
#include "sched/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace sendspin::server::sched {

/// @brief What the messages already written have told the client about one stream role
struct StreamSide {
    /// A stream/start written and no stream/end since
    bool active{false};
    /// The run of the last stream/start written
    RunId run;
    /// The epoch of the last stream/start or stream/clear written
    EpochId epoch;
    /// The format last announced
    FormatKey format;
};

/// @brief What the server wants the client to have for one stream role
struct Want {
    bool active{false};
    RunId run;
    EpochId epoch;
    FormatKey format;
};

/// @brief The state a marker evaluates one role against when it reaches the head
struct WantInputs {
    /// The role is in the active_roles of the activation last written
    bool role_written{false};
    /// The role is in roles_ahead() for this marker
    bool role_ahead{false};
    /// The session's group has a playing run
    bool group_playing{false};
    RunId run;
    EpochId epoch;
    /// The current epoch is draining
    bool epoch_draining{false};
    /// role_drain_done() for the role
    bool drain_done{false};
    /// The latest client/state reports `available: true`
    bool available{false};
    /// The selected format or visualizer configuration; nullopt when none is producible
    std::optional<FormatKey> format;
};

/// @brief `want`: active while the role is written and stays in the next activation, the group
/// plays, the client is available, a format is producible and, in a draining epoch, the role's
/// drain is not done
Want evaluate_want(const WantInputs& in);

enum class ControlItemKind : uint8_t {
    /// Any message that is not an activation
    MESSAGE,
    MARKER,
    ACTIVATION,
    /// A re-handshake start, carrying its frozen activation
    REKEY,
};

/// @brief A control FIFO entry as a marker sees it
struct ControlItemView {
    ControlItemKind kind{ControlItemKind::MESSAGE};
    /// The stream roles of an ACTIVATION or a REKEY item's frozen activation
    RoleSet roles;
};

/// @brief `roles_ahead`: the stream roles of the first activation queued behind a marker, a rekey
/// item's frozen one included; roles_written when none is
/// @param behind the control FIFO entries after the marker, in order
RoleSet roles_ahead(std::span<const ControlItemView> behind, RoleSet roles_written);

/// @brief What a stream/start from empty needs when it is written, besides the desired state
struct StartPreconditions {
    /// The latest client/state reports `available: true`
    bool available{false};
    /// The role is in the active_roles of the activation last written
    bool role_written{false};
    /// The role's client/state object has arrived since that activation (spec messaging.md
    /// "client/state")
    bool state_since_activation{false};
    /// `pending_kind` is set: a start needs a trigger event (spec messaging.md "stream/start")
    bool trigger_pending{false};
};

enum class StreamAction : uint8_t { NONE, END, CLEAR, START };

/// @brief The first row of the marker table that applies to one role: an end when the stream should
/// stop or its run changed (a run change is never a clear), a clear when its epoch changed, a start
/// from empty when wanted and every precondition holds
StreamAction reconcile_stream(const StreamSide& sent, const Want& want,
                              const StartPreconditions& pre);

/// @brief The one message a marker emits on a dequeue, listing every role it applies to
struct MarkerEmission {
    StreamAction action{StreamAction::NONE};
    RoleSet roles;
};

/// @brief A marker's next message: each role's reconcile_stream() action, taken in the order end,
/// clear, start, so a start never precedes an end or a clear the marker owes. NONE pops the marker.
MarkerEmission marker_step(const std::array<StreamSide, STREAM_ROLE_COUNT>& sent,
                           const std::array<Want, STREAM_ROLE_COUNT>& want,
                           const std::array<StartPreconditions, STREAM_ROLE_COUNT>& pre);

/// @brief The sent-state effect of a marker message on one listed role, in the handler that starts
/// its write; the caller also resets the role's ledger and moves its cursor
void apply_action(StreamSide& sent, const Want& want, StreamAction action);

/// @brief An active stream's drain is done: its cursor is past its own instance's final index and
/// its ledger has emptied
bool drain_done(int64_t cursor_next, std::optional<int64_t> own_final_index, const Ledger& ledger,
                int64_t now_us);

/// @brief A start in a draining epoch still finds audio: `now + startup lead <= last_ts(m)`; false
/// when the epoch has no input
/// @param last_ts_us `last_ts(m)` for the role's format, nullopt when the epoch has no input
bool start_finds_audio(int64_t now_us, int64_t startup_lead_us, std::optional<int64_t> last_ts_us);

/// @brief `drain_done(R)` in a draining epoch: drain_done() for an active stream, no audio left
/// for a start otherwise
bool role_drain_done(bool active, int64_t cursor_next, std::optional<int64_t> own_final_index,
                     const Ledger& ledger, int64_t startup_lead_us,
                     std::optional<int64_t> last_ts_us, int64_t now_us);

/// @brief A group's drain as core sees it
struct GroupDrainView {
    bool epoch_draining{false};
    bool run_playing{false};
    /// Some session of the group has an active player or visualizer stream in this run
    bool any_stream_active{false};
    /// `last_ts` from `DrainStarted`; nullopt for an epoch with no input
    std::optional<int64_t> last_ts_us;
};

/// @brief `drain_complete`: the draining run has no active stream and no player start can still
/// find audio, which holds from `last_ts - J_min`
bool drain_complete(const GroupDrainView& group, int64_t join_min_us, int64_t now_us);

}  // namespace sendspin::server::sched
