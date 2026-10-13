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

#include "visualizer_stream.h"

#include "fragmentation.h"
#include "sched/timeline.h"

#include <algorithm>
#include <utility>

namespace sendspin::server {

namespace {

/// The big-endian timestamp of the message at offset
int64_t message_ts(const std::vector<uint8_t>& payload, size_t offset) {
    uint64_t ts = 0;
    for (size_t i = 1; i < VISUALIZER_HEADER_BYTES; ++i) {
        ts = (ts << 8) | payload[offset + i];
    }
    return static_cast<int64_t>(ts);
}

uint32_t bins_of(const VisualizerConfig& config) {
    return config.spectrum ? config.spectrum->n_disp_bins : 0;
}

}  // namespace

// ==============================================================================
// Inputs
// ==============================================================================

void VisualizerStream::set_support(const VisualizerSupport& support) {
    this->support_ = support;
    // spec roles/visualizer/v1.md "client/hello visualizer@v1 support object"
    this->ledger_ = sched::Ledger(support.buffer_capacity, false);
}

std::optional<sched::TriggerKind> VisualizerStream::on_activation_written(bool visualizer_active) {
    const bool added = visualizer_active && !this->role_written_;
    this->role_written_ = visualizer_active;
    if (added) {
        // messaging.md "stream/start": wait for the client/state the activation requires
        this->state_since_activation_ = false;
        return sched::TriggerKind::JOIN;
    }
    if (!visualizer_active && this->sent_.active) {
        // A marker ahead of every activation ends a removed role's stream first, so this is a bug;
        // the stream is dropped all the same
        this->sent_.active = false;
        this->ledger_.reset();
        this->cursor_ = {};
        this->offset_ = 0;
        this->announce_.reset();
    }
    return std::nullopt;
}

std::optional<sched::TriggerKind> VisualizerStream::on_client_state(const ClientState& state) {
    this->available_ = state.available;
    std::optional<sched::TriggerKind> kind;
    // messaging.md "server/activate": an inactive role's object is ignored
    if (this->role_written_ && state.visualizer) {
        if (!this->state_since_activation_) {
            kind = sched::TriggerKind::JOIN;
        }
        this->state_since_activation_ = true;
        this->wanted_ = negotiate_visualizer(*state.visualizer,
                                             this->support_ ? this->support_->buffer_capacity : 0);
    }
    return kind;
}

// ==============================================================================
// The pump
// ==============================================================================

bool VisualizerStream::prepare(StreamSource* source, uint32_t markers_queued, int64_t now_us) {
    this->ledger_.prune(now_us);
    if (!this->sent_.active || source == nullptr) {
        return false;
    }
    const StreamSource::Playback playback = source->playback();
    // spec roles/visualizer/v1.md "client/state visualizer object": a changed configuration is
    // announced in place; a stream about to end or clear gets its configuration from the marker
    if (this->sent_.run == playback.run && this->sent_.epoch == playback.epoch) {
        const std::optional<VisualizerConfig> wanted = this->usable_wanted(source);
        if (wanted && *wanted != this->cursor_config()) {
            if (*wanted == this->sent_config_) {
                this->announce_.reset();
            } else {
                this->announce_ = wanted;
                // spec roles/visualizer/v1.md "Visualizer messages": an update does not let
                // timestamps decrease
                this->cursor_.min_ts = std::max(this->cursor_.min_ts, this->cursor_.last_ts + 1);
            }
            // The group builds the new configuration's analysis and drops an abandoned one
            source->on_stream_changed();
        }
    }
    const sched::FrameRing* ring = this->cursor_ring(source);
    if (ring == nullptr) {
        this->cursor_.final_done = false;
        return false;
    }
    // No message with `ts <= now`, nor before the trigger's `min_ts`
    if (this->cursor_.next < ring->base() || ring->instance() != this->offset_ring_) {
        this->cursor_.next = std::max(this->cursor_.next, ring->base());
        this->offset_ = 0;
        this->offset_ring_ = ring->instance();
    }
    const uint32_t bins = bins_of(this->cursor_config());
    for (const sched::Frame* cell = ring->at(this->sent_.epoch, this->cursor_.next);
         cell != nullptr; cell = ring->at(this->sent_.epoch, this->cursor_.next)) {
        if (this->offset_ < cell->payload.size()) {
            const int64_t ts = message_ts(cell->payload, this->offset_);
            if (ts > now_us && ts >= this->cursor_.min_ts) {
                break;
            }
            const uint8_t type = visualizer_type_of(cell->payload[this->offset_]);
            this->offset_ += visualizer_message_bytes(type, bins);
            continue;
        }
        ++this->cursor_.next;
        this->offset_ = 0;
    }
    const std::optional<int64_t> final_index = ring->final_index();
    this->cursor_.final_done = final_index && this->cursor_.next > *final_index;
    return sched::drain_watch_due(this->sent_.active, this->cursor_.final_done, markers_queued,
                                  this->ledger_, now_us);
}

sched::AudioCandidate VisualizerStream::candidate(const StreamSource* source,
                                                  int64_t now_us) const {
    sched::CursorView view;
    view.phase_normal = true;
    view.stream_active = this->sent_.active;
    view.available = this->available_;
    view.next = this->cursor_.next;
    view.header_bytes = 0;
    int64_t deadline = now_us;
    if (const sched::FrameRing* ring = this->cursor_ring(source)) {
        view.final_index = ring->final_index();
        const std::optional<Message> message =
            this->message_at(*ring, bins_of(this->cursor_config()));
        if (message) {
            view.next_payload_bytes = static_cast<int64_t>(message->bytes);
            // A visualizer message's deadline is its `ts`
            deadline = message->ts_us;
        }
        if (this->announce_) {
            view.switch_point = this->cursor_.next;
            view.target_holds_switch_point = this->announce_due(source);
        }
    }
    return sched::audio_candidate(sched::item_ready(view, this->ledger_, now_us), deadline);
}

int64_t VisualizerStream::drain_watch_us(uint32_t markers_queued) const {
    return sched::drain_watch_time(this->sent_.active, this->cursor_.final_done, markers_queued,
                                   this->ledger_);
}

sched::Want VisualizerStream::want(const StreamSource* source, sched::RoleSet roles_ahead,
                                   int64_t now_us) const {
    const StreamSource::Playback playback =
        source != nullptr ? source->playback() : StreamSource::Playback{};
    sched::WantInputs in;
    in.role_written = this->role_written_;
    in.role_ahead = roles_ahead.contains(sched::StreamRole::VISUALIZER);
    in.group_playing = playback.playing;
    in.run = playback.run;
    in.epoch = playback.epoch;
    in.epoch_draining = playback.draining;
    in.available = this->available_;
    // A stream on a failed configuration ends
    const auto failed = [&](const VisualizerConfig& config) {
        return std::find(playback.failed_visualizers.begin(), playback.failed_visualizers.end(),
                         config_key(config)) != playback.failed_visualizers.end();
    };
    const std::optional<VisualizerConfig> wanted = this->usable_wanted(source);
    if (wanted && (!this->sent_.active || !failed(this->cursor_config()))) {
        in.format = sched::FormatKey{{}, config_key(*wanted)};
    }
    if (playback.draining) {
        // A visualizer start's lead is its startup lead, and its messages follow the cell grid
        std::optional<int64_t> last_ts;
        if (playback.last_cell && playback.anchor != nullptr) {
            last_ts = sched::last_frame_ts(*playback.anchor, *playback.last_cell, 1);
        }
        const sched::FrameRing* ring = this->cursor_ring(source);
        in.drain_done = sched::role_drain_done(
            this->sent_.active, this->cursor_.next,
            ring != nullptr ? ring->final_index() : std::nullopt, this->ledger_,
            this->startup_lead_us(source, this->pending_kind_.value_or(sched::TriggerKind::JOIN)),
            last_ts, now_us);
    }
    return sched::evaluate_want(in);
}

sched::StartPreconditions VisualizerStream::preconditions() const {
    return {this->available_, this->role_written_, this->state_since_activation_,
            this->pending_kind_.has_value()};
}

VisualizerConfig VisualizerStream::config_for(const sched::Want& want) const {
    // want.format is wanted()'s key whenever a marker emits for the role
    return this->wanted_ && config_key(*this->wanted_) == want.format.visualizer_config
               ? *this->wanted_
               : this->sent_config_;
}

void VisualizerStream::commit_marker(StreamSource* source, sched::StreamAction action,
                                     const sched::Want& want, int64_t stamp_us) {
    switch (action) {
        case sched::StreamAction::END:
            this->cursor_ = {};
            this->offset_ = 0;
            this->announce_.reset();
            break;
        case sched::StreamAction::CLEAR: {
            // A changed configuration is announced in place before the new epoch's first message
            const VisualizerConfig config = this->config_for(want);
            if (config == this->sent_config_) {
                this->announce_.reset();
            } else {
                this->announce_ = config;
            }
            this->bind_cursor(source, stamp_us);
            break;
        }
        case sched::StreamAction::START:
            this->sent_config_ = this->config_for(want);
            this->announce_.reset();
            this->bind_cursor(source, stamp_us);
            break;
        case sched::StreamAction::NONE:
            break;
    }
    sched::apply_action(this->sent_, want, action);
    // spec roles/visualizer/v1.md "client/hello visualizer@v1 support object": a stream/clear or
    // stream/end resets the count
    this->ledger_.reset();
    if (source != nullptr) {
        source->on_stream_changed();
    }
}

bool VisualizerStream::announce_due(const StreamSource* source) const {
    if (!this->announce_) {
        return false;
    }
    // A cell of the new configuration past the last message sent, with messages or none: a
    // configuration whose cells carry no message (`peak` alone, or no type) is announced too
    const sched::FrameRing* ring = this->cursor_ring(source);
    if (ring == nullptr) {
        return false;
    }
    const sched::Frame* newest = ring->at(this->sent_.epoch, ring->next_expected() - 1);
    return newest != nullptr && newest->ts_us > this->cursor_.last_ts;
}

std::optional<OutboundPlaintext> VisualizerStream::commit_announce(StreamSource* source,
                                                                   int64_t stamp_us) {
    if (!this->announce_ || !this->announce_due(source)) {
        return std::nullopt;
    }
    this->sent_config_ = *this->announce_;
    this->announce_.reset();
    // spec roles/visualizer/v1.md "client/hello visualizer@v1 support object": only a
    // stream/clear or stream/end resets the count
    this->sent_.format = sched::FormatKey{{}, config_key(this->sent_config_)};
    if (source != nullptr) {
        source->on_stream_changed();
    }
    return json_plaintext(
        build_stream_start(stamp_us, {std::nullopt, std::nullopt, this->sent_config_}));
}

OutboundPlaintext VisualizerStream::take_message(const StreamSource* source) {
    OutboundPlaintext message{MessageKind::BINARY, {}};
    const sched::FrameRing* ring = this->cursor_ring(source);
    if (ring == nullptr) {
        return message;
    }
    const std::optional<Message> next = this->message_at(*ring, bins_of(this->cursor_config()));
    if (!next) {
        return message;
    }
    const auto from = next->cell->payload.begin() + static_cast<std::ptrdiff_t>(next->offset);
    reserve_plaintext(message.bytes, next->bytes);
    message.bytes.assign(from, from + static_cast<std::ptrdiff_t>(next->bytes));
    // Counted until its own timestamp
    this->ledger_.add(next->ts_us, static_cast<int64_t>(next->bytes));
    this->cursor_.last_ts = next->ts_us;
    this->cursor_.next = next->cell->index;
    this->offset_ = next->offset + next->bytes;
    this->offset_ring_ = ring->instance();
    return message;
}

// ==============================================================================
// Helpers
// ==============================================================================

const sched::FrameRing* VisualizerStream::cursor_ring(const StreamSource* source) const {
    if (!this->sent_.active) {
        return nullptr;
    }
    return this->ring_of(source, this->cursor_config());
}

const sched::FrameRing* VisualizerStream::ring_of(const StreamSource* source,
                                                  const VisualizerConfig& config) const {
    if (source == nullptr) {
        return nullptr;
    }
    const sched::FrameRing* ring = source->visualizer_ring(config_key(config));
    return ring != nullptr && ring->epoch() == this->sent_.epoch ? ring : nullptr;
}

std::optional<VisualizerStream::Message> VisualizerStream::message_at(const sched::FrameRing& ring,
                                                                      uint32_t bins) const {
    int64_t index = std::max(this->cursor_.next, ring.base());
    size_t offset =
        index == this->cursor_.next && ring.instance() == this->offset_ring_ ? this->offset_ : 0;
    for (const sched::Frame* cell = ring.at(this->sent_.epoch, index); cell != nullptr;
         cell = ring.at(this->sent_.epoch, ++index), offset = 0) {
        if (offset < cell->payload.size()) {
            const uint8_t type = visualizer_type_of(cell->payload[offset]);
            return Message{cell, offset, visualizer_message_bytes(type, bins),
                           message_ts(cell->payload, offset)};
        }
    }
    return std::nullopt;
}

std::optional<VisualizerConfig> VisualizerStream::usable_wanted(const StreamSource* source) const {
    if (!this->wanted_ || source == nullptr) {
        return this->wanted_;
    }
    const StreamSource::Playback playback = source->playback();
    const uint64_t key = config_key(*this->wanted_);
    if (std::find(playback.failed_visualizers.begin(), playback.failed_visualizers.end(), key) !=
        playback.failed_visualizers.end()) {
        return std::nullopt;
    }
    return this->wanted_;
}

int64_t VisualizerStream::startup_lead_us(const StreamSource* source,
                                          sched::TriggerKind kind) const {
    // A visualizer declares no lead of its own, so a join takes the group lead, as a joining player
    // takes its own; a group trigger's floor is `J_min` alone, the group start's `T0` being the
    // group lead past its anchor already
    if (kind == sched::TriggerKind::GROUP || source == nullptr) {
        return this->timing_.join_min_us;
    }
    return std::max(this->timing_.join_min_us, source->playback().group_lead_us);
}

void VisualizerStream::bind_cursor(const StreamSource* source, int64_t stamp_us) {
    // An epoch change is a group trigger unless a later event set the kind
    const sched::TriggerKind kind = this->pending_kind_.value_or(sched::TriggerKind::GROUP);
    this->cursor_ = {};
    this->offset_ = 0;
    this->cursor_.min_ts = stamp_us + this->startup_lead_us(source, kind);
}

}  // namespace sendspin::server
