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

#include "group.h"

#include "clock.h"
#include "log.h"
#include "messages.h"
#include "playback_policy.h"
#include "sched/lifecycle.h"

#include <algorithm>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sendspin::server {

// ==============================================================================
// MemberSource
// ==============================================================================

/// What one member's player and artwork streams read of the group
class Group::MemberSource : public StreamSource, public ArtworkView {
public:
    explicit MemberSource(Group& group) : group_(group) {}

    Playback playback() const override {
        Playback playback;
        playback.playing = this->group_.playing_;
        playback.run = this->group_.run_;
        playback.epoch = this->group_.epoch_;
        playback.anchor = this->group_.anchor_ ? &*this->group_.anchor_ : nullptr;
        playback.draining = this->group_.draining_;
        playback.last_cell = this->group_.last_cell_;
        playback.input = this->group_.input_format_;
        playback.kind = this->group_.input_kind_;
        playback.failed = this->group_.failed_formats_;
        playback.opus = this->group_.opus_;
        playback.failed_visualizers = this->group_.failed_visualizers_;
        playback.group_lead_us = this->group_.group_lead_us_;
        return playback;
    }

    const sched::FrameRing* ring_for(const sched::AudioFormat& format) const override {
        for (const PipelineRequest& pipeline : this->group_.pipelines_.frames) {
            if (pipeline.format == format) {
                return this->ring(pipeline.instance);
            }
        }
        return nullptr;
    }

    const sched::FrameRing* visualizer_ring(uint64_t key) const override {
        for (const AnalysisRequest& analysis : this->group_.pipelines_.analyses) {
            if (config_key(analysis.config) == key) {
                return this->ring(analysis.instance);
            }
        }
        return nullptr;
    }

    void on_stream_changed() override {
        this->group_.schedule_check();
    }

    ArtworkSchedule artwork(ArtworkSource source, int64_t now_us) const override {
        return this->group_.timeline(source).at(now_us);
    }

    std::optional<EncodedImage> encoded(const ArtworkImage& image,
                                        const ArtworkChannel& channel) override {
        return this->group_.artwork_cache_.encoded(image, channel);
    }

private:
    const sched::FrameRing* ring(sched::InstanceId instance) const {
        const auto it = this->group_.rings_.find(instance);
        return it == this->group_.rings_.end() ? nullptr : &it->second;
    }

    Group& group_;
};

// ==============================================================================
// Group
// ==============================================================================

Group::Group(EventLoop& loop, AudioWorkers& workers, GroupId id, std::string name,
             uint64_t& next_instance, ResamplerQuality quality, bool opus, GroupHooks hooks,
             ArtworkCache& artwork)
    : loop_(loop),
      workers_(workers),
      id_(id),
      name_(std::move(name)),
      next_instance_(next_instance),
      quality_(quality),
      opus_(opus),
      hooks_(std::move(hooks)),
      slot_(std::make_shared<PipelineSetSlot>()),
      stall_timer_(loop),
      writable_timer_(loop),
      drain_timer_(loop),
      metadata_timer_(loop),
      color_timer_(loop),
      artwork_cache_(artwork),
      album_(ArtworkImage{artwork.next_image_id(), now_us(), nullptr}),
      artist_(ArtworkImage{artwork.next_image_id(), now_us(), nullptr}),
      alive_(std::make_shared<bool>(true)) {}

Group::~Group() {
    if (this->input_) {
        this->input_->close();
    }
    // A job running now finishes on its thread; what it pushes the FIFO drops
    if (this->audio_) {
        this->audio_->stop();
    }
}

GroupInfo Group::info() const {
    GroupInfo info;
    info.id = this->id_;
    info.name = this->name_;
    info.playback_state = this->playing_ ? PlaybackState::PLAYING : PlaybackState::STOPPED;
    for (const Member& member : this->members_) {
        info.client_ids.push_back(member.member->client_info().client_id);
    }
    return info;
}

void Group::rename(const std::string& name) {
    if (name == this->name_) {
        return;
    }
    this->name_ = name;
    this->hooks_.changed();
    if (const std::optional<std::string> json = this->group_update()) {
        this->queue_to_members(*json, [](GroupMember& /*member*/) { return true; });
    }
}

// ==============================================================================
// Members
// ==============================================================================

void Group::add_member(GroupMember& member) {
    this->members_.push_back({&member, std::make_unique<MemberSource>(*this)});
    member.session().set_stream_source(this->members_.back().source.get());
    // The artwork stream shows this group's images from now on, an empty image where it has none
    member.session().set_artwork_view(this->members_.back().source.get());
    // messaging.md "group/update": after the first activation and on every change
    this->send_group_update(member);
    this->send_role_state(member);
    this->refresh_controller(&member);
    // Joining a playing group is a join trigger
    member.session().queue_marker(sched::TriggerKind::JOIN);
    this->recompute();
    this->hooks_.changed();
    member.pump();
}

