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

/// @file visualizer_stream.h
/// @brief One session's visualizer stream: what its messages have told the client, what the server
/// wants it to have, the cursor over the group's visualizer messages, its configuration change in
/// place and the ledger. It reads the group through StreamSource. Core thread only.

#pragma once

#include "messages.h"
#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/ledger.h"
#include "sched/lifecycle.h"
#include "sched/selection.h"
#include "sched/types.h"
#include "send_queues.h"
#include "stream_source.h"
#include "visualizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin::server {

class VisualizerStream {
public:
    // ------------------------------------------------------------------------------
    // Inputs
    // ------------------------------------------------------------------------------

    /// @brief The client/hello's visualizer support object; sets the ledger's capacity
    void set_support(const VisualizerSupport& support);

    /// @brief An activation was taken for writing
    /// @return a trigger kind when it added the role, which needs one
    std::optional<sched::TriggerKind> on_activation_written(bool visualizer_active);

    /// @brief A validated client/state
    /// @return a trigger kind when it carried the role's first object since its activation
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

    /// @brief The cursor's step before every selection: past messages late for the client (`ts <=
    /// now`) and before the trigger's, and a configuration change in place the client/state asks
    /// for (spec roles/visualizer/v1.md "client/state visualizer object")
    /// @return the drain watch wants a marker
    bool prepare(StreamSource* source, uint32_t markers_queued, int64_t now_us);

    /// @brief The cursor's entry in the pump snapshot, after prepare(); its deadline is the next
    /// message's `ts`
    sched::AudioCandidate candidate(const StreamSource* source, int64_t now_us) const;

    /// @brief When the drain watch wants a marker
    int64_t drain_watch_us(uint32_t markers_queued) const;

    /// @brief What a marker at the head wants for the role
    sched::Want want(const StreamSource* source, sched::RoleSet roles_ahead, int64_t now_us) const;

    /// @brief What a start from empty needs besides the desired state
    sched::StartPreconditions preconditions() const;

    const sched::StreamSide& sent() const {
        return this->sent_;
    }

    /// @brief The configuration a start or a clear with want announces
    VisualizerConfig config_for(const sched::Want& want) const;

    /// @brief A marker's message for the role, with want, taken for writing at stamp_us: its
    /// effects on sent state, ledger and cursor
    void commit_marker(StreamSource* source, sched::StreamAction action, const sched::Want& want,
                       int64_t stamp_us);

    /// @brief Whether the cursor's next item is an in-place stream/start: the configuration
    /// changed, and the new configuration's analysis holds a cell past the last message sent,
    /// with messages or none; after prepare()
    bool announce_due(const StreamSource* source) const;

    /// @brief Builds the in-place stream/start with the stamp; the cursor moves to the new
    /// configuration's messages after the last `ts` sent, with no ledger reset (spec
    /// roles/visualizer/v1.md "Visualizer messages": timestamps do not decrease)
    std::optional<OutboundPlaintext> commit_announce(StreamSource* source, int64_t stamp_us);

    /// @brief The next message, counted in the ledger; only when candidate() reports one ready
    OutboundPlaintext take_message(const StreamSource* source);

    // ------------------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------------------

    /// @brief A visualizer stream is active as of the messages taken for writing
    bool active() const {
        return this->sent_.active;
    }

    bool role_written() const {
        return this->role_written_;
    }

    /// @brief In the group's eligible set: the role written, available, its client/state object
    /// received since the activation
    bool eligible() const {
        return this->role_written_ && this->available_ && this->state_since_activation_;
    }

    /// @brief The configuration the client's latest request gives; nullopt before one
    const std::optional<VisualizerConfig>& wanted() const {
        return this->wanted_;
    }

    /// @brief The configuration whose messages the cursor reads, which an in-place stream/start
    /// may still announce
    const VisualizerConfig& cursor_config() const {
        return this->announce_ ? *this->announce_ : this->sent_config_;
    }

private:
    /// The next message at the cursor
    struct Message {
        const sched::Frame* cell{nullptr};
        size_t offset{0};
        size_t bytes{0};
        int64_t ts_us{0};
    };

    /// The ring of the configuration the cursor reads, while it holds the sent epoch
    const sched::FrameRing* cursor_ring(const StreamSource* source) const;
    const sched::FrameRing* ring_of(const StreamSource* source,
                                    const VisualizerConfig& config) const;
    /// The message at the cursor in ring, or the first after it; nullopt when none is produced
    std::optional<Message> message_at(const sched::FrameRing& ring, uint32_t bins) const;
    /// The wanted configuration, unless its analysis failed
    std::optional<VisualizerConfig> usable_wanted(const StreamSource* source) const;
    /// The startup lead after a trigger of kind: `J_min` after a group trigger, the group lead `G`
    /// if more after a join
    int64_t startup_lead_us(const StreamSource* source, sched::TriggerKind kind) const;
    /// Points the cursor at the first message the startup lead past stamp_us
    void bind_cursor(const StreamSource* source, int64_t stamp_us);

    std::optional<VisualizerSupport> support_;
    std::optional<VisualizerConfig> wanted_;
    sched::StreamSide sent_;
    /// The configuration of the last stream/start written
    VisualizerConfig sent_config_;
    /// A configuration change waiting for its in-place stream/start
    std::optional<VisualizerConfig> announce_;
    sched::Cursor cursor_;
    /// The byte offset of the next message in the cursor's cell, in the ring of offset_ring_: a
    /// change of configuration moves the cursor to another ring, whose cells it reads from their
    /// start
    size_t offset_{0};
    sched::InstanceId offset_ring_;
    sched::Ledger ledger_{0, false};
    std::optional<sched::TriggerKind> pending_kind_;
    bool role_written_{false};
    bool state_since_activation_{false};
    bool available_{false};
    sched::SchedTiming timing_;
};

}  // namespace sendspin::server
