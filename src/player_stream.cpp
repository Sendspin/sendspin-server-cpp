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

#include "player_stream.h"

#include "fragmentation.h"
#include "frame_encoder.h"
#include "playback_policy.h"
#include "sched/chunk.h"
#include "sched/leads.h"
#include "sched/timeline.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace sendspin::server {

StreamStart player_start(const sched::AudioFormat& format) {
    return {from_sched(format), codec_header(format), std::nullopt};
}

// ==============================================================================
// Inputs
// ==============================================================================

void PlayerStream::set_support(const PlayerSupport& support) {
    this->support_ = support;
    this->ledger_ = sched::Ledger(support.buffer_capacity, true);
    this->ledger_.set_output_delay_us(this->output_delay_us());
}

std::optional<sched::TriggerKind> PlayerStream::on_activation_written(bool player_active) {
    const bool added = player_active && !this->role_written_;
    this->role_written_ = player_active;
    if (added) {
        // messaging.md "stream/start": wait for the client/state the activation requires
        this->state_since_activation_ = false;
        return sched::TriggerKind::JOIN;
    }
    if (!player_active && this->sent_.active) {
        // A marker ahead of every activation ends a removed role's stream first, so this is a bug;
        // the stream is dropped all the same
        this->sent_.active = false;
        this->ledger_.reset();
        this->cursor_ = {};
        this->announce_.reset();
    }
    return std::nullopt;
}

std::optional<sched::TriggerKind> PlayerStream::on_client_state(const ClientState& state) {
    this->available_ = state.available;
    std::optional<sched::TriggerKind> kind;
    // messaging.md "server/activate": an inactive role's object is ignored
    if (this->role_written_ && state.player) {
        if (!this->state_since_activation_) {
            kind = sched::TriggerKind::JOIN;
        }
        this->state_since_activation_ = true;
        this->player_state_ = *state.player;
        // spec player/v1.md "Player Buffer Accounting": the latest output_delay_ms
        this->ledger_.set_output_delay_us(this->output_delay_us());
    }
    return kind;
}

// ==============================================================================
// The pump
// ==============================================================================

bool PlayerStream::prepare(StreamSource* source, uint32_t markers_queued, int64_t now_us) {
    this->ledger_.prune(now_us);
    if (!this->sent_.active) {
        return false;
    }
    this->follow_wanted(source);
    const sched::FrameRing* ring = this->cursor_ring(source);
    if (ring != nullptr) {
        sched::prepare_cursor(this->cursor_, *ring, this->sent_.epoch, this->output_delay_us(),
                              now_us);
    } else {
        this->cursor_.final_done = false;
    }
    if (this->switch_) {
        // Past the cursor and the target ring's base, on a frame boundary of both formats
        const sched::FrameRing* target = source->ring_for(this->switch_->format);
        std::optional<int64_t> base;
        if (target != nullptr && target->epoch() == this->sent_.epoch) {
            base = target->base();
        }
        const sched::AudioFormat from = this->cursor_format();
        this->switch_->point = sched::plan_switch(
            this->cursor_.next, this->switch_->point, base,
            sched::frame_layout(from.sample_rate, from.codec).cells,
            sched::frame_layout(this->switch_->format.sample_rate, this->switch_->format.codec)
                .cells);
    }
    return sched::drain_watch_due(this->sent_.active, this->cursor_.final_done, markers_queued,
                                  this->ledger_, now_us);
}

sched::AudioCandidate PlayerStream::candidate(const StreamSource* source, size_t max_message_bytes,
                                              int64_t now_us) const {
    sched::CursorView view;
    view.phase_normal = true;
    view.stream_active = this->sent_.active;
    view.available = this->available_;
    view.next = this->cursor_.next;
    int64_t deadline = now_us;
    if (const sched::FrameRing* ring = this->cursor_ring(source)) {
        view.final_index = ring->final_index();
        if (const sched::Frame* frame = ring->at(this->sent_.epoch, this->cursor_.next)) {
            view.next_payload_bytes = static_cast<int64_t>(frame->payload.size());
            // When its first sample leaves the device
            deadline = frame->ts_us - this->output_delay_us();
        }
    }
    if (this->announce_) {
        view.switch_point = this->cursor_.next;
        view.target_holds_switch_point = true;
    } else if (const std::optional<SwitchTarget> target = this->switch_at(source)) {
        view.switch_point = target->point;
        view.target_holds_switch_point =
            target->ring->at(this->sent_.epoch, target->point) != nullptr;
    }
    sched::ItemState state = sched::item_ready(view, this->ledger_, now_us);
    if (state.kind == sched::ItemKind::CHUNK) {
        // A chunk short of n_max waits for its frames or room
        const ChunkView chunk =
            this->chunk_view(source, *this->cursor_ring(source), max_message_bytes, now_us);
        const sched::PackPlan plan =
            sched::plan_chunk(std::span(chunk.frames.data(), chunk.held), chunk.limits);
        const int64_t hold = sched::pack_hold_until(plan, chunk.frames[0].ts_us, chunk.limits);
        // A chunk whose next frame would not fit even an empty ledger can never grow
        if (hold != sched::NEVER && plan.bytes_with_next <= this->ledger_.capacity()) {
            const int64_t room = plan.bytes_with_next > 0
                                     ? this->ledger_.room_time(plan.bytes_with_next, now_us)
                                     : sched::NEVER;
            state = {sched::ItemKind::HELD, std::min(hold, room)};
        }
    }
    return sched::audio_candidate(state, deadline);
}