void Group::remove_member(GroupMember& member) {
    const auto it = std::find_if(this->members_.begin(), this->members_.end(),
                                 [&](const Member& entry) { return entry.member == &member; });
    if (it == this->members_.end()) {
        return;
    }
    member.session().set_stream_source(nullptr);
    member.session().set_artwork_view(nullptr);
    this->members_.erase(it);
    this->unplayable_reported_.erase(&member);
    this->failure_reported_.erase(&member);
    this->recompute();
    this->schedule_check();
    this->refresh_controller(nullptr);
    this->hooks_.changed();
}

void Group::on_client_state(GroupMember& /*member*/) {
    this->recompute();
    // controller/v1.md: a player's volume, mute or their support changes the group's
    this->refresh_controller(nullptr);
}

std::optional<std::string> Group::group_update() const {
    return build_group_update({this->playing_, std::to_string(this->id_.value), this->name_});
}

void Group::send_group_update(GroupMember& member) {
    if (const std::optional<std::string> json = this->group_update()) {
        member.session().queue_json_message(*json);
    }
}

void Group::queue_markers(std::optional<sched::TriggerKind> kind) {
    // A pump may close a connection, which removes its member: iterate over a copy
    std::vector<GroupMember*> members;
    members.reserve(this->members_.size());
    for (const Member& member : this->members_) {
        member.member->session().queue_marker(kind);
        members.push_back(member.member);
    }
    for (GroupMember* member : members) {
        member->pump();
    }
}

void Group::pump_members(bool active_only) {
    std::vector<GroupMember*> members;
    members.reserve(this->members_.size());
    for (const Member& member : this->members_) {
        const Session& session = member.member->session();
        if (!active_only || session.player().active() || session.visualizer().active()) {
            members.push_back(member.member);
        }
    }
    for (GroupMember* member : members) {
        member->pump();
    }
}

// ==============================================================================
// Controller and metadata
// ==============================================================================

ControllerState Group::controller_state() const {
    std::vector<PlayerLevels> players;
    for (const Member& member : this->members_) {
        const Session& session = member.member->session();
        const std::optional<PlayerState>& state = session.player().player_state();
        if (!session.role_active(PLAYER_ROLE) || !state) {
            continue;
        }
        PlayerLevels levels;
        // roles/player/v1.md "Supported commands": a reported value is settable only with its
        // command
        if ((state->supported_commands & player_command::VOLUME) != 0) {
            levels.volume = state->volume;
        }
        if ((state->supported_commands & player_command::MUTE) != 0) {
            levels.muted = state->muted;
        }
        players.push_back(levels);
    }
    return ::sendspin::server::controller_state(players, this->caps_);
}

void Group::set_controller_caps(const ControllerCaps& caps) {
    this->caps_ = caps;
    this->refresh_controller(nullptr);
}

void Group::refresh_controller(GroupMember* except) {
    const ControllerState state = this->controller_state();
    const bool changed = state != this->controller_;
    this->controller_ = state;
    const std::optional<std::string> json = build_server_state({state, std::nullopt, std::nullopt});
    if (!json) {
        return;
    }
    // messaging.md "server/state": prompt reports of changes
    this->queue_to_members(*json, [&](GroupMember& member) {
        return changed && &member != except && member.session().role_queued(CONTROLLER_ROLE);
    });
}

template <typename Select>
void Group::queue_to_members(const std::string& json, Select select) {
    // A pump may close a connection, which removes its member: iterate over a copy
    std::vector<GroupMember*> members;
    for (const Member& member : this->members_) {
        if (select(*member.member)) {
            member.member->session().queue_json_message(json);
            members.push_back(member.member);
        }
    }
    for (GroupMember* member : members) {
        member->pump();
    }
}

// roles/player/v1.md "server/command player object": only commands in the player's latest
// supported_commands, and none before its client/state
void Group::apply_volume(uint32_t volume) {
    std::vector<GroupMember*> players;
    std::vector<uint32_t> volumes;
    for (const Member& member : this->members_) {
        const Session& session = member.member->session();
        const std::optional<PlayerState>& state = session.player().player_state();
        if (session.role_active(PLAYER_ROLE) && session.role_queued(PLAYER_ROLE) && state &&
            (state->supported_commands & player_command::VOLUME) != 0 && state->volume) {
            players.push_back(member.member);
            volumes.push_back(*state->volume);
        }
    }
    const std::vector<uint32_t> levels = group_volumes(volumes, volume);
    for (size_t i = 0; i < players.size(); ++i) {
        players[i]->session().queue_json_message(build_player_volume_command(levels[i]));
    }
    for (GroupMember* player : players) {
        player->pump();
    }
}

void Group::apply_mute(bool muted) {
    const std::string json = build_player_mute_command(muted);
    this->queue_to_members(json, [](GroupMember& member) {
        const Session& session = member.session();
        const std::optional<PlayerState>& state = session.player().player_state();
        return session.role_active(PLAYER_ROLE) && session.role_queued(PLAYER_ROLE) && state &&
               (state->supported_commands & player_command::MUTE) != 0;
    });
}

