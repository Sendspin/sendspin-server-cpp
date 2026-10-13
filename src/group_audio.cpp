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

#include "group_audio.h"

#include "pcm_convert.h"
#include "sched/constants.h"
#include "sched/leads.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace sendspin::server {

namespace {

/// The most samples a segment boundary extends a stretch by before its pipeline fails: one
/// from the boundary index's rounding, and one to spare
constexpr int64_t MAX_BOUNDARY_EXTENSION = 2;

/// Samples of input history kept past a warm-up's bound: one for the rounding between a start's
/// cell_position() and begin()'s position for it, and one to spare
constexpr uint64_t HISTORY_SLACK = 2;

/// The most input samples, over all channels, an analysis reads at once: 256 / channels frames, so
/// its float buffer is 1 KB at any channel count
constexpr size_t ANALYSIS_READ_SAMPLES = 256;
static_assert(MAX_INPUT_CHANNELS <= ANALYSIS_READ_SAMPLES);

/// The stages' floats as the encoder takes F32 samples
std::span<const uint8_t> float_bytes(std::span<const float> samples) {
    return {reinterpret_cast<const uint8_t*>(samples.data()), samples.size_bytes()};
}

/// The input rate of the anchored segment holding position
uint32_t segment_rate(const sched::Anchor& anchor, uint64_t position) {
    uint32_t rate = anchor.segments.front().rate;
    for (const sched::Segment& segment : anchor.segments) {
        if (segment.position <= position) {
            rate = segment.rate;
        }
    }
    return rate;
}

}  // namespace

// ==============================================================================
// GroupToCoreFifo
// ==============================================================================

GroupToCoreFifo::GroupToCoreFifo(EventLoop& loop, std::function<void(GroupEvent&)> deliver)
    : loop_(loop), state_(std::make_shared<State>()) {
    this->state_->deliver = std::move(deliver);
}

GroupToCoreFifo::~GroupToCoreFifo() {
    Ring<GroupEvent> dropped;
    {
        const std::lock_guard<std::mutex> lock(this->state_->mutex);
        this->state_->closed = true;
        dropped.swap(this->state_->events);
    }
    this->state_->deliver = nullptr;
}

GroupToCoreFifo::Producer GroupToCoreFifo::producer() const {
    return {this->loop_, this->state_};
}

GroupToCoreFifo::Producer::Producer(EventLoop& loop, std::shared_ptr<State> state)
    : loop_(&loop), state_(std::move(state)) {}

void GroupToCoreFifo::Producer::push(GroupEvent event) const {
    bool post = false;
    {
        const std::lock_guard<std::mutex> lock(this->state_->mutex);
        if (this->state_->closed) {
            return;
        }
        this->state_->events.push(std::move(event));
        post = !std::exchange(this->state_->drain_posted, true);
    }
    if (post) {
        GroupToCoreFifo::drain(*this->loop_, this->state_);
    }
}

// At most one handler is pending, and none leaves an event stranded
void GroupToCoreFifo::drain(EventLoop& loop, const std::shared_ptr<State>& state) {
    loop.post([&loop, weak = std::weak_ptr<State>(state)] {
        const std::shared_ptr<State> state = weak.lock();
        if (!state) {
            return;
        }
        Ring<GroupEvent>& batch = state->taking;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            while (!state->events.empty() && batch.try_push(std::move(state->events.front()))) {
                state->events.pop_front();
            }
        }
        for (; !batch.empty(); batch.pop_front()) {
            if (!state->deliver) {
                batch.clear();
                return;
            }
            state->deliver(batch.front());
        }
        bool again = false;
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            again = !state->events.empty();
            state->drain_posted = again;
        }
        if (again) {
            GroupToCoreFifo::drain(loop, state);
        }
    });
}

// ==============================================================================
// PipelineSetSlot
// ==============================================================================

void PipelineSetSlot::publish(std::vector<PipelineRequest> frames,
                              std::vector<AnalysisRequest> analyses) {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    this->set_ = {std::move(frames), std::move(analyses)};
    ++this->version_;
}

std::optional<PipelineSet> PipelineSetSlot::read_if_newer(uint64_t& seen) const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    if (seen == this->version_) {
        return std::nullopt;
    }
    seen = this->version_;
    return this->set_;
}

// ==============================================================================
// GroupAudio
// ==============================================================================

GroupAudio::GroupAudio(std::shared_ptr<HostView> view, GroupToCoreFifo::Producer fifo,
                       std::shared_ptr<const PipelineSetSlot> slot, ResamplerQuality quality)
    : view_(std::move(view)), fifo_(std::move(fifo)), slot_(std::move(slot)), quality_(quality) {
    // read_job_view() copies the segments under the mutex write() takes: into reserved storage,
    // so the copy never allocates
    this->job_.segments.reserve(MAX_STAGED_SEGMENTS);
    this->job_.anchor.segments.reserve(MAX_STAGED_SEGMENTS);
    this->job_.anchor.shifts.reserve(MAX_ANCHOR_SHIFTS);
}

