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

#include "host_view.h"

#include "clock.h"
#include "pcm_convert.h"
#include "sched/constants.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>

namespace sendspin::server {

namespace {

// Server-wide, so ids from two groups never compare equal
std::atomic<uint64_t> next_run_id{1};
std::atomic<uint64_t> next_epoch_id{1};
std::atomic<uint64_t> next_drain_id{1};

sched::RunId new_run_id() {
    return {next_run_id.fetch_add(1)};
}

sched::EpochId new_epoch_id() {
    return {next_epoch_id.fetch_add(1)};
}

/// The longest single wait in wait_writable()
constexpr int64_t MAX_WAIT_US = int64_t{24} * 60 * 60 * sched::US_PER_SECOND;

/// The bytes one frame of format takes
size_t frame_bytes_of(const InputFormat& format) {
    return sample_bytes(format.sample_format) * format.channels;
}

/// The staging ring's size for format: STAGING_RING_US of input, within STAGING_RING_MAX_BYTES
size_t ring_bytes_for(const InputFormat& format) {
    const size_t frame = frame_bytes_of(format);
    const auto by_time = static_cast<uint64_t>(
        sched::ceil_div(int64_t{format.sample_rate} * STAGING_RING_US, sched::US_PER_SECOND));
    const uint64_t by_bytes = STAGING_RING_MAX_BYTES / frame;
    return static_cast<size_t>(std::max<uint64_t>(1, std::min(by_time, by_bytes))) * frame;
}

/// Copies bytes between the ring and a flat buffer, from ring byte `byte` on, wrapping
void copy_out(std::span<const uint8_t> ring, uint64_t byte, uint8_t* to, size_t bytes) {
    while (bytes > 0) {
        const auto slot = static_cast<size_t>(byte % ring.size());
        const size_t run = std::min(bytes, ring.size() - slot);
        std::memcpy(to, ring.data() + slot, run);
        to += run;
        byte += run;
        bytes -= run;
    }
}

void copy_in(std::span<uint8_t> ring, uint64_t byte, const uint8_t* from, size_t bytes) {
    while (bytes > 0) {
        const auto slot = static_cast<size_t>(byte % ring.size());
        const size_t run = std::min(bytes, ring.size() - slot);
        std::memcpy(ring.data() + slot, from, run);
        from += run;
        byte += run;
        bytes -= run;
    }
}

/// Copies ring bytes [byte, byte + bytes) from one ring to the same counted offsets in another
void copy_between(std::span<const uint8_t> from, std::span<uint8_t> to, uint64_t byte,
                  size_t bytes) {
    while (bytes > 0) {
        const auto out = static_cast<size_t>(byte % from.size());
        const auto in = static_cast<size_t>(byte % to.size());
        const size_t run = std::min({bytes, from.size() - out, to.size() - in});
        std::memcpy(to.data() + in, from.data() + out, run);
        byte += run;
        bytes -= run;
    }
}

}  // namespace

bool is_valid_input(const InputFormat& format) {
    return format.sample_rate > 0 && format.sample_rate <= MAX_INPUT_RATE && format.channels > 0 &&
           format.channels <= MAX_INPUT_CHANNELS;
}

HostView::HostView(GroupId group, InputId id, const InputOptions& options,
                   const GroupParams& initial, std::shared_ptr<Dispatcher> dispatcher,
                   ServerListener* listener)
    : group_(group),
      id_(id),
      kind_(options.kind),
      write_jitter_us_(options.kind == InputKind::LIVE ? options.write_jitter_us : 0),
      dispatcher_(std::move(dispatcher)),
      listener_(listener),
      format_(options.format),
      params_(initial),
      group_lead_eff_us_(initial.group_lead_us) {
    // write() reuses this storage when it anchors, time_of() the provisional anchor's, and the
    // pipelines' shifts the anchor's, so none of them allocates
    this->anchor_.segments.reserve(MAX_STAGED_SEGMENTS);
    this->anchor_.shifts.reserve(MAX_ANCHOR_SHIFTS);
    this->provisional_.segments.reserve(MAX_STAGED_SEGMENTS);
    this->segments_.reserve(MAX_STAGED_SEGMENTS);
    this->segments_.push_back({0, options.format, 0});
    this->ring_ = RingStorage::allocate(ring_bytes_for(options.format));
}

HostView::FormatState HostView::format() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return {this->format_, this->format_generation_};
}