void Group::set_metadata(const Metadata& metadata, int64_t at_us) {
    this->send_metadata(this->metadata_.set(metadata, at_us, now_us()));
}

void Group::cancel_scheduled_metadata() {
    this->send_metadata(this->metadata_.cancel(now_us()));
}

MetadataState Group::metadata_now() const {
    const int64_t now = now_us();
    return metadata_at(this->metadata_.current(now), now);
}

void Group::send_role_state(GroupMember& member) {
    Session& session = member.session();
    ServerState state;
    if (session.role_queued(CONTROLLER_ROLE)) {
        state.controller = this->controller_state();
    }
    // messaging.md "server/state": the first carries a past or present timestamp. An update that
    // came due and is not sent yet reaches every member when the timer sends it.
    ServerState pending;
    if (session.role_queued(METADATA_ROLE)) {
        state.metadata = this->metadata_.current(now_us());
        pending.metadata = this->metadata_.sent_pending();
    }
    if (session.role_queued(COLOR_ROLE)) {
        state.color = this->colors_.current(now_us());
        pending.color = this->colors_.sent_pending();
    }
    if (!state.controller && !state.metadata && !state.color) {
        return;
    }
    if (const std::optional<std::string> json = build_server_state(state)) {
        session.queue_json_message(*json);
    }
    if (pending.metadata || pending.color) {
        if (const std::optional<std::string> json = build_server_state(pending)) {
            session.queue_json_message(*json);
        }
    }
}

void Group::send_metadata(const MetadataSends& sends) {
    for (const MetadataState& state : sends.states) {
        const std::optional<std::string> json =
            build_server_state({std::nullopt, state, std::nullopt});
        if (json) {
            this->queue_to_members(*json, [](GroupMember& member) {
                return member.session().role_queued(METADATA_ROLE);
            });
        }
    }
    const std::optional<int64_t> due = this->metadata_.next_due_us();
    if (due) {
        this->metadata_timer_.start(*due, this->guarded([this] { this->on_metadata_timer(); }));
    } else {
        this->metadata_timer_.cancel();
    }
}

void Group::on_metadata_timer() {
    this->send_metadata(this->metadata_.advance(now_us()));
}

// ==============================================================================
// Artwork and colors
// ==============================================================================

ArtworkTimeline& Group::timeline(ArtworkSource source) {
    return source == ArtworkSource::ALBUM ? this->album_ : this->artist_;
}

void Group::set_artwork(ArtworkSource source, std::shared_ptr<const SourceImage> image,
                        int64_t at_us) {
    const int64_t now = now_us();
    if (source == ArtworkSource::ALBUM) {
        // The colors follow the album image
        if (image) {
            this->palette_wait_ = PaletteWait{image, at_us};
            this->resolve_palette();
        } else {
            this->palette_wait_.reset();
            this->send_colors(this->colors_.set({at_us, Colors{}}, now));
        }
    }
    this->timeline(source).set({this->artwork_cache_.next_image_id(), at_us, std::move(image)},
                               now);
    this->pump_artwork_members();
}

void Group::cancel_scheduled_artwork(ArtworkSource source) {
    const int64_t now = now_us();
    this->timeline(source).cancel(now);
    if (source == ArtworkSource::ALBUM) {
        if (this->palette_wait_ && this->palette_wait_->at_us > now) {
            this->palette_wait_.reset();
        }
        this->send_colors(this->colors_.cancel(now));
    }
    this->pump_artwork_members();
}

void Group::set_colors(const Colors& colors, int64_t at_us) {
    // The host's colors replace those of an album image's palette still to come
    this->palette_wait_.reset();
    this->send_colors(this->colors_.set({at_us, with_contrast(colors)}, now_us()));
}

void Group::cancel_scheduled_colors() {
    this->send_colors(this->colors_.cancel(now_us()));
}

void Group::resolve_palette() {
    if (!this->palette_wait_) {
        return;
    }
    const std::optional<Colors> colors = this->artwork_cache_.palette(this->palette_wait_->image);
    if (!colors) {
        return;
    }
    const int64_t at_us = this->palette_wait_->at_us;
    this->palette_wait_.reset();
    this->send_colors(this->colors_.set({at_us, *colors}, now_us()));
}

void Group::on_artwork_ready() {
    this->resolve_palette();
    this->pump_artwork_members();
}

void Group::pump_artwork_members() {
    // A pump may close a connection, which removes its member: iterate over a copy
    std::vector<GroupMember*> members;
    for (const Member& member : this->members_) {
        if (member.member->session().artwork().role_written()) {
            members.push_back(member.member);
        }
    }
    for (GroupMember* member : members) {
        member->pump();
    }
}