int64_t PlayerStream::drain_watch_us(const StreamSource* /*source*/,
                                     uint32_t markers_queued) const {
    return sched::drain_watch_time(this->sent_.active, this->cursor_.final_done, markers_queued,
                                   this->ledger_);
}

void PlayerStream::commit_marker(StreamSource* source, sched::StreamAction action,
                                 const sched::Want& want, int64_t stamp_us) {
    switch (action) {
        case sched::StreamAction::END:
            this->cursor_ = {};
            this->announce_.reset();
            this->switch_.reset();
            break;
        case sched::StreamAction::CLEAR: {
            // A changed format, a pending switch's included, is announced in place before the new
            // epoch's first chunk
            const sched::AudioFormat format = want.format.audio;
            if (format == this->sent_.format.audio) {
                this->announce_.reset();
            } else {
                this->announce_ = format;
            }
            this->switch_.reset();
            this->bind_cursor(source, stamp_us);
            break;
        }
        case sched::StreamAction::START:
            this->announce_.reset();
            this->switch_.reset();
            this->derived_for_ = this->preferences();
            this->bind_cursor(source, stamp_us);
            break;
        case sched::StreamAction::NONE:
            break;
    }
    sched::apply_action(this->sent_, want, action);
    // spec player/v1.md "Player Buffer Accounting": a stream/clear or stream/end resets the count
    this->ledger_.reset();
    if (source != nullptr) {
        source->on_stream_changed();
    }
}

bool PlayerStream::announce_due(const StreamSource* source) const {
    if (this->announce_) {
        return true;
    }
    const std::optional<SwitchTarget> target = this->switch_at(source);
    return target && this->cursor_.next == target->point &&
           target->ring->at(this->sent_.epoch, target->point) != nullptr;
}

std::optional<OutboundPlaintext> PlayerStream::commit_announce(StreamSource* source,
                                                               int64_t stamp_us) {
    if (!this->announce_due(source)) {
        return std::nullopt;
    }
    // The cursor moves to the target between chunks, at the index after the last old-format frame,
    // so the timeline continues
    std::optional<sched::AudioFormat> format = std::exchange(this->announce_, std::nullopt);
    if (!format && this->switch_) {
        format = this->switch_->format;
    }
    this->switch_.reset();
    if (!format) {
        return std::nullopt;
    }
    // spec player/v1.md "Player Buffer Accounting": an in-place stream/start does not reset
    this->sent_.format = sched::FormatKey{*format, 0};
    if (source != nullptr) {
        source->on_stream_changed();
    }
    return json_plaintext(build_stream_start(stamp_us, player_start(*format)));
}

PlayerStream::ChunkView PlayerStream::chunk_view(const StreamSource* source,
                                                 const sched::FrameRing& ring,
                                                 size_t max_message_bytes, int64_t now_us) const {
    ChunkView view;
    // Frames are m cells apart
    const int64_t cells = ring.cells();
    for (int64_t index = this->cursor_.next; view.held < view.frames.size(); index += cells) {
        const sched::Frame* frame = ring.at(this->sent_.epoch, index);
        if (frame == nullptr) {
            break;
        }
        view.frames[view.held++] = sched::view_of(*frame);
    }
    const sched::AudioFormat format = this->cursor_format();
    sched::PackLimits& limits = view.limits;
    limits.codec = format.codec;
    limits.cells = static_cast<uint32_t>(cells);
    limits.now_us = now_us;
    limits.output_delay_us = this->output_delay_us();
    // A held chunk keeps at least the player's min_buffer_ms, and a buffered stream's S_buf
    const sched::PlayerTiming timing =
        this->player_state_ ? player_timing(*this->player_state_) : sched::PlayerTiming{};
    limits.hold_slack_us = sched::pack_hold_slack_us(
        std::min(timing.min_buffer_ms * sched::US_PER_MS, this->timing_.timing_clamp_us),
        this->timing_.pump_margin_us,
        source != nullptr ? source->playback().kind : InputKind::BUFFERED);
    if (!this->cursor_shape_ || this->cursor_shape_->format != format) {
        this->cursor_shape_ = output_shape(format);
    }
    limits.message_cap = sched::message_cap(*this->cursor_shape_, max_message_bytes);
    limits.room_bytes = this->ledger_.capacity() - this->ledger_.count(now_us);
    limits.final_index = ring.final_index();
    // No frame at or past the switch point
    if (const std::optional<SwitchTarget> target = this->switch_at(source)) {
        limits.switch_point = target->point;
    }
    return view;
}