// ==============================================================================
// Host calls
// ==============================================================================

WriteResult HostView::write(const void* pcm, size_t frames) {
    AfterUnlock after;
    WriteResult result;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_ || this->released_) {
            return {0, InputState::CLOSED};
        }
        const int64_t now = now_us();
        this->host_calls_seen_ = this->host_calls_;
        const sched::WriteDecision decision =
            sched::decide_write(this->admission_input(), this->anchor_, frames, now);
        uint64_t taken = decision.frames;
        if (this->kind_ == InputKind::LIVE && this->run_.run == sched::RunState::PLAYING) {
            taken = this->lower_latency(taken, now);
        }
        if (taken > 0) {
            // The write that begins a run or a pending epoch anchors it
            if (this->run_.run == sched::RunState::NONE) {
                sched::view_anchor_write(this->run_, new_run_id(), new_epoch_id());
                this->anchor_at(now);
            } else if (this->run_.run == sched::RunState::PENDING) {
                sched::view_anchor_write(this->run_, {}, {});
                this->anchor_at(now);
            }
            copy_in(this->ring_.span(), this->byte_of(this->head_),
                    static_cast<const uint8_t*>(pcm),
                    static_cast<size_t>(taken) * frame_bytes_of(this->format_));
            this->head_ += taken;
        }
        if (decision.state == InputState::FULL) {
            this->writable_wanted_ = true;
        }
        if (taken > 0 || decision.state == InputState::FULL) {
            this->signal(after);
        }
        result = {static_cast<size_t>(taken), decision.state};
    }
    this->run_after(after);
    return result;
}

InputState HostView::state() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->state_locked(now_us());
}

InputState HostView::wait_writable(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(this->mutex_);
    const int64_t start = now_us();
    const int64_t deadline = timeout_us <= 0 ? start
                             : timeout_us > std::numeric_limits<int64_t>::max() - start
                                 ? std::numeric_limits<int64_t>::max()
                                 : start + timeout_us;
    const uint64_t seen = this->host_calls_seen_;
    for (;;) {
        const int64_t now = now_us();
        const InputState state = this->state_locked(now);
        if (state != InputState::FULL || this->host_calls_ != seen || now >= deadline) {
            this->host_calls_seen_ = this->host_calls_;
            return state;
        }
        const int64_t ready = sched::readiness_time(this->admission_input(), this->anchor_,
                                                    this->ready_samples(), now);
        // Within what a steady_clock duration holds; the loop waits again after a longer one
        const int64_t wait_us = std::min({deadline, ready, now + MAX_WAIT_US}) - now;
        this->changed_.wait_for(lock, std::chrono::microseconds(wait_us));
    }
}

void HostView::flush() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->end_waits();
        if (this->closed_ || this->run_.latched || this->run_.run == sched::RunState::NONE) {
            return;
        }
        const std::optional<sched::DrainResolution> resolution =
            sched::view_flush(this->run_, new_epoch_id());
        this->restart_positions();
        this->queue_drained(resolution, after);
        this->signal(after);
    }
    this->run_after(after);
}

DrainId HostView::drain() {
    AfterUnlock after;
    const DrainId id{next_drain_id.fetch_add(1)};
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->end_waits();
        this->drain_ids_.push(id);
        if (this->closed_) {
            this->queue_drained(sched::DrainResolution{DrainOutcome::STOPPED, 1}, after);
        } else {
            const bool joins = this->run_.drain.has_value();
            const std::optional<sched::DrainResolution> resolution = sched::view_drain(this->run_);
            if (resolution) {
                this->queue_drained(resolution, after);
            } else {
                if (!joins) {
                    this->drain_end_ = this->head_;
                }
                this->resolve_empty_drain(after);
                this->signal(after);
            }
        }
    }
    this->run_after(after);
    return id;
}

uint64_t HostView::pause() {
    AfterUnlock after;
    uint64_t position = 0;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->end_waits();
        if (this->closed_) {
            return 0;
        }
        const sched::RunState run = this->run_.run;
        if (!sched::view_pause(this->run_)) {
            return run == sched::RunState::PAUSED ? this->p_pause_ : 0;
        }
        // The timeline position at now, the same for every player
        this->p_pause_ = sched::pause_position(this->anchor_, this->p0_, this->head_,
                                               run == sched::RunState::PENDING, now_us());
        position = this->p_pause_;
        this->signal(after);
    }
    this->run_after(after);
    return position;
}