void Group::send_colors(const std::vector<ColorState>& states) {
    for (const ColorState& state : states) {
        const std::optional<std::string> json =
            build_server_state({std::nullopt, std::nullopt, state});
        if (json) {
            this->queue_to_members(*json, [](GroupMember& member) {
                return member.session().role_queued(COLOR_ROLE);
            });
        }
    }
    const std::optional<int64_t> due = this->colors_.next_due_us();
    if (due) {
        this->color_timer_.start(*due, this->guarded([this] { this->on_color_timer(); }));
    } else {
        this->color_timer_.cancel();
    }
}

void Group::on_color_timer() {
    this->send_colors(this->colors_.advance(now_us()));
}

// ==============================================================================
// Input
// ==============================================================================

bool Group::attach_input(const std::shared_ptr<HostView>& view) {
    if (this->input_) {
        // A released input whose detach has not run yet no longer holds the group
        if (!this->input_->gone()) {
            return false;
        }
        this->drop_released_input();
    }
    this->input_ = view;
    this->input_kind_ = view->kind();
    this->write_jitter_us_ = view->write_jitter_us();
    const HostView::FormatState format = view->format();
    this->input_format_ = format.format;
    this->input_generation_ = format.generation;
    this->wakeup_ =
        std::make_unique<Wakeup>(this->loop_, this->guarded([this] { this->on_wake(); }));
    this->fifo_ = std::make_unique<GroupToCoreFifo>(
        this->loop_, [this](GroupEvent& event) { this->deliver(event); });
    this->audio_ = this->workers_.add(PipelineJob{
        std::make_shared<GroupAudio>(view, this->fifo_->producer(), this->slot_, this->quality_),
        this->wakeup_->handle()});
    view->attach(this->wakeup_->handle(), this->audio_);
    this->unplayable_reported_.clear();
    this->failure_reported_.clear();
    this->recompute();
    this->audio_->wake();
    return true;
}

void Group::close_input() {
    if (this->input_) {
        this->input_->close();
        this->detach_input();
    }
}

bool Group::close_input_of(const HostView& view) {
    if (this->input_.get() != &view) {
        return false;
    }
    this->input_->close();
    this->drop_released_input();
    return true;
}

void Group::detach_input() {
    // The view is closed first, so no host call signals the wakeup it destroys or wakes the
    // pipelines it stops; the FIFO goes with them, so what they posted and core has not taken
    // is dropped
    this->input_->close();
    this->input_.reset();
    this->audio_->stop();
    this->audio_.reset();
    this->fifo_.reset();
    this->wakeup_.reset();
    this->input_format_.reset();
    this->input_kind_ = InputKind::BUFFERED;
    this->write_jitter_us_ = 0;
    this->filter_lengths_.clear();
    this->writable_timer_.cancel();
    this->recompute();
    this->hooks_.detached();
}

void Group::drop_released_input() {
    // The pipelines may never post the run's end once stopped: core ends it here, and drops what
    // they posted with the FIFO
    if (this->playing_) {
        this->end_run(this->epoch_);
    }
    this->detach_input();
}

void Group::on_wake() {
    if (!this->input_) {
        return;
    }
    if (this->input_->gone()) {
        this->drop_released_input();
        return;
    }
    this->input_->free_retired_slew();
    const HostView::FormatState format = this->input_->format();
    if (format.generation != this->input_generation_) {
        this->input_format_ = format.format;
        this->input_generation_ = format.generation;
        const std::set<const GroupMember*> unplayable =
            std::exchange(this->unplayable_reported_, {});
        this->failure_reported_.clear();
        this->recompute();
        // The input's new format can give a player with none a format, which makes it eligible, so
        // it joins
        std::vector<GroupMember*> joined;
        for (const Member& member : this->members_) {
            if (unplayable.contains(member.member) &&
                !this->unplayable_reported_.contains(member.member)) {
                member.member->session().queue_marker(sched::TriggerKind::JOIN);
                joined.push_back(member.member);
            }
        }
        for (GroupMember* member : joined) {
            member->pump();
        }
    }
    this->poll_writable();
}

bool Group::PipelineJob::operator()() const {
    const bool more = this->audio->run_job(now_us());
    Wakeup::signal(this->done);
    return more;
}

void Group::poll_writable() {
    if (!this->input_) {
        return;
    }
    const int64_t at = this->input_->poll_writable(now_us());
    if (at == sched::NEVER) {
        this->writable_timer_.cancel();
        return;
    }
    this->writable_timer_.start(at, this->guarded([this] { this->poll_writable(); }));
}

void Group::server_stop() {
    if (!this->input_) {
        return;
    }
    const sched::ServerStopResult result = this->input_->server_stop();
    if (!result.applied) {
        return;
    }
    // Core ends the run on its side at once and ignores what the pipelines post for the stopped
    // epoch
    if (this->playing_) {
        this->end_run(std::max(this->epoch_, result.stopped_epoch));
    } else {
        this->ended_epoch_ = std::max(this->ended_epoch_, result.stopped_epoch);
        this->hooks_.changed();
    }
}

// ==============================================================================
// Group-to-core events
// ==============================================================================