bool GroupAudio::run_job(int64_t now_us) {
    this->view_->read_job_view(this->job_);
    JobView& view = this->job_;
    this->built_this_job_ = false;
    this->waiting_ = false;
    this->follow_epochs(view);
    this->follow_pipeline_set(view);
    // Building a resampler allocates its filter bank: at most one per job
    const bool unbuilt = this->configure_one(view);
    if (view.run.run == sched::RunState::PAUSED && view.kind == InputKind::LIVE && !view.gone) {
        this->release_live_pause(view);
    }
    if (!this->started_ || !this->active_ || view.run.run != sched::RunState::PLAYING ||
        view.run.epoch != this->epoch_) {
        return unbuilt;
    }
    const std::optional<int64_t> held = this->rebase(view, now_us);
    bool progressed = unbuilt;
    for (Instance& instance : this->instances_) {
        // No frame at or past k* before its rebase is decided
        if (held && instance.next >= *held && !instance.failed) {
            continue;
        }
        progressed = this->produce(instance, view) || progressed;
    }
    for (Analysis& analysis : this->analyses_) {
        // Nothing at or past k* before its rebase is decided
        if (held && analysis.next >= *held && !analysis.failed) {
            continue;
        }
        progressed = this->analyze(analysis, view, now_us, held) || progressed;
    }
    progressed = this->post_drain_started(view) || progressed;
    // The ring keeps what a pause or a new instance may still need: from the position playing now
    // less the largest warm-up, and what no instance has consumed yet
    uint64_t keep = sched::pause_position(view.anchor, view.p0, view.head, false, now_us);
    uint32_t playing_rate = view.segments.front().format.sample_rate;
    for (const StagedSegment& segment : view.segments) {
        if (segment.position <= keep) {
            playing_rate = segment.format.sample_rate;
        }
    }
    uint64_t warmup = 0;
    for (const Instance& instance : this->instances_) {
        if (instance.failed) {
            continue;
        }
        // A resume warms a resampled instance up from history before the paused position
        if (instance.format.sample_rate != playing_rate) {
            warmup = std::max(warmup, this->history_samples(instance, playing_rate));
        }
        if (instance.begun) {
            keep = std::min(keep, instance.fed);
            continue;
        }
        const uint64_t first = sched::cell_position(view.anchor, instance.start);
        const uint32_t rate = this->start_rate(instance, view);
        const uint64_t h =
            instance.format.sample_rate == rate ? 0 : this->history_samples(instance, rate);
        keep = std::min(keep, first - std::min(first, h));
    }
    for (const Analysis& analysis : this->analyses_) {
        if (analysis.failed) {
            continue;
        }
        if (analysis.begun) {
            keep = std::min(keep, analysis.fed);
            continue;
        }
        // An analysis starts with a window of history before its start
        const uint64_t first = sched::cell_position(view.anchor, analysis.start);
        const size_t window = analysis_window(segment_rate(view.anchor, first));
        keep = std::min(keep, first - std::min<uint64_t>(first, window));
    }
    keep -= std::min(keep, warmup);
    this->view_->release_before(this->epoch_, keep);
    return progressed || this->waiting_;
}

void GroupAudio::follow_epochs(const JobView& view) {
    if (view.gone) {
        if (this->active_) {
            this->fifo_.push(EpochEnded{this->epoch_});
            this->active_ = false;
        }
        return;
    }
    if (view.run.epoch != this->epoch_) {
        if (this->active_) {
            const bool flushed =
                view.run.run_id == this->run_ && (view.run.run == sched::RunState::PENDING ||
                                                  view.run.run == sched::RunState::PLAYING);
            if (flushed) {
                this->fifo_.push(EpochCut{view.run.epoch});
            } else {
                this->fifo_.push(EpochEnded{this->epoch_});
                this->active_ = false;
            }
        }
        this->epoch_ = view.run.epoch;
        this->run_ = view.run.run_id;
        this->started_ = false;
        this->drain_posted_ = false;
    }
    if (view.run.run == sched::RunState::PLAYING && !this->started_) {
        EpochStarted started{view.run.run_id, view.run.epoch, view.anchor, std::nullopt};
        if (view.drain_end) {
            started.drain = DrainStarted{view.run.epoch, this->last_cell(view)};
            this->drain_posted_ = true;
        }
        this->fifo_.push(std::move(started));
        this->started_ = true;
        this->active_ = true;
        // Every instance resets
        for (Instance& instance : this->instances_) {
            instance.start = 0;
            instance.next = 0;
            instance.last = -1;
            instance.final_posted = false;
            instance.begun = false;
            instance.flushed = false;
            instance.pending.clear();
            if (instance.failed || !instance.configured) {
                continue;
            }
            // The epoch may start in another segment's rate: its resampler is built by
            // configure_one()
            const uint32_t rate = this->start_rate(instance, view);
            if (rate != instance.format.sample_rate && rate != instance.resampler_rate) {
                instance.configured = false;
                continue;
            }
            instance.encoder->reset();
        }
        for (Analysis& analysis : this->analyses_) {
            analysis.start = 0;
            analysis.next = 0;
            analysis.last = -1;
            analysis.final_posted = false;
            analysis.begun = false;
        }
    }
    if (this->active_ &&
        (view.run.run == sched::RunState::PAUSED || view.run.run == sched::RunState::NONE)) {
        this->fifo_.push(EpochEnded{this->epoch_});
        this->active_ = false;
    }
}