void HostView::resume() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->end_waits();
        // A live input resumes at the head, anchored now; its input arrives in real time
        const bool live = this->kind_ == InputKind::LIVE;
        if (this->closed_ || !sched::view_resume(this->run_, new_run_id(), new_epoch_id(),
                                                 live || this->head_ > this->p_pause_)) {
            return;
        }
        this->p0_ = live ? this->head_ : this->p_pause_;
        if (this->run_.run == sched::RunState::PLAYING) {
            this->anchor_at(now_us());
        } else {
            this->anchor_valid_ = false;
        }
        this->resolve_empty_drain(after);
        this->signal(after);
    }
    this->run_after(after);
}

void HostView::stop() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->end_waits();
        if (this->closed_) {
            return;
        }
        const std::optional<sched::DrainResolution> resolution = sched::view_stop(this->run_);
        this->restart_positions();
        this->queue_drained(resolution, after);
        this->signal(after);
    }
    this->run_after(after);
}

bool HostView::set_format(const InputFormat& format) {
    AfterUnlock after;
    // A larger ring is allocated with the mutex released, so write() and core's calls never wait on
    // it; the ring it replaces is freed after the unlock too
    RingStorage storage;
    RingStorage replaced;
    size_t wanted = 0;
    while (true) {
        if (storage.size != wanted) {
            storage = RingStorage::allocate(wanted);
        }
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_ || !is_valid_input(format) || this->run_.drain) {
            this->end_waits();
            return false;
        }
        const bool between_runs = this->run_.run == sched::RunState::NONE;
        const size_t needed = between_runs ? ring_bytes_for(format)
                                           : std::max(this->ring_.size, ring_bytes_for(format));
        if (needed != this->ring_.size && storage.size != needed) {
            wanted = needed;
            continue;
        }
        this->end_waits();
        if (between_runs) {
            // The next run's input: nothing is staged
            this->format_ = format;
            this->restart_positions();
        } else {
            // A new segment at the head; one that holds no input yet takes the format in place
            const bool anchored = this->run_.run == sched::RunState::PLAYING;
            if (this->segments_.back().position == this->head_) {
                this->segments_.back().format = format;
                if (anchored && this->anchor_.segments.back().position == this->head_) {
                    this->anchor_.segments.back().rate = format.sample_rate;
                    this->anchor_.segments.back().channels = format.channels;
                }
            } else {
                this->prune_segments();
                if (this->segments_.size() >= MAX_STAGED_SEGMENTS) {
                    return false;
                }
                this->segments_.push_back({this->head_, format, this->byte_of(this->head_)});
                if (anchored) {
                    sched::add_segment(this->anchor_, this->head_, format.sample_rate,
                                       format.channels);
                }
            }
            this->format_ = format;
            if (needed != this->ring_.size) {
                // Each retained byte keeps its counted offset, at its slot in the larger ring
                const uint64_t from = this->byte_of(this->retain_from_);
                const uint64_t to = this->byte_of(this->head_);
                copy_between(this->ring_.span(), storage.span(), from,
                             static_cast<size_t>(to - from));
            }
        }
        if (needed != this->ring_.size) {
            replaced = std::exchange(this->ring_, std::move(storage));
        }
        ++this->format_generation_;
        this->signal(after);
        break;
    }
    this->run_after(after);
    return true;
}

int64_t HostView::time_of(uint64_t position) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    return this->time_of_locked(position, now_us());
}

int64_t HostView::time_of_locked(uint64_t position, int64_t now) const {
    uint64_t from = this->p0_;
    switch (this->run_.run) {
        case sched::RunState::PLAYING:
            return sched::time_of(this->anchor_, position);
        case sched::RunState::PAUSED:
            // Before the coming epoch's p0, the ended epoch's time; from it on, the time it would
            // have if resume() were called now. A live input's coming p0 is the head.
            from = this->kind_ == InputKind::LIVE ? this->head_ : this->p_pause_;
            if (this->anchor_valid_ && position < from) {
                return sched::time_of(this->anchor_, position);
            }
            break;
        case sched::RunState::NONE:
        case sched::RunState::PENDING:
            break;
    }
    // Provisional: as if anchored now, through the staged segments
    this->build_anchor(this->provisional_, now + this->params_.group_lead_us, from);
    return sched::time_of(this->provisional_, position);
}