void Group::deliver(GroupEvent& event) {
    std::visit(
        [this](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, EpochStarted>) {
                this->on_epoch_started(value);
            } else if constexpr (std::is_same_v<T, EpochCut>) {
                this->on_epoch_cut(value);
            } else if constexpr (std::is_same_v<T, EpochEnded>) {
                if (this->playing_ && value.epoch == this->epoch_) {
                    this->end_run(value.epoch);
                }
            } else if constexpr (std::is_same_v<T, AnchorShift>) {
                this->on_anchor_shift(value);
            } else if constexpr (std::is_same_v<T, DrainStarted>) {
                this->on_drain_started(value);
            } else if constexpr (std::is_same_v<T, FrameReady>) {
                this->on_frame(value);
            } else if constexpr (std::is_same_v<T, InstanceFinal>) {
                this->on_instance_final(value);
            } else {
                this->on_pipeline_failed(value);
            }
        },
        event);
}

void Group::on_epoch_started(EpochStarted& started) {
    if (started.epoch <= this->ended_epoch_) {
        return;
    }
    const bool new_run = !this->playing_ || started.run != this->run_;
    this->run_ = started.run;
    this->epoch_ = started.epoch;
    this->anchor_ = std::move(started.anchor);
    this->playing_ = true;
    this->draining_ = false;
    this->last_cell_.reset();
    this->drain_timer_.cancel();
    for (auto& [instance, ring] : this->rings_) {
        ring.restart(this->epoch_, 0);
    }
    if (started.drain) {
        this->on_drain_started(*started.drain);
    }
    if (new_run) {
        for (const Member& member : this->members_) {
            this->send_group_update(*member.member);
            // A member that left the group with client/leave takes part in its playback again;
            // its artwork stream starts with the pump below
            member.member->session().set_artwork_left(false);
        }
        this->hooks_.changed();
        // metadata/v1.md: a new state on resume, taking effect as the run's first sample plays
        if (this->anchor_) {
            this->send_metadata(
                this->metadata_.resume(sched::frame_timestamp(*this->anchor_, 0), now_us()));
        }
    }
    this->queue_markers(sched::TriggerKind::GROUP);
    this->schedule_check();
}

void Group::on_epoch_cut(const EpochCut& cut) {
    if (cut.epoch <= this->ended_epoch_ || !this->playing_) {
        return;
    }
    this->epoch_ = cut.epoch;
    this->anchor_.reset();
    this->draining_ = false;
    this->last_cell_.reset();
    this->drain_timer_.cancel();
    for (auto& [instance, ring] : this->rings_) {
        ring.restart(this->epoch_, 0);
    }
    this->queue_markers(sched::TriggerKind::GROUP);
}

void Group::on_anchor_shift(const AnchorShift& shift) {
    // Recorded, so core maps timestamps to indices with it
    if (shift.epoch != this->epoch_ || !this->playing_ || !this->anchor_) {
        return;
    }
    sched::add_shift(*this->anchor_, shift.index, shift.delta_us);
    // A live epoch's list stays short
    const int64_t now = now_us();
    sched::fold_shifts(*this->anchor_, sched::last_begun_cell(*this->anchor_, now));
    ++this->stalls_.rebases;
    this->stalls_.shifted_us += shift.delta_us;
    this->report_stalls(now);
}

void Group::report_stalls(int64_t now) {
    Stalls& stalls = this->stalls_;
    if (stalls.rebases == 0 && stalls.late_frames == 0) {
        return;
    }
    if (stalls.reported_us != 0 && now - stalls.reported_us < Stalls::STALL_REPORT_US) {
        // Reported once due, should no stall follow to report them
        this->stall_timer_.start(stalls.reported_us + Stalls::STALL_REPORT_US,
                                 this->guarded([this] { this->report_stalls(now_us()); }));
        return;
    }
    this->stall_timer_.cancel();
    if (this->hooks_.info) {
        this->hooks_.info(log_format(
            "group %s: %u rebases (%lld ms in all) and %u frames ready after their play time "
            "since the last report",
            printable(this->name_).c_str(), static_cast<unsigned>(stalls.rebases),
            static_cast<long long>(stalls.shifted_us / sched::US_PER_MS),
            static_cast<unsigned>(stalls.late_frames)));
    }
    stalls = Stalls{};
    stalls.reported_us = now;
}

void Group::on_drain_started(const DrainStarted& drain) {
    if (drain.epoch != this->epoch_ || !this->playing_) {
        return;
    }
    this->draining_ = true;
    this->last_cell_ = drain.last_cell;
    this->arm_drain_timer();
    this->queue_markers(std::nullopt);
    this->schedule_check();
}