std::optional<int64_t> GroupAudio::rebase(JobView& view, int64_t now_us) {
    this->nexts_.clear();
    size_t reach = 0;
    uint32_t widest = 1;
    sched::RebaseParams params;
    params.kind = view.kind;
    params.output_delay_max_us = view.output_delay_max_us;
    params.group_lead_us = view.group_lead_eff_us;
    params.live_jitter_us = view.write_jitter_us;
    for (const Instance& instance : this->instances_) {
        if (instance.failed) {
            continue;
        }
        this->nexts_.push_back({instance.next, instance.cells});
        widest = std::max(widest, instance.cells);
        const uint32_t rate = instance.begun ? instance.run_rate : this->start_rate(instance, view);
        sched::PipelineShape shape{instance.cells, 0, rate, encoder_delay_us(instance.format)};
        if (instance.resampler && rate != instance.format.sample_rate) {
            reach = std::max(reach, instance.resampler->reach());
            shape.resampler_filter_length = instance.resampler->filter_length();
        }
        // The live row takes the largest `L_p` over the frame instances; an Opus instance's covers
        // its encoder's lookahead (encoder_delay_us()), which it reads ahead of each frame
        params.pipeline_latency_us =
            std::max(params.pipeline_latency_us,
                     sched::pipeline_latency_us(shape, view.kind, view.write_jitter_us));
    }
    // `k*` is past every analysis's frontier too, which a resampled or multi-cell instance's next
    // frame can lag behind; an analysis alone is a one-cell instance with no latency of its own
    this->frontiers_.clear();
    for (const Analysis& analysis : this->analyses_) {
        if (!analysis.failed) {
            this->frontiers_.push_back(analysis.next);
        }
    }
    // With neither there is nothing to produce: admission takes the plain horizon
    if (this->nexts_.empty() && this->frontiers_.empty()) {
        this->view_->publish_shift_index(this->epoch_, std::nullopt, 0);
        return std::nullopt;
    }
    // After DrainStarted, last_ts is final
    if (this->drain_posted_) {
        return std::nullopt;
    }
    const int64_t index = sched::shift_index(this->nexts_, this->frontiers_);
    this->view_->publish_shift_index(this->epoch_, index, params.pipeline_latency_us);
    // Decided when frame k* is about to be produced: the input of the widest frame from it is
    // staged, with the resampler's lookahead past it
    const uint64_t start = sched::cell_position(view.anchor, index);
    uint64_t end = sched::cell_position(view.anchor, index + widest) + reach;
    if (view.drain_end) {
        end = std::min(end, *view.drain_end);
    }
    if (start >= end) {
        return std::nullopt;
    }
    if (end > view.head) {
        return index;
    }
    const int64_t lead = sched::frame_timestamp(view.anchor, index) - now_us;
    if (!sched::rebase_needed(lead, params)) {
        return std::nullopt;
    }
    const int64_t delta = sched::rebase_delta(lead, params);
    if (this->view_->add_shift(this->epoch_, index, delta)) {
        sched::add_shift(view.anchor, index, delta);
        this->fifo_.push(AnchorShift{this->epoch_, index, delta});
    }
    return std::nullopt;
}

void GroupAudio::release_live_pause(const JobView& view) {
    // A paused live input resumes at the head, so the ring keeps only the history a resampled
    // instance warms up from before it
    const uint32_t rate = view.segments.back().format.sample_rate;
    uint64_t history = 0;
    for (const Instance& instance : this->instances_) {
        if (!instance.failed && instance.format.sample_rate != rate) {
            history = std::max(history, this->history_samples(instance, rate));
        }
    }
    this->view_->release_before(this->epoch_, view.head - std::min(view.head, history));
}

bool GroupAudio::post_drain_started(const JobView& view) {
    if (!view.drain_end || this->drain_posted_) {
        return false;
    }
    // Every instance has produced through P_end, so no rebase follows
    const bool produced =
        std::all_of(this->instances_.begin(), this->instances_.end(),
                    [&](const Instance& instance) { return this->finished(instance); });
    if (!produced) {
        return false;
    }
    this->fifo_.push(DrainStarted{this->epoch_, this->last_cell(view)});
    this->drain_posted_ = true;
    return true;
}

void GroupAudio::follow_pipeline_set(const JobView& view) {
    std::optional<PipelineSet> set = this->slot_->read_if_newer(this->slot_seen_);
    if (!set) {
        return;
    }
    this->requests_ = std::move(*set);
    const std::vector<PipelineRequest>& frames = this->requests_.frames;
    std::erase_if(this->instances_, [&](const Instance& instance) {
        return std::none_of(frames.begin(), frames.end(), [&](const PipelineRequest& request) {
            return request.instance == instance.id;
        });
    });
    this->follow_analyses(view);
    for (const PipelineRequest& request : frames) {
        const bool exists =
            std::any_of(this->instances_.begin(), this->instances_.end(),
                        [&](const Instance& instance) { return instance.id == request.instance; });
        if (exists) {
            continue;
        }
        Instance instance;
        instance.id = request.instance;
        instance.format = request.format;
        const sched::FrameLayout layout =
            sched::frame_layout(request.format.sample_rate, request.format.codec);
        instance.cells = std::max<uint32_t>(1, layout.cells);
        instance.frame_samples = layout.samples;
        instance.depth = quantized_depth(request.format);
        instance.sample_bytes = pcm_output_bytes(instance.depth) * request.format.channels;
        instance.lead = encoder_delay_samples(request.format);
        instance.last_sample.assign(instance.sample_bytes, 0);
        // Pending holds less than a frame, then one block at the output's rate and a sample of
        // extension; a resampler reserves for its own output when it is built
        instance.pending.reserve((size_t{instance.frame_samples} +
                                  static_cast<size_t>(sched::ceil_div(request.format.sample_rate,
                                                                      sched::CELLS_PER_SECOND)) +
                                  1) *
                                 instance.sample_bytes);
        // from_index applies in its own epoch once anchored
        if (this->started_) {
            instance.start = this->start_cell(
                view, request.epoch == this->epoch_ ? request.from_index : int64_t{0},
                instance.cells);
        }
        instance.next = instance.start;
        this->instances_.push_back(std::move(instance));
    }
}

