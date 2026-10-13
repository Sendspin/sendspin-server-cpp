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

#include "sched/run_view.h"

namespace sendspin::server::sched {

namespace {

/// Clears the staged drain, if any, resolving every call it answers with outcome
std::optional<DrainResolution> resolve(RunView& view, DrainOutcome outcome) {
    if (!view.drain.has_value()) {
        return std::nullopt;
    }
    const DrainResolution resolution{outcome, view.drain->calls};
    view.drain.reset();
    return resolution;
}

/// Ends the run: the next write begins a new one
void end_run(RunView& view) {
    view.run = RunState::NONE;
}

}  // namespace

void view_anchor_write(RunView& view, RunId new_run, EpochId new_epoch) {
    if (view.run == RunState::NONE) {
        view.run_id = new_run;
        view.epoch = new_epoch;
        view.run = RunState::PLAYING;
    } else if (view.run == RunState::PENDING) {
        view.run = RunState::PLAYING;
    }
}

std::optional<DrainResolution> view_drain(RunView& view) {
    if (view.latched) {
        return DrainResolution{DrainOutcome::STOPPED, 1};
    }
    if (view.run == RunState::NONE) {
        return DrainResolution{DrainOutcome::PLAYED, 1};
    }
    if (view.drain.has_value()) {
        ++view.drain->calls;
    } else {
        view.drain = StagedDrain{1};
    }
    return std::nullopt;
}

std::optional<DrainResolution> view_flush(RunView& view, EpochId new_epoch) {
    if (view.latched || view.run == RunState::NONE) {
        return std::nullopt;
    }
    view.epoch = new_epoch;
    if (view.run != RunState::PAUSED) {
        view.run = RunState::PENDING;
    }
    return resolve(view, DrainOutcome::FLUSHED);
}

bool view_pause(RunView& view) {
    if (view.run != RunState::PLAYING && view.run != RunState::PENDING) {
        return false;
    }
    view.run = RunState::PAUSED;
    return true;
}

bool view_resume(RunView& view, RunId new_run, EpochId new_epoch, bool input_since_p0) {
    if (view.run != RunState::PAUSED) {
        return false;
    }
    view.run_id = new_run;
    view.epoch = new_epoch;
    view.run = input_since_p0 ? RunState::PLAYING : RunState::PENDING;
    return true;
}

std::optional<DrainResolution> view_stop(RunView& view) {
    view.latched = false;
    end_run(view);
    return resolve(view, DrainOutcome::STOPPED);
}

ServerStopResult view_server_stop(RunView& view) {
    if (view.run != RunState::PLAYING && view.run != RunState::PENDING) {
        return {};
    }
    ServerStopResult result;
    result.applied = true;
    result.stopped_epoch = view.epoch;
    view.latched = true;
    end_run(view);
    result.drain = resolve(view, DrainOutcome::STOPPED);
    return result;
}

std::optional<DrainResolution> view_drain_complete(RunView& view, EpochId draining_epoch) {
    if (!view.drain.has_value() || view.epoch != draining_epoch ||
        (view.run != RunState::PLAYING && view.run != RunState::PENDING)) {
        return std::nullopt;
    }
    end_run(view);
    return resolve(view, DrainOutcome::PLAYED);
}

}  // namespace sendspin::server::sched