void Group::on_frame(FrameReady& ready) {
    if (ready.frame.epoch <= this->ended_epoch_) {
        return;
    }
    const auto it = this->rings_.find(ready.instance);
    if (it == this->rings_.end()) {
        return;
    }
    const int64_t now = now_us();
    // An analysis that fell behind restarts past the cells late for everyone
    const std::vector<AnalysisRequest>& analyses = this->pipelines_.analyses;
    const bool analysis = std::any_of(
        analyses.begin(), analyses.end(),
        [&](const AnalysisRequest& request) { return request.instance == ready.instance; });
    if (analysis) {
        it->second.skip_to(ready.frame.epoch, ready.frame.index, now);
    } else if (ready.frame.epoch == this->epoch_ && ready.frame.ts_us <= now) {
        // Pruned as it arrives: no player gets it
        ++this->stalls_.late_frames;
        this->report_stalls(now);
    }
    it->second.append(std::move(ready.frame), now);
    // Pumps a cursor waiting for frames
    this->pump_members(true);
}

void Group::on_instance_final(const InstanceFinal& final) {
    if (final.epoch != this->epoch_ || !this->playing_) {
        return;
    }
    const auto it = this->rings_.find(final.instance);
    if (it != this->rings_.end()) {
        it->second.set_final_index(final.final_index);
    }
    this->pump_members(true);
    this->schedule_check();
}

void Group::on_pipeline_failed(const PipelineFailed& failed) {
    // Matched by instance, whatever its epoch: ids are never reused, and a broken instance stays
    // broken
    const std::vector<PipelineRequest>& frames = this->pipelines_.frames;
    const auto pipeline = std::find_if(
        frames.begin(), frames.end(),
        [&](const PipelineRequest& request) { return request.instance == failed.instance; });
    if (pipeline == frames.end()) {
        this->on_analysis_failed(failed);
        return;
    }
    const sched::AudioFormat format = pipeline->format;
    std::vector<GroupMember*> affected;
    for (const Member& member : this->members_) {
        const PlayerStream& player = member.member->session().player();
        const bool streamed = player.active() && player.cursor_format() == format;
        if (!streamed && player.wanted_format(member.source.get()) != format) {
            continue;
        }
        affected.push_back(member.member);
        const ClientInfo& info = member.member->client_info();
        this->hooks_.error(ServerError{ErrorKind::PIPELINE_FAILED, info.url, info.client_id,
                                       std::nullopt, stream_format(format)});
        // Reported as the failure, not again as having no format
        if (this->unplayable_reported_.insert(member.member).second) {
            this->failure_reported_.insert(member.member);
        }
    }
    this->failed_formats_.push_back(format);
    this->recompute();
    // A pipeline failure appends markers; a stream on the format ends, and a player with another
    // format joins on it at once (a join trigger)
    for (GroupMember* member : affected) {
        member->session().queue_marker(sched::TriggerKind::JOIN);
    }
    this->queue_markers(std::nullopt);
    this->schedule_check();
}

void Group::end_run(sched::EpochId epoch) {
    this->playing_ = false;
    this->ended_epoch_ = std::max(this->ended_epoch_, epoch);
    this->anchor_.reset();
    this->draining_ = false;
    this->last_cell_.reset();
    this->drain_timer_.cancel();
    for (auto& [instance, ring] : this->rings_) {
        ring.restart({}, 0);
    }
    for (const Member& member : this->members_) {
        this->send_group_update(*member.member);
    }
    this->hooks_.changed();
    // metadata/v1.md: a new state on pause or stop. A run that ends with the input paused, or
    // already resumed, is a pause, whose speed a resume restores.
    bool paused = false;
    if (this->input_) {
        const sched::RunState run = this->input_->job_view().run.run;
        paused = run == sched::RunState::PAUSED || run == sched::RunState::PLAYING;
    }
    this->send_metadata(this->metadata_.stop(now_us(), paused));
    // A failed format is excluded until the group's next run
    this->failed_formats_.clear();
    this->failed_visualizers_.clear();
    for (const GroupMember* member : this->failure_reported_) {
        this->unplayable_reported_.erase(member);
    }
    this->failure_reported_.clear();
    this->queue_markers(std::nullopt);
    this->recompute();
}

// ==============================================================================
// Drain completion
// ==============================================================================

void Group::arm_drain_timer() {
    // No player start can find a frame from `last_ts - J_min` on
    int64_t at = now_us();
    if (this->last_cell_ && this->anchor_) {
        at = sched::frame_timestamp(*this->anchor_, *this->last_cell_) - this->timing_.join_min_us;
    }
    this->drain_timer_.start(at, this->guarded([this] { this->check_drain(); }));
}

void Group::schedule_check() {
    if (this->drain_check_posted_) {
        return;
    }
    this->drain_check_posted_ = true;
    this->loop_.post(this->guarded([this] {
        this->drain_check_posted_ = false;
        this->recompute();
        this->check_drain();
    }));
}