bool GroupAudio::configure_one(const JobView& view) {
    bool waiting = false;
    for (Instance& instance : this->instances_) {
        if (instance.configured || instance.failed) {
            continue;
        }
        if (!instance.encoder) {
            // FLAC frames are independent, so a new instance's encoder starts its own stream at its
            // first frame
            instance.encoder = FrameEncoder::create(instance.format);
            if (!instance.encoder) {
                this->fail(instance);
                continue;
            }
        }
        const Stage stage = this->ensure_resampler(instance, this->start_rate(instance, view));
        if (stage == Stage::FAILED) {
            this->fail(instance);
            continue;
        }
        if (stage == Stage::WAIT) {
            waiting = true;
            continue;
        }
        instance.configured = true;
        instance.encoder->reset();
    }
    return waiting;
}

GroupAudio::Stage GroupAudio::ensure_resampler(Instance& instance, uint32_t rate) {
    if (rate == instance.format.sample_rate) {
        return Stage::READY;
    }
    if (instance.resampler && instance.resampler_rate == rate) {
        instance.resampler->reset();
        return Stage::READY;
    }
    if (this->built_this_job_) {
        return Stage::WAIT;
    }
    this->built_this_job_ = true;
    const auto block = static_cast<size_t>(sched::ceil_div(rate, sched::CELLS_PER_SECOND));
    instance.resampler.reset();
    instance.resampler_rate = 0;
    std::unique_ptr<Resampler> resampler = Resampler::create(
        rate, instance.format.sample_rate, instance.format.channels, this->quality_, block);
    if (!resampler) {
        return Stage::FAILED;
    }
    // Pending holds less than a frame, then one block's output, or the filter's tail and one
    // sample of extension at a boundary or a drain
    const uint64_t out_rate = instance.format.sample_rate;
    const uint64_t input = block + (2 * resampler->reach());
    instance.pending.reserve((instance.frame_samples + (input * out_rate / rate) + 3) *
                             instance.sample_bytes);
    instance.resampled.reserve(((input * out_rate / rate) + 2) * instance.format.channels *
                               sample_bytes(Resampler::SAMPLE_FORMAT));
    instance.resampler = std::move(resampler);
    instance.resampler_rate = rate;
    return Stage::READY;
}

int64_t GroupAudio::start_cell(const JobView& view, int64_t requested, uint32_t cells) const {
    // The first frame whose input the ring still holds; a resampled instance warms up from what it
    // holds before that cell
    const int64_t held =
        sched::first_index_at_or_after_position(view.anchor, view.retain_from, cells);
    return std::max(sched::instance_start(view.anchor, std::max<int64_t>(requested, 0), cells),
                    held);
}

uint32_t GroupAudio::start_rate(const Instance& instance, const JobView& view) const {
    if (!this->started_ || view.anchor.segments.empty()) {
        return view.segments.back().format.sample_rate;
    }
    const uint64_t position = sched::cell_position(view.anchor, instance.start);
    uint32_t rate = view.anchor.segments.front().rate;
    for (const sched::Segment& segment : view.anchor.segments) {
        if (segment.position <= position) {
            rate = segment.rate;
        }
    }
    return rate;
}

void GroupAudio::fail(Instance& instance) {
    if (instance.failed) {
        return;
    }
    instance.failed = true;
    this->fifo_.push(PipelineFailed{this->epoch_, instance.id});
}

bool GroupAudio::finished(const Instance& instance) const {
    if (instance.failed) {
        return true;
    }
    // An encoder with a delay owes frames past what pending holds, up to end_output
    return instance.configured && instance.begun && instance.flushed &&
           frame_start(instance, instance.next) >= instance.end_output;
}

bool GroupAudio::post_final(Instance& instance, const JobView& view) {
    if (!view.drain_end || instance.final_posted || !this->drain_posted_ ||
        !this->finished(instance)) {
        return false;
    }
    // The instance's own final index
    this->fifo_.push(InstanceFinal{this->epoch_, instance.id, instance.last});
    instance.final_posted = true;
    return true;
}

bool GroupAudio::produce(Instance& instance, const JobView& view) {
    if (instance.failed) {
        return false;
    }
    if (view.drain_end && this->finished(instance)) {
        return this->post_final(instance, view);
    }
    if (!instance.configured) {
        return false;
    }
    if (!instance.begun && !this->begin(instance, view)) {
        return instance.failed;
    }
    if (!this->fill(instance, view)) {
        return instance.failed;
    }
    size_t samples =
        std::min<size_t>(instance.pending.size() / instance.sample_bytes, instance.frame_samples);
    if (instance.flushed) {
        samples = static_cast<size_t>(std::clamp<int64_t>(
            instance.end_output - frame_start(instance, instance.next), 0, instance.frame_samples));
    }
    if (samples == 0) {
        // Cut at the drain position with nothing left: the instance has finished
        return this->post_final(instance, view);
    }
    if (!this->emit(instance, view, samples)) {
        this->fail(instance);
    }
    return true;
}

