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

/// @file send_queues.h
/// @brief One connection's time queue and control FIFO. Entries hold no stamp; a time reply is
/// built when it is taken. Core thread only.

#pragma once

#include "memory.h"
#include "message_kind.h"
#include "messages.h"
#include "ring.h"
#include "sched/selection.h"
#include "sched/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace sendspin::server {

/// @brief The time queue's cap, in entries
constexpr size_t DEFAULT_TIME_QUEUE_ENTRIES = 64;
/// @brief The control FIFO's cap, in entries and in plaintext bytes
constexpr size_t DEFAULT_CONTROL_QUEUE_ENTRIES = 256;
constexpr size_t DEFAULT_CONTROL_QUEUE_BYTES = size_t{1024} * 1024;

/// @brief Per-connection queue caps. Overflowing one is a misbehaving client.
struct SendQueueCaps {
    size_t time_entries{DEFAULT_TIME_QUEUE_ENTRIES};
    /// Markers count as entries
    size_t control_entries{DEFAULT_CONTROL_QUEUE_ENTRIES};
    /// Counts each queued message, and the one being fragmented until its last fragment is taken
    size_t control_bytes{DEFAULT_CONTROL_QUEUE_BYTES};
};

/// @brief One outbound WebSocket message before encryption
struct OutboundPlaintext {
    MessageKind kind{MessageKind::TEXT};
    /// TEXT: the wire bytes. BINARY: the plaintext, message ID first.
    std::vector<uint8_t> bytes;
};

/// @brief A JSON message as a binary plaintext, its message ID first (messaging.md "Binary
/// Message ID Structure")
OutboundPlaintext json_plaintext(std::string_view json);

/// @brief What a control FIFO entry is
enum class ControlKind : uint8_t {
    MESSAGE,
    /// A server/activate, which the markers ahead of it evaluate against
    ACTIVATION,
    /// A desired-state marker: emits the stream messages that close the gap when it reaches the
    /// head
    MARKER,
    /// A re-handshake's start, carrying the server/activate frozen for after it, which the markers
    /// ahead of it evaluate against
    REKEY,
};

class SendQueues {
public:
    /// @brief Allocates both queues at their caps' entries, in PSRAM where there is one
    explicit SendQueues(const SendQueueCaps& caps);

    /// @return false when the time queue is full; nothing is queued then
    [[nodiscard]] bool push_time(const ClientTime& request);

    /// @return false when the message would pass a control cap; nothing is queued then
    [[nodiscard]] bool push_control(OutboundPlaintext message);

    /// @param roles the stream roles of its active_roles
    /// @return false when it would pass a control cap
    [[nodiscard]] bool push_activation(OutboundPlaintext message, sched::RoleSet roles);

    /// @param activation the frozen server/activate
    /// @param roles the stream roles of its active_roles
    /// @return false when it would pass a control cap
    [[nodiscard]] bool push_rekey(OutboundPlaintext activation, sched::RoleSet roles);

    /// @brief Appends a marker unless the tail is one already, which runs after everything queued
    /// and so covers the change
    /// @return false when it would pass the entry cap
    [[nodiscard]] bool push_marker();

    /// @brief The selection inputs the queues decide: a fragment in progress, time, control
    sched::PumpSnapshot snapshot() const;

    /// @brief select_next() over the queues alone
    sched::Choice next() const {
        return sched::select_next(this->snapshot());
    }

    /// @brief Pops the oldest time request. Only when the time queue is selected.
    ClientTime take_time();

    /// @brief The head's kind. Only when the control FIFO is selected and no fragment is in
    /// progress.
    ControlKind head_kind() const;

    /// @brief The stream roles of the activation or rekey at the head
    sched::RoleSet head_roles() const;

    /// @brief `roles_ahead` for the marker at the head: the first activation or rekey behind it,
    /// else written
    sched::RoleSet roles_ahead(sched::RoleSet written) const;

    /// @brief Pops the marker at the head
    void pop_marker();

    /// @brief Pops the rekey at the head and returns its frozen activation, whole. Only when the
    /// control FIFO is selected and the head is a rekey.
    OutboundPlaintext take_rekey();

    /// @brief The head message or activation, or the next fragment of the message in progress.
    /// A BINARY message larger than one Noise transport message starts fragmenting here. Only
    /// when the control FIFO or a fragment is selected and the head is not a marker.
    /// @param max_fragment_plaintext as write_fragment()'s max_plaintext
    OutboundPlaintext take_control(size_t max_fragment_plaintext);

    /// @brief Markers queued, which suppress the drain watch
    uint32_t markers_queued() const {
        return this->markers_;
    }

    void clear();

private:
    struct ControlItem {
        ControlKind kind{ControlKind::MESSAGE};
        OutboundPlaintext message;
        /// ACTIVATION and REKEY: the activation's stream roles
        sched::RoleSet roles;
    };

    struct Fragmenting {
        OutboundPlaintext message;
        /// Offset of the next fragment into the payload after the ID byte
        size_t offset{0};
    };

    bool push(ControlItem item);

    /// @param fragmenting the message in progress, *fragmenting_
    OutboundPlaintext next_fragment(Fragmenting& fragmenting, size_t max_fragment_plaintext);

    SendQueueCaps caps_;
    Ring<ClientTime> time_;
    Ring<ControlItem> control_;
    std::optional<Fragmenting> fragmenting_;
    size_t control_bytes_{0};
    uint32_t markers_{0};
};

}  // namespace sendspin::server