int64_t HostView::latency_us() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    const int64_t now = now_us();
    return this->time_of_locked(this->head_, now) - now;
}

void HostView::release() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->released_ = true;
        this->mark_gone();
        this->end_waits();
        if (this->closed_) {
            return;
        }
        const std::optional<sched::DrainResolution> resolution = sched::view_stop(this->run_);
        this->restart_positions();
        this->queue_drained(resolution, after);
        this->signal(after);
    }
    this->run_after(after);
}

// ==============================================================================
// Core
// ==============================================================================

void HostView::attach(const Wakeup::Handle& wakeup, std::shared_ptr<AudioWorkers::Task> audio) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (!this->closed_) {
        this->wakeup_ = wakeup;
        this->audio_ = std::move(audio);
    }
}

void HostView::close() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (this->closed_) {
            return;
        }
        this->closed_ = true;
        this->mark_gone();
        this->wakeup_.reset();
        this->audio_.reset();
        const std::optional<sched::DrainResolution> resolution = sched::view_stop(this->run_);
        this->restart_positions();
        this->queue_drained(resolution, after);
        // A host call may have queued callbacks and not posted them yet, and the dispatcher may
        // finish before it does: core posts too, and the later task finds the outbox empty
        if (!this->outbox_.empty() && !this->outbox_held_) {
            this->outbox_posted_ = true;
            after.outbox = this->shared_from_this();
        }
        this->changed_.notify_all();
    }
    this->run_after(after);
}

void HostView::set_group_params(const GroupParams& params) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (params == this->params_) {
        return;
    }
    this->params_ = params;
    if (this->run_.run == sched::RunState::PLAYING) {
        this->group_lead_eff_us_ = std::max(this->group_lead_eff_us_, params.group_lead_us);
    }
    this->changed_.notify_all();
}

sched::ServerStopResult HostView::server_stop() {
    AfterUnlock after;
    sched::ServerStopResult result;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        result = sched::view_server_stop(this->run_);
        if (result.applied) {
            this->restart_positions();
            this->queue_drained(result.drain, after);
            this->signal(after);
            this->changed_.notify_all();
        }
    }
    this->run_after(after);
    return result;
}

bool HostView::complete_drain(sched::EpochId epoch) {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const std::optional<sched::DrainResolution> resolution =
            sched::view_drain_complete(this->run_, epoch);
        if (!resolution) {
            return false;
        }
        this->restart_positions();
        this->outbox_held_ = true;
        this->queue_drained(resolution, after);
        this->signal(after);
        this->changed_.notify_all();
    }
    this->run_after(after);
    return true;
}

void HostView::release_drained() {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->outbox_held_ = false;
        if (!this->outbox_.empty() && !this->outbox_posted_) {
            this->outbox_posted_ = true;
            after.outbox = this->shared_from_this();
        }
    }
    this->run_after(after);
}

int64_t HostView::poll_writable(int64_t now_us) {
    AfterUnlock after;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        if (!this->writable_wanted_ || this->closed_ || this->released_) {
            return sched::NEVER;
        }
        const int64_t ready = sched::readiness_time(this->admission_input(), this->anchor_,
                                                    this->ready_samples(), now_us);
        if (ready > now_us) {
            return ready;
        }
        this->writable_wanted_ = false;
        this->queue_entry({true, {}, DrainOutcome::PLAYED}, after);
    }
    this->run_after(after);
    return sched::NEVER;
}

bool HostView::add_shift(sched::EpochId epoch, int64_t index, int64_t delta_us) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->run_.epoch != epoch || this->run_.run != sched::RunState::PLAYING ||
        this->anchor_.shifts.size() >= MAX_ANCHOR_SHIFTS) {
        return false;
    }
    sched::add_shift(this->anchor_, index, delta_us);
    this->changed_.notify_all();
    return true;
}