bool GroupAudio::begin(Instance& instance, const JobView& view) {
    const sched::Anchor& anchor = view.anchor;
    const uint32_t out_rate = instance.format.sample_rate;
    for (;;) {
        // Cell start's first output sample, and the segment holding it
        const int64_t first = frame_start(instance, instance.start);
        size_t segment = 0;
        while (segment + 1 < anchor.segments.size() &&
               sched::segment_output_index(anchor, segment + 1, out_rate) <= first) {
            ++segment;
        }
        const sched::Segment& in = anchor.segments[segment];
        const int64_t origin = sched::segment_output_index(anchor, segment, out_rate);
        const Stage stage = this->ensure_resampler(instance, in.rate);
        if (stage == Stage::FAILED) {
            this->fail(instance);
            return false;
        }
        if (stage == Stage::WAIT) {
            this->waiting_ = true;
            return false;
        }
        instance.run_rate = in.rate;
        instance.origin_position = in.position;
        instance.origin_output = origin;
        instance.stage_ready = true;
        instance.pending.clear();
        instance.flushed = false;
        if (in.rate == out_rate) {
            instance.fed = in.position + static_cast<uint64_t>(first - origin);
            if (instance.fed < view.retain_from) {
                // The ring no longer holds it: the next frame
                instance.start += instance.cells;
                instance.next = instance.start;
                continue;
            }
            instance.produced = first;
            instance.prime = false;
            break;
        }
        // The input position of the start, in this segment; a warm-up starts at a zero-phase
        // step from the segment's start, so its output samples fall on the stream's
        const uint64_t common = std::gcd(in.rate, out_rate);
        const auto step_in = static_cast<int64_t>(in.rate / common);
        const auto step_out = static_cast<int64_t>(out_rate / common);
        const auto base = static_cast<int64_t>(in.position);
        const int64_t at = base + sched::floor_div((first - origin) * in.rate, out_rate);
        // History of the same input: the staged segment holding the start, while staged
        int64_t lowest = static_cast<int64_t>(view.retain_from);
        for (const StagedSegment& staged : view.segments) {
            if (static_cast<int64_t>(staged.position) <= at) {
                lowest = std::max(lowest, static_cast<int64_t>(staged.position));
            }
        }
        const int64_t wanted =
            std::max(at - static_cast<int64_t>(this->warmup_samples(instance, in.rate)), lowest);
        int64_t steps = sched::floor_div(wanted - base, step_in);
        if (base + (steps * step_in) < lowest) {
            ++steps;
        }
        const int64_t from = base + (steps * step_in);
        if (from > at) {
            // No zero-phase step between the history and the start: the next frame
            instance.start += instance.cells;
            instance.next = instance.start;
            continue;
        }
        instance.fed = static_cast<uint64_t>(from);
        instance.produced = origin + (steps * step_out);
        instance.prime = static_cast<size_t>(at - from) < instance.resampler->reach();
        break;
    }
    instance.begun = true;
    return true;
}

bool GroupAudio::fill(Instance& instance, const JobView& view) {
    const size_t frame = size_t{instance.frame_samples} * instance.sample_bytes;
    while (instance.pending.size() < frame && !instance.flushed) {
        if (!instance.stage_ready) {
            const Stage stage = this->ensure_resampler(instance, instance.run_rate);
            if (stage == Stage::FAILED) {
                this->fail(instance);
                return false;
            }
            if (stage == Stage::WAIT) {
                this->waiting_ = true;
                return false;
            }
            instance.stage_ready = true;
        }
        if (view.drain_end && instance.fed >= *view.drain_end) {
            if (!this->finish_drain(instance, *view.drain_end)) {
                return false;
            }
            break;
        }
        // The next segment at another rate ends this stretch
        std::optional<size_t> boundary;
        for (size_t i = 0; i < view.anchor.segments.size(); ++i) {
            const sched::Segment& segment = view.anchor.segments[i];
            if (segment.position >= instance.fed && segment.rate != instance.run_rate) {
                boundary = i;
                break;
            }
        }
        if (boundary && view.anchor.segments[*boundary].position == instance.fed) {
            if (!this->cross(instance, view, *boundary)) {
                return false;
            }
            continue;
        }
        uint64_t limit = view.head;
        if (view.drain_end) {
            limit = std::min(limit, *view.drain_end);
        }
        if (boundary) {
            limit = std::min(limit, view.anchor.segments[*boundary].position);
        }
        // A block reads one staged segment
        for (const StagedSegment& staged : view.segments) {
            if (staged.position > instance.fed) {
                limit = std::min(limit, staged.position);
                break;
            }
        }
        if (instance.fed >= limit) {
            return false;
        }
        const uint64_t block = sched::ceil_div(instance.run_rate, sched::CELLS_PER_SECOND);
        if (!this->feed(instance, view,
                        static_cast<size_t>(std::min(limit - instance.fed, block)))) {
            return false;
        }
    }
    return true;
}

bool GroupAudio::feed(Instance& instance, const JobView& view, size_t frames) {
    if (!this->view_->read(this->epoch_, instance.fed, frames, this->input_)) {
        return false;
    }
    const InputFormat* format = &view.segments.front().format;
    for (const StagedSegment& staged : view.segments) {
        if (staged.position <= instance.fed) {
            format = &staged.format;
        }
    }
    instance.fed += frames;
    const uint32_t in_channels = format->channels;
    const uint32_t out_channels = instance.format.channels;
    const bool resampled = instance.run_rate != instance.format.sample_rate;
    if (!resampled && in_channels == out_channels) {
        // The host's samples, quantized as they are
        this->append(instance, this->input_, format->sample_format, frames);
        return true;
    }
    // A 16-bit resampler takes the host's 16-bit samples as they are
    const bool as_staged = resampled && in_channels == out_channels &&
                           Resampler::SAMPLE_FORMAT == SampleFormat::S16 &&
                           format->sample_format == SampleFormat::S16;
    std::span<const uint8_t> stage = this->input_;
    if (!as_staged) {
        instance.floats.resize(frames * in_channels);
        to_float(this->input_, format->sample_format, instance.floats);
        std::span<const float> floats = instance.floats;
        if (in_channels != out_channels) {
            instance.mixed.resize(frames * out_channels);
            mix_channels(instance.floats, in_channels, instance.mixed, out_channels);
            floats = instance.mixed;
        }
        if (!resampled) {
            this->append(instance, float_bytes(floats), SampleFormat::F32, frames);
            return true;
        }
        stage = float_bytes(floats);
        if (Resampler::SAMPLE_FORMAT != SampleFormat::F32) {
            instance.staged.resize(floats.size() * sample_bytes(Resampler::SAMPLE_FORMAT));
            convert_pcm(stage, SampleFormat::F32,
                        static_cast<uint32_t>(sample_bytes(Resampler::SAMPLE_FORMAT) * 8),
                        instance.staged);
            stage = instance.staged;
        }
    }
    if (instance.prime && !instance.resampler->prime(stage)) {
        this->fail(instance);
        return false;
    }
    instance.prime = false;
    instance.resampled.clear();
    if (!instance.resampler->process(stage, instance.resampled)) {
        this->fail(instance);
        return false;
    }
    this->append_resampled(instance);
    return true;
}

