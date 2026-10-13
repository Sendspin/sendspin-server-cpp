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

#include "sched/selection.h"

#include <algorithm>

namespace sendspin::server::sched {

Choice select_next(const PumpSnapshot& snapshot) {
    if (snapshot.fragment_in_progress) {
        return Choice::FRAGMENT;
    }
    if (snapshot.phase == Phase::AWAIT_MSG2) {
        return Choice::NONE;
    }
    if (snapshot.phase == Phase::AWAIT_ACTIVATE) {
        return Choice::REKEY_ACTIVATE;
    }
    if (snapshot.time_pending) {
        return Choice::TIME;
    }
    if (snapshot.control_pending) {
        return Choice::CONTROL;
    }
    if (snapshot.artwork_ready && snapshot.artwork_aged) {
        return Choice::ARTWORK;
    }
    const AudioCandidate& player = snapshot.player;
    const AudioCandidate& visualizer = snapshot.visualizer;
    if (player.ready && (!visualizer.ready || player.deadline_us <= visualizer.deadline_us)) {
        return Choice::PLAYER;
    }
    if (visualizer.ready) {
        return Choice::VISUALIZER;
    }
    if (snapshot.artwork_ready) {
        return Choice::ARTWORK;
    }
    return Choice::NONE;
}

int64_t next_wake(const PumpSnapshot& snapshot) {
    if (snapshot.phase == Phase::AWAIT_MSG2) {
        return NEVER;
    }
    return std::min({snapshot.player.room_time_us, snapshot.visualizer.room_time_us,
                     snapshot.artwork_wake_us, snapshot.drain_watch_us});
}

bool artwork_aged(int64_t head_queued_us, std::optional<int64_t> last_aged_write_us,
                  int64_t artwork_age_us, int64_t now_us) {
    return now_us - head_queued_us >= artwork_age_us &&
           (!last_aged_write_us.has_value() || now_us - *last_aged_write_us >= artwork_age_us);
}

ItemState item_ready(const CursorView& cursor, const Ledger& ledger, int64_t now_us) {
    if (!cursor.phase_normal || !cursor.stream_active) {
        return {};
    }
    if (cursor.switch_point.has_value() && cursor.next == *cursor.switch_point) {
        const bool ready = cursor.target_holds_switch_point && cursor.available;
        return {ready ? ItemKind::SWITCH_START : ItemKind::STARVED};
    }
    if (cursor.final_index.has_value() && cursor.next > *cursor.final_index) {
        return {ItemKind::FINISHED};
    }
    if (!cursor.next_payload_bytes.has_value()) {
        return {ItemKind::STARVED};
    }
    const int64_t bytes = cursor.header_bytes + *cursor.next_payload_bytes;
    if (ledger.admits(bytes, now_us)) {
        return {ItemKind::CHUNK};
    }
    return {ItemKind::BLOCKED, ledger.room_time(bytes, now_us)};
}

AudioCandidate audio_candidate(const ItemState& state, int64_t deadline_us) {
    const bool ready = state.kind == ItemKind::CHUNK || state.kind == ItemKind::SWITCH_START;
    const bool waits = state.kind == ItemKind::BLOCKED || state.kind == ItemKind::HELD;
    return {ready, deadline_us, waits ? state.room_time_us : NEVER};
}

int64_t drain_watch_time(bool active, bool final_done, uint32_t markers_queued,
                         const Ledger& ledger) {
    if (!active || !final_done || markers_queued != 0) {
        return NEVER;
    }
    return ledger.last_completion();
}

bool drain_watch_due(bool active, bool final_done, uint32_t markers_queued, const Ledger& ledger,
                     int64_t now_us) {
    return active && final_done && markers_queued == 0 && ledger.count(now_us) == 0;
}

}  // namespace sendspin::server::sched