void HostView::publish_shift_index(sched::EpochId epoch, std::optional<int64_t> index,
                                   int64_t pipeline_latency_us) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->run_.epoch != epoch || this->run_.run != sched::RunState::PLAYING ||
        (this->shift_index_ == index && this->rebase_latency_us_ == pipeline_latency_us)) {
        return;
    }
    const int64_t now = now_us();
    const int64_t ready_before =
        sched::readiness_time(this->admission_input(), this->anchor_, this->ready_samples(), now);
    this->shift_index_ = index;
    this->rebase_latency_us_ = pipeline_latency_us;
    this->notify_if_sooner(ready_before, now);
}

JobView HostView::job_view() const {
    // Reserved before the lock, so the copy under it allocates nothing
    JobView view;
    view.segments.reserve(MAX_STAGED_SEGMENTS);
    view.anchor.segments.reserve(MAX_STAGED_SEGMENTS);
    view.anchor.shifts.reserve(MAX_ANCHOR_SHIFTS);
    this->read_job_view(view);
    return view;
}

void HostView::read_job_view(JobView& view) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    view.run = this->run_;
    view.anchor = this->anchor_;
    view.p0 = this->p0_;
    view.head = this->head_;
    view.retain_from = this->retain_from_;
    view.drain_end = this->run_.drain ? this->drain_end_ : std::nullopt;
    view.segments = this->segments_;
    view.gone = this->closed_ || this->released_;
    view.group_lead_eff_us = this->group_lead_eff_us_;
    view.output_delay_max_us = this->params_.output_delay_max_us;
    view.kind = this->kind_;
    view.write_jitter_us = this->write_jitter_us_;
}

bool HostView::gone() const {
    return this->gone_.load();
}

bool HostView::read(sched::EpochId epoch, uint64_t position, size_t frames,
                    std::vector<uint8_t>& out) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->run_.epoch != epoch || position < this->retain_from_ ||
        position + frames > this->head_) {
        return false;
    }
    const StagedSegment& segment = this->segment_at(position);
    if (&segment != &this->segments_.back() && position + frames > (&segment + 1)->position) {
        return false;
    }
    out.resize(frames * frame_bytes_of(segment.format));
    copy_out(this->ring_.span(), this->byte_of(position), out.data(), out.size());
    return true;
}

void HostView::release_before(sched::EpochId epoch, uint64_t position) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    const bool playing = this->run_.run == sched::RunState::PLAYING;
    const bool live_pause =
        this->run_.run == sched::RunState::PAUSED && this->kind_ == InputKind::LIVE;
    if (this->run_.epoch != epoch || (!playing && !live_pause)) {
        return;
    }
    const uint64_t released = std::min(position, this->head_);
    if (released <= this->retain_from_) {
        return;
    }
    const int64_t now = now_us();
    const int64_t ready_before =
        sched::readiness_time(this->admission_input(), this->anchor_, this->ready_samples(), now);
    this->retain_from_ = released;
    this->prune_segments();
    if (playing) {
        sched::fold_shifts(this->anchor_, sched::first_kept_cell(this->anchor_, released));
    }
    this->notify_if_sooner(ready_before, now);
}

// ==============================================================================
// Helpers
// ==============================================================================

InputState HostView::state_locked(int64_t now_us) const {
    if (this->closed_ || this->released_) {
        return InputState::CLOSED;
    }
    if (this->run_.latched) {
        return InputState::STOP_LATCHED;
    }
    if (this->run_.drain) {
        return InputState::DRAINING;
    }
    return sched::admitted_past_head(this->admission_input(), this->anchor_, now_us) >=
                   this->ready_samples()
               ? InputState::OPEN
               : InputState::FULL;
}

void HostView::end_waits() {
    ++this->host_calls_;
    this->changed_.notify_all();
}

void HostView::notify_if_sooner(int64_t ready_before_us, int64_t now_us) {
    if (this->state_locked(now_us) != InputState::FULL ||
        sched::readiness_time(this->admission_input(), this->anchor_, this->ready_samples(),
                              now_us) < ready_before_us) {
        this->changed_.notify_all();
    }
}

void HostView::mark_gone() {
    this->gone_.store(true);
}