void Group::check_drain() {
    if (!this->input_ || !this->draining_ || !this->playing_) {
        return;
    }
    sched::GroupDrainView view;
    view.epoch_draining = true;
    view.run_playing = true;
    view.any_stream_active =
        std::any_of(this->members_.begin(), this->members_.end(), [](const Member& member) {
            const Session& session = member.member->session();
            return session.player().active() || session.visualizer().active();
        });
    if (this->last_cell_ && this->anchor_) {
        view.last_ts_us = sched::frame_timestamp(*this->anchor_, *this->last_cell_);
    }
    if (!sched::drain_complete(view, this->timing_.join_min_us, now_us())) {
        return;
    }
    // The host view decides against a racing flush, pause or stop; the run ends on core's side
    // before the host hears on_drained
    const sched::EpochId epoch = this->epoch_;
    const std::shared_ptr<HostView> input = this->input_;
    if (input->complete_drain(epoch)) {
        this->end_run(epoch);
        input->release_drained();
    }
}

// ==============================================================================
// Parameters and pipelines
// ==============================================================================

void Group::recompute() {
    if (!this->input_ || !this->input_format_) {
        if (!this->pipelines_.frames.empty() || !this->pipelines_.analyses.empty()) {
            this->pipelines_ = {};
            this->rings_.clear();
            this->slot_->publish({});
        }
        return;
    }
    const uint32_t input_rate = this->input_format_->sample_rate;
    std::vector<EligiblePlayer> eligible;
    std::vector<sched::AudioFormat> formats;
    const auto add_format = [&formats](const sched::AudioFormat& format) {
        if (std::find(formats.begin(), formats.end(), format) == formats.end()) {
            formats.push_back(format);
        }
    };
    for (const Member& member : this->members_) {
        const PlayerStream& player = member.member->session().player();
        // An instance stays while a cursor reads it, unless it failed
        if (player.active() && std::find(this->failed_formats_.begin(), this->failed_formats_.end(),
                                         player.cursor_format()) == this->failed_formats_.end()) {
            add_format(player.cursor_format());
        }
        const std::optional<PlayerSupport>& support = player.support();
        const std::optional<PlayerState>& state = player.player_state();
        if (!player.eligible() || !support || !state) {
            continue;
        }
        const std::optional<sched::AudioFormat> format = player.wanted_format(member.source.get());
        if (!format) {
            // No stream, and on_error names the player
            if (this->unplayable_reported_.insert(member.member).second) {
                const ClientInfo& info = member.member->client_info();
                this->hooks_.error(ServerError{ErrorKind::NO_PLAYABLE_FORMAT, info.url,
                                               info.client_id, std::nullopt, std::nullopt});
            }
            continue;
        }
        eligible.push_back({player_timing(*state), int64_t{support->buffer_capacity}, *format,
                            this->pipeline_latency(*format, input_rate)});
        add_format(*format);
    }
    const GroupParams params = group_params(eligible, this->timing_, this->input_kind_);
    this->input_->set_group_params(params);
    this->group_lead_us_ = params.group_lead_us;
    // A new lead or write-ahead moves when the input has room
    this->poll_writable();

    // One instance per format, keeping its id while the format is wanted
    PipelineSet set;
    for (const sched::AudioFormat& format : formats) {
        const std::vector<PipelineRequest>& frames = this->pipelines_.frames;
        const auto existing =
            std::find_if(frames.begin(), frames.end(),
                         [&](const PipelineRequest& request) { return request.format == format; });
        if (existing != frames.end()) {
            set.frames.push_back(*existing);
            continue;
        }
        const sched::InstanceId instance{this->next_instance_++};
        const uint32_t cells = sched::frame_layout(format.sample_rate, format.codec).cells;
        // A flush's pending epoch has no frame yet, so the instance starts at its index 0
        const int64_t from_index = this->playing_ && this->anchor_
                                       ? this->start_index(*this->anchor_, cells, now_us())
                                       : 0;
        set.frames.push_back({instance, this->epoch_, format, from_index});
        sched::FrameRing ring(instance, cells);
        ring.restart(this->playing_ ? this->epoch_ : sched::EpochId{}, from_index);
        this->rings_.emplace(instance, std::move(ring));
    }
    std::vector<VisualizerConfig> overflow;
    set.analyses = this->analyses(overflow);
    // Past the cap, a configuration fails as an analysis that failed would
    this->fail_visualizers(overflow);
    if (set != this->pipelines_) {
        std::erase_if(this->rings_, [&set](const auto& entry) {
            const auto same = [&](const auto& request) { return request.instance == entry.first; };
            return std::none_of(set.frames.begin(), set.frames.end(), same) &&
                   std::none_of(set.analyses.begin(), set.analyses.end(), same);
        });
        this->pipelines_ = std::move(set);
        this->slot_->publish(this->pipelines_.frames, this->pipelines_.analyses);
        this->audio_->wake();
    }
    // A configuration newly past the cap is one a client/state or a join just asked for, whose own
    // marker, taken after this recompute, ends or holds back the stream on it
    if (!overflow.empty()) {
        this->schedule_check();
    }
}