void GroupAudio::append_resampled(Instance& instance) {
    const size_t frame_bytes = sample_bytes(Resampler::SAMPLE_FORMAT) * instance.format.channels;
    this->append(instance, instance.resampled, Resampler::SAMPLE_FORMAT,
                 instance.resampled.size() / frame_bytes);
}

bool GroupAudio::cross(Instance& instance, const JobView& view, size_t segment) {
    const uint32_t out_rate = instance.format.sample_rate;
    if (instance.run_rate != out_rate) {
        instance.resampled.clear();
        if (!instance.resampler->flush(instance.resampled)) {
            this->fail(instance);
            return false;
        }
        this->append_resampled(instance);
    }
    // The output index from the exact boundary time
    const int64_t boundary = sched::segment_output_index(view.anchor, segment, out_rate);
    if (!this->cut_to(instance, boundary)) {
        this->fail(instance);
        return false;
    }
    const sched::Segment& next = view.anchor.segments[segment];
    instance.run_rate = next.rate;
    instance.origin_position = next.position;
    instance.origin_output = boundary;
    instance.prime = true;
    instance.stage_ready = false;
    return true;
}

bool GroupAudio::finish_drain(Instance& instance, uint64_t drain_end) {
    const uint64_t out_rate = instance.format.sample_rate;
    const uint64_t in_rate = instance.run_rate;
    if (in_rate != out_rate) {
        instance.resampled.clear();
        if (!instance.resampler->flush(instance.resampled)) {
            this->fail(instance);
            return false;
        }
        this->append_resampled(instance);
    }
    // The output sample count from the exact drain position, round((P_end - p) * r_out / r_in) past
    // a position p the stretch maps exactly, so rounding never accumulates
    const uint64_t span = drain_end - instance.origin_position;
    const auto total = instance.origin_output +
                       static_cast<int64_t>(((span * 2 * out_rate) + in_rate) / (2 * in_rate));
    if (instance.produced > total) {
        this->cut_to(instance, total);
    }
    instance.end_output = std::min(instance.produced, total);
    instance.flushed = true;
    return true;
}

void GroupAudio::append(Instance& instance, std::span<const uint8_t> input, SampleFormat format,
                        size_t count) {
    if (count == 0) {
        return;
    }
    const size_t in_bytes = input.size() / count;
    // Cell next's first output sample and the lead: what comes before it is a warm-up's, or was
    // emitted
    const int64_t kept_from = frame_start(instance, instance.next) + instance.lead;
    const auto dropped = static_cast<size_t>(
        std::clamp<int64_t>(kept_from - instance.produced, 0, static_cast<int64_t>(count)));
    instance.produced += static_cast<int64_t>(count);
    convert_pcm(input.subspan((count - 1) * in_bytes, in_bytes), format, instance.depth,
                instance.last_sample);
    if (dropped == count) {
        return;
    }
    const size_t at = instance.pending.size();
    instance.pending.resize(at + ((count - dropped) * instance.sample_bytes));
    convert_pcm(input.subspan(dropped * in_bytes), format, instance.depth,
                std::span(instance.pending).subspan(at));
}

bool GroupAudio::cut_to(Instance& instance, int64_t total) {
    if (instance.produced > total) {
        const auto excess = static_cast<size_t>(instance.produced - total);
        const size_t held = instance.pending.size() / instance.sample_bytes;
        instance.pending.resize((held - std::min(held, excess)) * instance.sample_bytes);
        instance.produced = total;
        return true;
    }
    // The stages give at least a stretch's rounded count, and the boundary's index rounds at most
    // one above it: more would be a stage that lost output
    if (total - instance.produced > MAX_BOUNDARY_EXTENSION) {
        return false;
    }
    const int64_t kept_from = frame_start(instance, instance.next) + instance.lead;
    while (instance.produced < total) {
        if (instance.produced >= kept_from) {
            instance.pending.insert(instance.pending.end(), instance.last_sample.begin(),
                                    instance.last_sample.end());
        }
        ++instance.produced;
    }
    return true;
}