sched::AdmissionInput HostView::admission_input() const {
    sched::AdmissionInput in;
    in.kind = this->kind_;
    in.run = this->run_.run;
    in.draining = this->run_.drain.has_value();
    in.latched = this->run_.latched;
    in.head = this->head_;
    in.p0 = this->p0_;
    in.p_pause = this->p_pause_;
    in.head_rate = this->format_.sample_rate;
    in.group_lead_us = this->run_.run == sched::RunState::PLAYING ? this->group_lead_eff_us_
                                                                  : this->params_.group_lead_us;
    in.write_ahead_us = this->params_.write_ahead_us;
    // What the ring has room for past the head, in the head's format
    const uint64_t used = this->byte_of(this->head_) - this->byte_of(this->retain_from_);
    const uint64_t room = this->ring_.size > used ? this->ring_.size - used : 0;
    in.ring_limit = this->head_ + (room / frame_bytes_of(this->format_));
    in.shift_index = this->shift_index_;
    in.rebase.kind = this->kind_;
    in.rebase.output_delay_max_us = this->params_.output_delay_max_us;
    in.rebase.group_lead_us = this->group_lead_eff_us_;
    in.rebase.pipeline_latency_us = this->rebase_latency_us_;
    in.rebase.live_jitter_us = this->write_jitter_us_;
    return in;
}

uint64_t HostView::ready_samples() const {
    return static_cast<uint64_t>(
        sched::ceil_div(this->format_.sample_rate, sched::CELLS_PER_SECOND));
}

void HostView::anchor_at(int64_t now_us) {
    this->build_anchor(this->anchor_, now_us + this->params_.group_lead_us, this->p0_);
    this->anchor_valid_ = true;
    this->group_lead_eff_us_ = this->params_.group_lead_us;
    this->shift_index_.reset();
    this->rebase_latency_us_ = 0;
    this->lowering_ = {};
}

uint64_t HostView::lower_latency(uint64_t frames, int64_t now_us) {
    const int64_t excess =
        sched::time_of(this->anchor_, this->head_) - now_us - this->params_.group_lead_us;
    const int64_t debt = sched::observe_live_write(this->lowering_, excess, now_us);
    const uint32_t rate = this->format_.sample_rate;
    const auto cut = std::min<uint64_t>(
        frames, static_cast<uint64_t>(std::max<int64_t>(debt, 0) * rate / sched::US_PER_SECOND));
    if (cut == 0) {
        return frames;
    }
    const auto cut_us = static_cast<int64_t>(cut) * sched::US_PER_SECOND / rate;
    sched::record_cut(this->lowering_, cut_us);
    // The rebase threshold falls with the latency, down to the lead every player needs, so the
    // next frame does not shift the timeline back up
    this->group_lead_eff_us_ =
        std::max(this->params_.group_lead_us, this->group_lead_eff_us_ - cut_us);
    return frames - cut;
}

void HostView::resolve_empty_drain(AfterUnlock& after) {
    // A pending epoch with no input can take none once draining, so its drain has nothing to play
    // and completes at once
    if (this->run_.run != sched::RunState::PENDING || !this->run_.drain ||
        this->head_ != this->p0_) {
        return;
    }
    const std::optional<sched::DrainResolution> resolution =
        sched::view_drain_complete(this->run_, this->run_.epoch);
    this->restart_positions();
    this->queue_drained(resolution, after);
}

void HostView::build_anchor(sched::Anchor& anchor, int64_t t0_us, uint64_t p0) const {
    const InputFormat& first = this->segment_at(p0).format;
    anchor.t0_us = t0_us;
    anchor.segments.clear();
    anchor.segments.push_back({p0, t0_us, first.sample_rate, first.channels});
    anchor.shift_base_us = 0;
    anchor.shifts.clear();
    for (const StagedSegment& segment : this->segments_) {
        if (segment.position > p0) {
            sched::add_segment(anchor, segment.position, segment.format.sample_rate,
                               segment.format.channels);
        }
    }
}

const StagedSegment& HostView::segment_at(uint64_t position) const {
    const auto it = std::upper_bound(
        this->segments_.begin(), this->segments_.end(), position,
        [](uint64_t p, const StagedSegment& segment) { return p < segment.position; });
    return it == this->segments_.begin() ? this->segments_.front() : *std::prev(it);
}

uint64_t HostView::byte_of(uint64_t position) const {
    const StagedSegment& segment = this->segment_at(position);
    return segment.byte + ((position - segment.position) * frame_bytes_of(segment.format));
}

