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

/// @file run_view.h
/// @brief The run, stop latch and staged drain of an input's host view, and how each host call and
/// core event changes them. Each function is one step under the host-view mutex, so whichever
/// resolves a staged drain first decides its one outcome.

#pragma once

#include "sched/types.h"

#include <cstdint>
#include <optional>

namespace sendspin::server::sched {

/// @brief A drain() waiting for its outcome. It drains the view's current epoch: a flush or a
/// stop resolves it, and a resume carries it into the resumed epoch.
struct StagedDrain {
    /// The drain() calls it answers, each with its own on_drained
    uint32_t calls{1};
};

struct RunView {
    RunState run{RunState::NONE};
    /// A server-initiated stop the host has not acknowledged with stop()
    bool latched{false};
    /// The current run; kept while paused, so a resume's new run id replaces it
    RunId run_id;
    /// The current epoch, a pending one included
    EpochId epoch;
    std::optional<StagedDrain> drain;
};

/// @brief A resolved drain: one on_drained per call, all with this outcome
struct DrainResolution {
    DrainOutcome outcome{DrainOutcome::PLAYED};
    uint32_t callbacks{0};
};

/// @brief The write that anchors: with no run it begins one, taking new_run and new_epoch; a
/// pending epoch it anchors in place, keeping its ids
void view_anchor_write(RunView& view, RunId new_run, EpochId new_epoch);

/// @brief drain(): resolves at once with no run (PLAYED) or under a latch (STOPPED, the latch
/// kept); joins a staged drain; otherwise stages one for the current epoch
std::optional<DrainResolution> view_drain(RunView& view);

/// @brief flush(): a new pending epoch, cancelling a staged drain (FLUSHED); a paused view stays
/// paused. No effect while latched or with no run.
std::optional<DrainResolution> view_flush(RunView& view, EpochId new_epoch);

/// @brief pause(): a playing or pending run is paused, keeping a staged drain; false when there
/// was nothing to pause
bool view_pause(RunView& view);

/// @brief resume(): a paused input begins new_run and new_epoch, carrying a staged drain. It
/// anchors at once when input exists at or after the epoch's `p0`; otherwise the epoch stays
/// pending for the first write, whichever of pause() and flush() came first. False when not
/// paused.
/// @param input_since_p0 the host view's head is past `p0`
bool view_resume(RunView& view, RunId new_run, EpochId new_epoch, bool input_since_p0);

/// @brief The host's stop(): ends any run, discards a staged drain (STOPPED) and clears a latch
std::optional<DrainResolution> view_stop(RunView& view);

struct ServerStopResult {
    /// False when paused (playback is already stopped and resume() still works) or with no run
    bool applied{false};
    /// `E_stop`: the epoch core ignores late events up to
    EpochId stopped_epoch;
    std::optional<DrainResolution> drain;
};

/// @brief A server-initiated stop: ends a playing or pending run, discards a staged drain
/// (STOPPED) and latches
ServerStopResult view_server_stop(RunView& view);

/// @brief Core's drain completion for draining_epoch: delivered only while that epoch is still
/// the view's, its drain is still staged and the view is not paused; the run then ends (PLAYED)
std::optional<DrainResolution> view_drain_complete(RunView& view, EpochId draining_epoch);

}  // namespace sendspin::server::sched