PackedChunk PlayerStream::take_chunk(const StreamSource* source, size_t max_message_bytes,
                                     int64_t now_us) {
    PackedChunk chunk;
    const sched::FrameRing* ring = this->cursor_ring(source);
    if (ring == nullptr) {
        return chunk;
    }
    const int64_t cells = ring->cells();
    const ChunkView view = this->chunk_view(source, *ring, max_message_bytes, now_us);
    const std::span<const sched::FrameView> views(view.frames.data(), view.held);
    const size_t frames = sched::pack_chunk(views, view.limits);
    if (frames == 0) {
        return chunk;
    }
    size_t payload = 0;
    int64_t duration = 0;
    for (size_t i = 0; i < frames; ++i) {
        payload += static_cast<size_t>(views[i].payload_bytes);
        duration += views[i].duration_us;
    }
    std::vector<uint8_t>& bytes = chunk.plaintext.bytes;
    chunk.plaintext.kind = MessageKind::BINARY;
    reserve_plaintext(bytes, sched::CHUNK_HEADER_BYTES + payload);
    bytes.resize(sched::CHUNK_HEADER_BYTES + payload);
    size_t offset = sched::CHUNK_HEADER_BYTES;
    for (size_t i = 0; i < frames; ++i) {
        const std::span<const uint8_t> from =
            ring->at(this->sent_.epoch, this->cursor_.next + (static_cast<int64_t>(i) * cells))
                ->payload.bytes();
        std::memcpy(bytes.data() + offset, from.data(), from.size());
        offset += from.size();
    }
    chunk.ts_us = views[0].ts_us;
    chunk.audio_bytes = payload;
    // Counted from the write's start, its key the end of its audio
    this->ledger_.add(chunk.ts_us + duration, static_cast<int64_t>(bytes.size()));
    this->cursor_.next += static_cast<int64_t>(frames) * cells;
    return chunk;
}

// ==============================================================================
// Queries
// ==============================================================================

std::optional<sched::AudioFormat> PlayerStream::wanted_format(const StreamSource* source) const {
    if (source == nullptr || !this->support_) {
        return std::nullopt;
    }
    const StreamSource::Playback playback = source->playback();
    if (!playback.input) {
        return std::nullopt;
    }
    const auto failed = [&](const sched::AudioFormat& format) {
        return std::find(playback.failed.begin(), playback.failed.end(), format) !=
               playback.failed.end();
    };
    const auto select = [&] {
        return select_player_format(*this->support_, this->client_preference(), *playback.input,
                                    playback.failed, this->format_override_, playback.opus);
    };
    if (!this->sent_.active || this->sent_.run != playback.run || failed(this->cursor_format())) {
        return select();
    }
    // An active stream keeps its format through its run, across epochs and set_format(), and
    // switches in place to the one a changed preference selects (spec player/v1.md "client/state
    // player object": the server re-derives the stream format)
    if (this->preferences() != this->derived_for_) {
        if (const std::optional<sched::AudioFormat> selected = select()) {
            return selected;
        }
    }
    const sched::AudioFormat target = this->target_format();
    return failed(target) ? this->cursor_format() : target;
}

// ==============================================================================
// Helpers
// ==============================================================================

