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

/// @file selection.h
/// @brief The pump's choice of the next message, whether a cursor has an item, and the session
/// timer's deadline. Pure functions over a snapshot.

#pragma once

#include "sched/constants.h"
#include "sched/ledger.h"

#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief A session's re-handshake phase
enum class Phase : uint8_t {
    NORMAL,
    /// Noise message 1 is written; nothing is selectable
    AWAIT_MSG2,
    /// Message 2 is processed; only the rekey item's frozen server/activate is selectable
    AWAIT_ACTIVATE,
};

enum class Choice : uint8_t {
    NONE,
    /// The next fragment of the message in progress
    FRAGMENT,
    /// The frozen server/activate after a re-handshake
    REKEY_ACTIVATE,
    TIME,
    CONTROL,
    ARTWORK,
    PLAYER,
    VISUALIZER,
};

/// @brief What one cursor has for the pump
struct AudioCandidate {
    /// A chunk, a visualizer message or an in-place stream/start is ready
    bool ready{false};
    /// The ready item's deadline: `ts(next) - od` for a player chunk, `ts` for a visualizer message
    int64_t deadline_us{0};
    /// When the cursor's ledger has room for its next item, if that is what it waits for
    int64_t room_time_us{NEVER};
};

/// @brief Everything select_next() and next_wake() decide from
struct PumpSnapshot {
    bool fragment_in_progress{false};
    Phase phase{Phase::NORMAL};
    bool time_pending{false};
    bool control_pending{false};
    bool artwork_ready{false};
    /// artwork_aged() holds for the artwork head
    bool artwork_aged{false};
    AudioCandidate player;
    AudioCandidate visualizer;
    /// The artwork module's next pacing or aging instant
    int64_t artwork_wake_us{NEVER};
    /// The earliest drain watch over the session's stream roles (drain_watch_time())
    int64_t drain_watch_us{NEVER};
};

/// @brief The next message's source: the rest of a fragmented message, nothing during a
/// re-handshake freeze but the frozen activation, then time replies, then the control FIFO, then
/// one aged artwork message, then the cursors by earliest deadline (a tie goes to the player), then
/// artwork
Choice select_next(const PumpSnapshot& snapshot);

/// @brief The session timer's deadline when select_next() returns NONE: the earliest room time,
/// artwork instant and drain watch; NEVER while awaiting Noise message 2
int64_t next_wake(const PumpSnapshot& snapshot);

/// @brief The artwork head may go ahead of the cursors: it has waited `A_age`, and no aged artwork
/// message was written in the last `A_age`
bool artwork_aged(int64_t head_queued_us, std::optional<int64_t> last_aged_write_us,
                  int64_t artwork_age_us, int64_t now_us);

/// @brief What a cursor's next item is
enum class ItemKind : uint8_t {
    /// No active stream, or the phase allows no audio
    IDLE,
    /// A chunk fits the ledger
    CHUNK,
    /// The in-place stream/start at a due switch point; it needs no ledger room
    SWITCH_START,
    /// The next frame exists but the ledger has no room for it
    BLOCKED,
    /// A chunk fits, but one short of `n_max` waits for its frames or room while it has the slack;
    /// PlayerStream decides it from the frames
    HELD,
    /// The frame (or, at a due switch, the target's frame) is not produced
    STARVED,
    /// Past its own instance's final index
    FINISHED,
};

struct ItemState {
    ItemKind kind{ItemKind::IDLE};
    /// With BLOCKED: when the ledger has room; with HELD: when the chunk goes anyway, or the ledger
    /// has room for one more frame
    int64_t room_time_us{NEVER};
};

/// @brief A cursor's state that item_ready() reads, after its step
struct CursorView {
    bool phase_normal{false};
    bool stream_active{false};
    /// The latest client/state reports `available: true`
    bool available{false};
    int64_t next{0};
    std::optional<int64_t> final_index;
    /// Set while a switch to another instance is pending
    std::optional<int64_t> switch_point;
    /// The target ring holds the frame at the switch point
    bool target_holds_switch_point{false};
    /// The payload size of the frame at next; nullopt when it is not produced
    std::optional<int64_t> next_payload_bytes;
    /// What the ledger counts besides the payload: a chunk's header, or nothing for a visualizer
    /// message, whose payload is the whole message
    int64_t header_bytes{CHUNK_HEADER_BYTES};
};

/// @brief The one predicate select_next() and next_wake() share for a cursor
ItemState item_ready(const CursorView& cursor, const Ledger& ledger, int64_t now_us);

/// @brief The snapshot entry for a cursor in state with the given deadline
AudioCandidate audio_candidate(const ItemState& state, int64_t deadline_us);

/// @brief A stream's drain watch: its ledger's last completion while the stream is active, past its
/// final index and no marker for the session is queued; NEVER otherwise
int64_t drain_watch_time(bool active, bool final_done, uint32_t markers_queued,
                         const Ledger& ledger);

/// @brief prepare() appends a marker for the drain watch: the watch applies and the ledger is empty
bool drain_watch_due(bool active, bool final_done, uint32_t markers_queued, const Ledger& ledger,
                     int64_t now_us);

}  // namespace sendspin::server::sched