std::vector<AnalysisRequest> Group::analyses(std::vector<VisualizerConfig>& overflow) {
    std::vector<VisualizerConfig> configs;
    const auto add_config = [&configs](const VisualizerConfig& config) {
        if (std::find(configs.begin(), configs.end(), config) == configs.end()) {
            configs.push_back(config);
        }
    };
    const auto failed = [this](const VisualizerConfig& config) {
        return std::find(this->failed_visualizers_.begin(), this->failed_visualizers_.end(),
                         config_key(config)) != this->failed_visualizers_.end();
    };
    // The running analyses keep their place under the cap
    const std::vector<AnalysisRequest>& current = this->pipelines_.analyses;
    for (const AnalysisRequest& request : current) {
        add_config(request.config);
    }
    std::vector<VisualizerConfig> used;
    for (const Member& member : this->members_) {
        const VisualizerStream& visualizer = member.member->session().visualizer();
        // An analysis stays while a cursor reads it, unless it failed
        if (visualizer.active() && !failed(visualizer.cursor_config())) {
            used.push_back(visualizer.cursor_config());
            add_config(visualizer.cursor_config());
        }
        if (visualizer.eligible() && visualizer.wanted() && !failed(*visualizer.wanted())) {
            used.push_back(*visualizer.wanted());
            add_config(*visualizer.wanted());
        }
    }
    // A running analysis no member uses any more goes
    std::erase_if(configs, [&used](const VisualizerConfig& config) {
        return std::find(used.begin(), used.end(), config) == used.end();
    });
    std::vector<AnalysisRequest> analyses;
    for (const VisualizerConfig& config : configs) {
        if (analyses.size() == MAX_GROUP_ANALYSES) {
            overflow.push_back(config);
            continue;
        }
        const auto existing =
            std::find_if(current.begin(), current.end(),
                         [&](const AnalysisRequest& request) { return request.config == config; });
        if (existing != current.end()) {
            analyses.push_back(*existing);
            continue;
        }
        const sched::InstanceId instance{this->next_instance_++};
        // An analysis added mid-epoch starts at the first cell at or after now
        const int64_t from_index = this->playing_ && this->anchor_
                                       ? sched::first_index_at_or_after(*this->anchor_, now_us(), 1)
                                       : 0;
        analyses.push_back({instance, this->epoch_, config, from_index});
        sched::FrameRing ring(instance, 1);
        ring.restart(this->playing_ ? this->epoch_ : sched::EpochId{}, from_index);
        this->rings_.emplace(instance, std::move(ring));
    }
    return analyses;
}

void Group::on_analysis_failed(const PipelineFailed& failed) {
    const std::vector<AnalysisRequest>& analyses = this->pipelines_.analyses;
    const auto analysis = std::find_if(
        analyses.begin(), analyses.end(),
        [&](const AnalysisRequest& request) { return request.instance == failed.instance; });
    if (analysis == analyses.end()) {
        return;
    }
    const VisualizerConfig config = analysis->config;
    this->fail_visualizers(std::span(&config, 1));
    this->recompute();
    // A stream on the configuration ends
    this->queue_markers(std::nullopt);
    this->schedule_check();
}

void Group::fail_visualizers(std::span<const VisualizerConfig> configs) {
    for (const VisualizerConfig& config : configs) {
        for (const Member& member : this->members_) {
            const VisualizerStream& visualizer = member.member->session().visualizer();
            const bool streamed = visualizer.active() && visualizer.cursor_config() == config;
            if (streamed || (visualizer.eligible() && visualizer.wanted() == config)) {
                const ClientInfo& info = member.member->client_info();
                this->hooks_.error(ServerError{ErrorKind::PIPELINE_FAILED, info.url, info.client_id,
                                               std::nullopt, std::nullopt});
            }
        }
        this->failed_visualizers_.push_back(config_key(config));
    }
}

int64_t Group::pipeline_latency(const sched::AudioFormat& format, uint32_t input_rate) {
    if (this->input_kind_ != InputKind::LIVE) {
        return 0;
    }
    const auto key = std::make_pair(input_rate, format.sample_rate);
    auto it = this->filter_lengths_.find(key);
    if (it == this->filter_lengths_.end()) {
        it = this->filter_lengths_
                 .emplace(key,
                          Resampler::filter_length(input_rate, format.sample_rate, this->quality_))
                 .first;
    }
    const uint32_t cells = sched::frame_layout(format.sample_rate, format.codec).cells;
    return sched::pipeline_latency_us(
        {std::max<uint32_t>(cells, 1), it->second, input_rate, encoder_delay_us(format)},
        InputKind::LIVE, this->write_jitter_us_);
}

int64_t Group::start_index(const sched::Anchor& anchor, uint32_t cells, int64_t now) const {
    // Every start trigger's first frame is at least `J_min` past its stamp, and a switch finds its
    // target from there; the pipelines catch up from the staging ring's history
    return sched::first_index_at_or_after(anchor, now + this->timing_.join_min_us, cells);
}

std::function<void()> Group::guarded(std::function<void()> fn) {
    return [weak = std::weak_ptr<bool>(this->alive_), fn = std::move(fn)] {
        if (weak.lock()) {
            fn();
        }
    };
}

}  // namespace sendspin::server