void HostView::prune_segments() {
    // A position below the staged input then takes the oldest segment kept, as one before p0 takes
    // the first
    const auto ended = [this](const auto& segments) {
        size_t count = 0;
        while (count + 1 < segments.size() && segments[count + 1].position <= this->retain_from_) {
            ++count;
        }
        return static_cast<std::ptrdiff_t>(count);
    };
    this->segments_.erase(this->segments_.begin(),
                          this->segments_.begin() + ended(this->segments_));
    if (this->run_.run == sched::RunState::PLAYING) {
        this->anchor_.segments.erase(
            this->anchor_.segments.begin(),
            this->anchor_.segments.begin() + ended(this->anchor_.segments));
    }
}

void HostView::restart_positions() {
    this->segments_.clear();
    this->segments_.push_back({0, this->format_, 0});
    this->head_ = 0;
    this->p0_ = 0;
    this->p_pause_ = 0;
    this->retain_from_ = 0;
    this->drain_end_.reset();
    this->anchor_valid_ = false;
    this->shift_index_.reset();
}

void HostView::queue_drained(const std::optional<sched::DrainResolution>& resolution,
                             AfterUnlock& after) {
    if (!resolution) {
        return;
    }
    // Decided under the mutex, so a racing resolution finds nothing left; the outbox keeps the
    // order
    for (uint32_t i = 0; i < resolution->callbacks && !this->drain_ids_.empty(); ++i) {
        this->queue_entry({false, this->drain_ids_.front(), resolution->outcome}, after);
        this->drain_ids_.pop_front();
    }
}

void HostView::queue_entry(const OutboxEntry& entry, AfterUnlock& after) {
    if (this->listener_ == nullptr) {
        return;
    }
    this->outbox_.push_back(entry);
    if (!this->outbox_posted_ && !this->outbox_held_) {
        this->outbox_posted_ = true;
        after.outbox = this->shared_from_this();
    }
}

void HostView::signal(AfterUnlock& after) const {
    after.wakeup = this->wakeup_;
    after.audio = this->audio_;
}

void HostView::run_after(AfterUnlock& after) const {
    Wakeup::signal(after.wakeup);
    if (after.audio) {
        after.audio->wake();
    }
    if (after.outbox) {
        this->dispatcher_->post([view = std::move(after.outbox)] { view->deliver_outbox(); });
    }
}

void HostView::deliver_outbox() {
    std::vector<OutboxEntry> entries;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->outbox_posted_ = false;
        if (this->outbox_held_) {
            // release_drained() posts again
            return;
        }
        entries.swap(this->outbox_);
    }
    for (const OutboxEntry& entry : entries) {
        if (entry.writable) {
            this->listener_->on_writable(this->group_, this->id_);
        } else {
            this->listener_->on_drained(this->group_, this->id_, entry.drain, entry.outcome);
        }
    }
}

// ==============================================================================
// GroupInputs
// ==============================================================================

bool GroupInputs::available(GroupId group) {
    std::shared_ptr<HostView> held;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        const auto it = this->inputs_.find(group);
        if (it != this->inputs_.end()) {
            held = it->second.lock();
        }
    }
    // Released outside the lock, which may destroy the view
    return !held || held->gone();
}

bool GroupInputs::claim(GroupId group, const std::shared_ptr<HostView>& view) {
    std::shared_ptr<HostView> held;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        std::erase_if(this->inputs_, [](const auto& entry) { return entry.second.expired(); });
        held = this->inputs_[group].lock();
        if (held && !held->gone()) {
            return false;
        }
        this->inputs_[group] = view;
    }
    // Released outside the lock, which may destroy the view
    return true;
}

// ==============================================================================
// InputRegistry
// ==============================================================================

bool InputRegistry::add(const std::shared_ptr<HostView>& view) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (this->closed_) {
        return false;
    }
    this->views_.push_back(view);
    return true;
}

void InputRegistry::remove(const HostView& view) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    std::erase_if(this->views_,
                  [&view](const std::shared_ptr<HostView>& entry) { return entry.get() == &view; });
}

void InputRegistry::close_all() {
    std::vector<std::shared_ptr<HostView>> views;
    {
        const std::lock_guard<std::mutex> lock(this->mutex_);
        this->closed_ = true;
        views.swap(this->views_);
    }
    for (const std::shared_ptr<HostView>& view : views) {
        view->close();
    }
}

}  // namespace sendspin::server