sched::Want PlayerStream::want(const StreamSource* source, sched::RoleSet roles_ahead,
                               int64_t now_us) const {
    const StreamSource::Playback playback =
        source != nullptr ? source->playback() : StreamSource::Playback{};
    sched::WantInputs in;
    in.role_written = this->role_written_;
    in.role_ahead = roles_ahead.contains(sched::StreamRole::PLAYER);
    in.group_playing = playback.playing;
    in.run = playback.run;
    in.epoch = playback.epoch;
    in.epoch_draining = playback.draining;
    in.available = this->available_;
    // A stream on a failed format ends
    const std::optional<sched::AudioFormat> format = this->wanted_format(source);
    const bool usable =
        !this->sent_.active || std::find(playback.failed.begin(), playback.failed.end(),
                                         this->cursor_format()) == playback.failed.end();
    if (format && usable) {
        in.format = sched::FormatKey{*format, 0};
    }
    if (playback.draining) {
        // The pending kind's startup lead
        const int64_t lead = sched::startup_lead_us(
            this->lead(playback.kind), this->pending_kind_.value_or(sched::TriggerKind::JOIN),
            this->timing_.join_min_us);
        // The last frame of the stream's format begins at its frame boundary at or below the
        // last cell
        std::optional<int64_t> last_ts;
        const std::optional<sched::AudioFormat> frames_of =
            this->sent_.active ? std::optional(this->cursor_format()) : format;
        if (playback.last_cell && playback.anchor != nullptr) {
            const uint32_t cells =
                frames_of ? sched::frame_layout(frames_of->sample_rate, frames_of->codec).cells : 1;
            last_ts = sched::last_frame_ts(*playback.anchor, *playback.last_cell,
                                           std::max<uint32_t>(cells, 1));
        }
        const sched::FrameRing* ring = this->cursor_ring(source);
        in.drain_done = sched::role_drain_done(this->sent_.active, this->cursor_.next,
                                               ring != nullptr ? ring->final_index() : std::nullopt,
                                               this->ledger_, lead, last_ts, now_us);
    }
    return sched::evaluate_want(in);
}

std::optional<AudioFormat> PlayerStream::client_preference() const {
    return this->player_state_ ? this->player_state_->format : std::nullopt;
}

PlayerStream::Preferences PlayerStream::preferences() const {
    return {this->format_override_, this->client_preference()};
}

void PlayerStream::follow_wanted(StreamSource* source) {
    if (source == nullptr) {
        return;
    }
    const StreamSource::Playback playback = source->playback();
    // A stream about to end or clear gets its format from the marker
    if (this->sent_.run != playback.run || this->sent_.epoch != playback.epoch) {
        return;
    }
    const std::optional<sched::AudioFormat> wanted = this->wanted_format(source);
    this->derived_for_ = this->preferences();
    if (!wanted || *wanted == this->target_format()) {
        return;
    }
    if (this->announce_) {
        // No chunk of the epoch went out yet: the announcement names the new format
        this->announce_ = wanted;
        if (*wanted == this->sent_.format.audio) {
            this->announce_.reset();
        }
    } else if (*wanted == this->cursor_format()) {
        // Back to the stream's own format cancels the switch
        this->switch_.reset();
    } else {
        // A new target, planned from the cursor
        this->switch_ = Switch{*wanted, std::nullopt};
    }
    // The group builds the target's instance and drops an abandoned one
    source->on_stream_changed();
}

std::optional<PlayerStream::SwitchTarget> PlayerStream::switch_at(
    const StreamSource* source) const {
    if (!this->switch_ || !this->switch_->point || source == nullptr) {
        return std::nullopt;
    }
    const sched::FrameRing* ring = source->ring_for(this->switch_->format);
    if (ring == nullptr || ring->epoch() != this->sent_.epoch) {
        return std::nullopt;
    }
    // A drained epoch whose target ends below the point (a downsampling instance can end a cell
    // early) leaves the stream to end in its format
    const int64_t point = *this->switch_->point;
    const std::optional<int64_t> final_index = ring->final_index();
    if (final_index && *final_index < point) {
        return std::nullopt;
    }
    return SwitchTarget{ring, point};
}

sched::StartPreconditions PlayerStream::preconditions() const {
    return {this->available_, this->role_written_, this->state_since_activation_,
            this->pending_kind_.has_value()};
}

sched::PlayerLead PlayerStream::lead(InputKind kind) const {
    const sched::PlayerTiming timing =
        this->player_state_ ? player_timing(*this->player_state_) : sched::PlayerTiming{};
    return sched::player_lead_us(timing, kind, this->timing_.timing_clamp_us);
}

const sched::FrameRing* PlayerStream::cursor_ring(const StreamSource* source) const {
    if (source == nullptr || !this->sent_.active) {
        return nullptr;
    }
    const sched::FrameRing* ring = source->ring_for(this->cursor_format());
    return ring != nullptr && ring->epoch() == this->sent_.epoch ? ring : nullptr;
}

void PlayerStream::bind_cursor(const StreamSource* source, int64_t stamp_us) {
    // An epoch change is a group trigger unless a later event set the kind
    const sched::TriggerKind kind = this->pending_kind_.value_or(sched::TriggerKind::GROUP);
    const InputKind input = source != nullptr ? source->playback().kind : InputKind::BUFFERED;
    this->cursor_ = {};
    this->cursor_.min_ts =
        sched::startup_min_ts(stamp_us, this->lead(input), kind, this->timing_.join_min_us);
}

int64_t PlayerStream::output_delay_us() const {
    return this->player_state_ ? int64_t{this->player_state_->output_delay_ms} * sched::US_PER_MS
                               : 0;
}

}  // namespace sendspin::server
