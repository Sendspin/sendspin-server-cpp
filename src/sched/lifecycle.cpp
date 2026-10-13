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

#include "sched/lifecycle.h"

namespace sendspin::server::sched {

Want evaluate_want(const WantInputs& in) {
    Want want;
    want.run = in.run;
    want.epoch = in.epoch;
    if (in.format.has_value()) {
        want.format = *in.format;
    }
    want.active = in.role_written && in.role_ahead && in.group_playing &&
                  !(in.epoch_draining && in.drain_done) && in.available && in.format.has_value();
    return want;
}

RoleSet roles_ahead(std::span<const ControlItemView> behind, RoleSet roles_written) {
    for (const ControlItemView& item : behind) {
        if (item.kind == ControlItemKind::ACTIVATION || item.kind == ControlItemKind::REKEY) {
            return item.roles;
        }
    }
    return roles_written;
}

StreamAction reconcile_stream(const StreamSide& sent, const Want& want,
                              const StartPreconditions& pre) {
    if (sent.active) {
        if (!want.active || sent.run != want.run) {
            return StreamAction::END;
        }
        if (sent.epoch != want.epoch) {
            return StreamAction::CLEAR;
        }
        return StreamAction::NONE;
    }
    if (want.active && pre.available && pre.role_written && pre.state_since_activation &&
        pre.trigger_pending) {
        return StreamAction::START;
    }
    return StreamAction::NONE;
}

MarkerEmission marker_step(const std::array<StreamSide, STREAM_ROLE_COUNT>& sent,
                           const std::array<Want, STREAM_ROLE_COUNT>& want,
                           const std::array<StartPreconditions, STREAM_ROLE_COUNT>& pre) {
    std::array<StreamAction, STREAM_ROLE_COUNT> actions{};
    for (size_t i = 0; i < STREAM_ROLE_COUNT; ++i) {
        actions[i] = reconcile_stream(sent[i], want[i], pre[i]);
    }
    for (const StreamAction action :
         {StreamAction::END, StreamAction::CLEAR, StreamAction::START}) {
        MarkerEmission emission{action, {}};
        for (size_t i = 0; i < STREAM_ROLE_COUNT; ++i) {
            if (actions[i] == action) {
                emission.roles = emission.roles.with(static_cast<StreamRole>(i));
            }
        }
        if (!emission.roles.empty()) {
            return emission;
        }
    }
    return {};
}

void apply_action(StreamSide& sent, const Want& want, StreamAction action) {
    switch (action) {
        case StreamAction::NONE:
            break;
        case StreamAction::END:
            sent.active = false;
            break;
        case StreamAction::CLEAR:
            sent.epoch = want.epoch;
            break;
        case StreamAction::START:
            sent = {true, want.run, want.epoch, want.format};
            break;
    }
}

bool drain_done(int64_t cursor_next, std::optional<int64_t> own_final_index, const Ledger& ledger,
                int64_t now_us) {
    return own_final_index.has_value() && cursor_next > *own_final_index &&
           ledger.count(now_us) == 0;
}

bool start_finds_audio(int64_t now_us, int64_t startup_lead_us, std::optional<int64_t> last_ts_us) {
    return last_ts_us.has_value() && now_us + startup_lead_us <= *last_ts_us;
}

bool role_drain_done(bool active, int64_t cursor_next, std::optional<int64_t> own_final_index,
                     const Ledger& ledger, int64_t startup_lead_us,
                     std::optional<int64_t> last_ts_us, int64_t now_us) {
    if (active) {
        return drain_done(cursor_next, own_final_index, ledger, now_us);
    }
    return !start_finds_audio(now_us, startup_lead_us, last_ts_us);
}

bool drain_complete(const GroupDrainView& group, int64_t join_min_us, int64_t now_us) {
    return group.epoch_draining && group.run_playing && !group.any_stream_active &&
           (!group.last_ts_us.has_value() || now_us >= *group.last_ts_us - join_min_us);
}

}  // namespace sendspin::server::sched