bool GroupAudio::emit(Instance& instance, const JobView& view, size_t samples) {
    sched::Frame frame;
    frame.epoch = this->epoch_;
    frame.index = instance.next;
    frame.ts_us = sched::frame_timestamp(view.anchor, instance.next);
    // An Opus packet decodes to a whole frame (spec player/v1.md "Codec framing"), a drained
    // epoch's last one padded with silence
    frame.samples = static_cast<uint32_t>(
        instance.format.codec == sched::Codec::OPUS ? instance.frame_samples : samples);
    frame.duration_us = sched::frame_duration_us(frame.samples, instance.format.sample_rate);
    // With a lead, a drained epoch's last frames take what pending still holds
    const size_t bytes = std::min(samples * instance.sample_bytes, instance.pending.size());
    if (!instance.encoder->encode_quantized(std::span(instance.pending.data(), bytes),
                                            frame.payload)) {
        return false;
    }
    this->fifo_.push(FrameReady{instance.id, std::move(frame)});
    instance.pending.erase(instance.pending.begin(),
                           instance.pending.begin() + static_cast<std::ptrdiff_t>(bytes));
    instance.last = instance.next;
    instance.next += instance.cells;
    return true;
}

// ==============================================================================
// Visualizer analyses
// ==============================================================================

void GroupAudio::follow_analyses(const JobView& view) {
    const std::vector<AnalysisRequest>& requests = this->requests_.analyses;
    std::erase_if(this->analyses_, [&](const Analysis& analysis) {
        return std::none_of(requests.begin(), requests.end(), [&](const AnalysisRequest& request) {
            return request.instance == analysis.id;
        });
    });
    for (const AnalysisRequest& request : requests) {
        const bool exists =
            std::any_of(this->analyses_.begin(), this->analyses_.end(),
                        [&](const Analysis& analysis) { return analysis.id == request.instance; });
        if (exists) {
            continue;
        }
        if (this->analysis_mono_.empty()) {
            this->analysis_floats_.resize(ANALYSIS_READ_SAMPLES);
            this->analysis_mono_.resize(ANALYSIS_READ_SAMPLES);
        }
        Analysis analysis;
        analysis.id = request.instance;
        analysis.analysis = std::make_unique<VisualizerAnalysis>(request.config);
        const uint32_t bins = request.config.spectrum ? request.config.spectrum->n_disp_bins : 0;
        // At most one message of each type per cell (MAX_VISUALIZER_RATE)
        for (uint8_t type = visualizer_type::LOUDNESS; type <= visualizer_type::PEAK; type <<= 1) {
            if ((request.config.types & type) != 0) {
                analysis.cell_bytes += visualizer_message_bytes(type, bins);
            }
        }
        // from_index applies in its own epoch once anchored
        if (this->started_) {
            analysis.start = this->start_cell(
                view, request.epoch == this->epoch_ ? request.from_index : int64_t{0}, 1);
        }
        analysis.next = analysis.start;
        this->analyses_.push_back(std::move(analysis));
    }
}

bool GroupAudio::analyze(Analysis& analysis, const JobView& view, int64_t now_us,
                         std::optional<int64_t> held) {
    if (analysis.failed) {
        return false;
    }
    if (view.drain_end && analysis_finished(analysis, view)) {
        return this->post_analysis_final(analysis);
    }
    const sched::Anchor& anchor = view.anchor;
    // A cell that ends by now has every instant, shifted as it is, at or before now, so every
    // cursor skips its messages: an analysis that fell behind with its input staged restarts at the
    // cell playing now, so it never holds the staging ring behind the players. Input not staged yet
    // is a stall, which the rebase moves past instead; so is a restart at or past `k*` while its
    // rebase is not decided, which would move `k*` past the instances' frames the rebase is for.
    const int64_t next = analysis.begun ? analysis.next : analysis.start;
    const int64_t playing = sched::last_begun_cell(anchor, now_us);
    if (next < playing && sched::cell_position(anchor, next + 1) <= view.head) {
        const int64_t restart = this->start_cell(view, playing, 1);
        if (!held || restart < *held) {
            analysis.begun = false;
            analysis.start = restart;
            analysis.next = restart;
        }
    }
    if (!analysis.begun) {
        // From the window before the start, within the start's segment
        const uint64_t first = sched::cell_position(anchor, analysis.start);
        uint64_t segment_start = anchor.segments.front().position;
        for (const sched::Segment& segment : anchor.segments) {
            if (segment.position <= first) {
                segment_start = segment.position;
            }
        }
        const uint32_t rate = segment_rate(anchor, first);
        if (analysis.analysis->rate() != rate) {
            // Building the FFT and the tables allocates: at most one build per job
            if (this->built_this_job_) {
                this->waiting_ = true;
                return false;
            }
            this->built_this_job_ = true;
            if (!analysis.analysis->set_rate(rate)) {
                this->fail(analysis);
                return true;
            }
        }
        analysis.analysis->reset();
        const uint64_t window = analysis.analysis->window();
        analysis.fed = std::max({first - std::min(first, window), segment_start, view.retain_from});
        analysis.begun = true;
    }
    const int64_t cell = analysis.next;
    uint64_t end = sched::cell_position(anchor, cell + 1);
    if (view.drain_end) {
        end = std::min(end, *view.drain_end);
    }
    if (end > view.head) {
        return false;
    }
    // The onset check at the cell's start, and the periodic instant in the cell, which may be the
    // same instant
    const int64_t cell_start = anchor.t0_us + (cell * sched::CELL_US);
    std::optional<int64_t> periodic;
    if (const std::optional<int64_t> offset =
            periodic_instant_us(cell, analysis.analysis->config().rate)) {
        periodic = anchor.t0_us + *offset;
    }
    const int64_t shift = sched::shift_at(anchor, cell);
    sched::Frame frame;
    frame.epoch = this->epoch_;
    frame.index = cell;
    frame.ts_us = cell_start + shift;
    frame.payload.reserve(analysis.cell_bytes);
    const auto instant = [&](int64_t at, bool is_periodic, bool onset) {
        // A drained epoch's last cell has no input past the drain position to analyze
        const uint64_t position = sched::time_position(anchor, at);
        if (position > end) {
            return true;
        }
        if (!this->feed_analysis(analysis, view, position)) {
            return false;
        }
        const size_t before = frame.payload.size();
        analysis.analysis->analyze(at + shift, is_periodic, onset, frame.payload);
        if (frame.payload.size() != before) {
            frame.ts_us = at + shift;
        }
        return true;
    };
    const bool merged = periodic && *periodic == cell_start;
    if (!instant(cell_start, merged, true) ||
        (periodic && !merged && !instant(*periodic, true, false)) ||
        !this->feed_analysis(analysis, view, end)) {
        // Input below the head is staged, so only a failure, or a read the view refused, stops it
        // here: the cell starts again with no history
        analysis.begun = false;
        analysis.start = cell;
        return analysis.failed;
    }
    this->fifo_.push(FrameReady{analysis.id, std::move(frame)});
    analysis.last = cell;
    analysis.next = cell + 1;
    return true;
}

bool GroupAudio::feed_analysis(Analysis& analysis, const JobView& view, uint64_t position) {
    const sched::Anchor& anchor = view.anchor;
    while (analysis.fed < position) {
        // The anchored segment holding fed, and where the next starts
        size_t segment = 0;
        while (segment + 1 < anchor.segments.size() &&
               anchor.segments[segment + 1].position <= analysis.fed) {
            ++segment;
        }
        // A read stays within one staged segment
        const InputFormat* format = &view.segments.front().format;
        uint64_t staged_end = view.head;
        for (const StagedSegment& staged : view.segments) {
            if (staged.position <= analysis.fed) {
                format = &staged.format;
            } else {
                staged_end = staged.position;
                break;
            }
        }
        const uint32_t channels = format->channels;
        uint64_t limit = std::min(
            {position, view.head, staged_end, analysis.fed + (ANALYSIS_READ_SAMPLES / channels)});
        if (segment + 1 < anchor.segments.size()) {
            limit = std::min(limit, anchor.segments[segment + 1].position);
        }
        // A segment of another rate starts the window afresh, its tables built under the one build
        // a job allows
        const uint32_t rate = anchor.segments[segment].rate;
        if (rate != analysis.analysis->rate()) {
            if (this->built_this_job_) {
                this->waiting_ = true;
                return false;
            }
            this->built_this_job_ = true;
            if (!analysis.analysis->set_rate(rate)) {
                this->fail(analysis);
                return false;
            }
        }
        if (analysis.fed >= limit) {
            return false;
        }
        const auto frames = static_cast<size_t>(limit - analysis.fed);
        if (!this->view_->read(this->epoch_, analysis.fed, frames, this->input_)) {
            return false;
        }
        const std::span<float> floats(this->analysis_floats_.data(), frames * channels);
        to_float(this->input_, format->sample_format, floats);
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0F;
            for (uint32_t channel = 0; channel < channels; ++channel) {
                sum += floats[(i * channels) + channel];
            }
            this->analysis_mono_[i] = sum / static_cast<float>(channels);
        }
        analysis.analysis->push(std::span(this->analysis_mono_.data(), frames));
        analysis.fed = limit;
    }
    return true;
}

bool GroupAudio::analysis_finished(const Analysis& analysis, const JobView& view) {
    if (analysis.failed) {
        return true;
    }
    const int64_t next = analysis.begun ? analysis.next : analysis.start;
    return view.drain_end && sched::cell_position(view.anchor, next) >= *view.drain_end;
}

bool GroupAudio::post_analysis_final(Analysis& analysis) {
    if (analysis.final_posted || !this->drain_posted_) {
        return false;
    }
    this->fifo_.push(InstanceFinal{this->epoch_, analysis.id, analysis.last});
    analysis.final_posted = true;
    return true;
}

void GroupAudio::fail(Analysis& analysis) {
    if (analysis.failed) {
        return;
    }
    analysis.failed = true;
    this->fifo_.push(PipelineFailed{this->epoch_, analysis.id});
}

int64_t GroupAudio::frame_start(const Instance& instance, int64_t index) {
    return (index / instance.cells) * instance.frame_samples;
}

uint64_t GroupAudio::warmup_samples(const Instance& instance, uint32_t rate) const {
    const auto cell = static_cast<uint64_t>(sched::ceil_div(rate, sched::CELLS_PER_SECOND));
    // A reach not known yet is taken as within one cell
    const uint64_t reach =
        instance.resampler && instance.resampler_rate == rate ? instance.resampler->reach() : cell;
    return static_cast<uint64_t>(
               sched::ceil_div(static_cast<int64_t>(reach), static_cast<int64_t>(cell))) *
           cell;
}

uint64_t GroupAudio::history_samples(const Instance& instance, uint32_t rate) const {
    // begin() rounds the warm-up's first position down to a zero-phase step, from a start it
    // floors from rounded output counts, which can fall a sample below the cell_position() the
    // callers count from: HISTORY_SLACK covers that
    const uint32_t step_in = rate / std::gcd(rate, instance.format.sample_rate);
    return this->warmup_samples(instance, rate) + step_in - 1 + HISTORY_SLACK;
}

std::optional<int64_t> GroupAudio::last_cell(const JobView& view) const {
    if (!view.drain_end || *view.drain_end <= view.p0) {
        return std::nullopt;
    }
    return sched::cell_at(view.anchor, sched::unshifted_time(view.anchor, *view.drain_end - 1));
}

}  // namespace sendspin::server
