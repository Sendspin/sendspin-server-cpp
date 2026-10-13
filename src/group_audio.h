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

/// @file group_audio.h
/// @brief A group's pipelines and their interfaces with core: the group-to-core FIFO of epoch
/// events and frames, the pipeline-set slot, and GroupAudio, which turns the host view's staged PCM
/// into frames. GroupAudio runs on a group audio thread and reaches core only through the FIFO, the
/// slot and the host view.

#pragma once

#include "frame_encoder.h"
#include "host_view.h"
#include "memory.h"
#include "pipeline_stages.h"
#include "ring.h"
#include "sched/frames.h"
#include "sched/timeline.h"
#include "sched/types.h"
#include "transport.h"
#include "visualizer.h"
#include "visualizer_analysis.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace sendspin::server {

// ==============================================================================
// Group-to-core events
// ==============================================================================

/// @brief A flush began a new epoch in the same run; no write has anchored it
struct EpochCut {
    sched::EpochId epoch;
};

/// @brief The epoch's timeline moved forward by delta_us from index on: a stall rebase
struct AnchorShift {
    sched::EpochId epoch;
    int64_t index{0};
    int64_t delta_us{0};
};

/// @brief The current epoch is draining, and every instance has produced its last frame, so no
/// rebase follows
struct DrainStarted {
    sched::EpochId epoch;
    /// The cell holding the last input sample; nullopt when the epoch has no input
    std::optional<int64_t> last_cell;
};

/// @brief An epoch was anchored: a run started or resumed, or a flush's first write
struct EpochStarted {
    sched::RunId run;
    sched::EpochId epoch;
    sched::Anchor anchor;
    /// The drain staged when the epoch was anchored, which a resume carries
    std::optional<DrainStarted> drain;
};

/// @brief The epoch's run ended: a pause, a stop, a completed drain or the input going away
struct EpochEnded {
    sched::EpochId epoch;
};

struct FrameReady {
    sched::InstanceId instance;
    sched::Frame frame;
};

/// @brief The instance's last frame in a drained epoch; -1 for an instance with none
struct InstanceFinal {
    sched::EpochId epoch;
    sched::InstanceId instance;
    int64_t final_index{-1};
};

/// @brief The instance's encoder or resampler refused, or could not be built: it produces nothing
/// more
struct PipelineFailed {
    sched::EpochId epoch;
    sched::InstanceId instance;
};

using GroupEvent = std::variant<EpochCut, EpochStarted, EpochEnded, AnchorShift, DrainStarted,
                                FrameReady, InstanceFinal, PipelineFailed>;

/// @brief The ordered group-to-core FIFO: its producers push from any thread, and one posted
/// handler at a time delivers the events on core, at most a budget per handler.
///
/// It needs no cap, since its producers bound it: a frame comes only from staged input, which
/// admission keeps within the group's lead and write-ahead past the time playing, and a job posts
/// at most a few epoch events, however many host calls preceded it.
class GroupToCoreFifo {
    struct State;

public:
    /// The most events one handler delivers before it posts itself again
    static constexpr size_t DRAIN_BUDGET = 64;
    /// The events the FIFO and a handler's batch have room for before they grow: a few jobs' frames
    /// and epoch events between two drains
    static constexpr size_t QUEUED_EVENTS = 16;

    /// @brief The pushing end, which a job holds on its thread and which may outlive the FIFO
    class Producer {
    public:
        /// @brief Any thread. Once the FIFO is destroyed, the event is dropped.
        void push(GroupEvent event) const;

    private:
        friend class GroupToCoreFifo;

        Producer(EventLoop& loop, std::shared_ptr<State> state);

        /// Outlives every producer: the group audio threads are joined before the loop goes
        EventLoop* loop_;
        std::shared_ptr<State> state_;
    };

    /// @param deliver called on core, in push order
    GroupToCoreFifo(EventLoop& loop, std::function<void(GroupEvent&)> deliver);
    /// @brief A posted handler still pending delivers nothing, and a later push is dropped. On
    /// core.
    ~GroupToCoreFifo();
    GroupToCoreFifo(const GroupToCoreFifo&) = delete;
    GroupToCoreFifo& operator=(const GroupToCoreFifo&) = delete;

    Producer producer() const;

private:
    struct State {
        std::mutex mutex;
        /// Its producers bound it (above); room for QUEUED_EVENTS at first
        Ring<GroupEvent> events{QUEUED_EVENTS, Ring<GroupEvent>::UNLIMITED,
                                MemoryLocation::PREFER_EXTERNAL};
        bool drain_posted{false};
        /// The FIFO is gone
        bool closed{false};
        /// Core only
        std::function<void(GroupEvent&)> deliver;
        /// Core only: the batch a handler delivers
        Ring<GroupEvent> taking{QUEUED_EVENTS, DRAIN_BUDGET, MemoryLocation::PREFER_EXTERNAL};
    };

    static void drain(EventLoop& loop, const std::shared_ptr<State>& state);

    EventLoop& loop_;
    std::shared_ptr<State> state_;
};

// ==============================================================================
// Pipeline-set slot
// ==============================================================================

/// @brief One pipeline instance core wants
struct PipelineRequest {
    sched::InstanceId instance;
    /// The epoch from_index belongs to
    sched::EpochId epoch;
    sched::AudioFormat format;
    /// The first cell a new instance produces, in epoch; one added mid-epoch catches up to it from
    /// the staging ring's history
    int64_t from_index{0};

    bool operator==(const PipelineRequest&) const = default;
};

/// @brief One visualizer analysis core wants: the messages of one visualizer stream configuration,
/// one cell at a time
struct AnalysisRequest {
    sched::InstanceId instance;
    /// The epoch from_index belongs to
    sched::EpochId epoch;
    VisualizerConfig config;
    /// The first cell a new analysis produces, in epoch
    int64_t from_index{0};

    bool operator==(const AnalysisRequest&) const = default;
};

/// @brief Every instance core wants: the frame instances and the visualizer analyses
struct PipelineSet {
    std::vector<PipelineRequest> frames;
    std::vector<AnalysisRequest> analyses;

    bool operator==(const PipelineSet&) const = default;
};

/// @brief Latest wins: core publishes the whole set, the pipelines read the latest. Shared by the
/// group and its pipelines, whose job may outlive the group.
class PipelineSetSlot {
public:
    void publish(std::vector<PipelineRequest> frames, std::vector<AnalysisRequest> analyses = {});

    /// @return the set, when it changed since the version seen, which it updates
    std::optional<PipelineSet> read_if_newer(uint64_t& seen) const;

private:
    mutable std::mutex mutex_;
    PipelineSet set_;
    uint64_t version_{0};
};

// ==============================================================================
// GroupAudio
// ==============================================================================

/// @brief One group's pipelines: one instance per output format, each the input through the channel
/// mix when the channel count differs, the resampler when the rate differs, the quantize stage and
/// its PCM, FLAC or Opus encoder. An instance runs each input segment through the stages that
/// segment needs, so its output format and its frames' cell grid hold across set_format(). Each
/// instance produces at most one frame, of its format's `m` cells, per job; frames, epoch events
/// and drain events go to the FIFO in order. Its visualizer analyses each turn one cell of the
/// input into that cell's visualizer messages per job, posted as a frame of the analysis. Its jobs
/// run on a group audio thread (AudioWorkers), one at a time.
class GroupAudio {
public:
    GroupAudio(std::shared_ptr<HostView> view, GroupToCoreFifo::Producer fifo,
               std::shared_ptr<const PipelineSetSlot> slot, ResamplerQuality quality);

    /// @brief One job: follows the view's epochs, then produces at most one frame per instance
    /// @return true when another job could produce more now
    bool run_job(int64_t now_us);

private:
    struct Instance {
        sched::InstanceId id;
        sched::AudioFormat format;
        /// `m`, and the samples per channel of a full frame
        uint32_t cells{1};
        uint32_t frame_samples{0};
        /// The quantize stage's depth (quantized_depth()), and one output sample of every channel
        /// at it
        uint32_t depth{0};
        size_t sample_bytes{0};
        /// The encoder's delay in output samples (encoder_delay_samples()): pending runs this far
        /// ahead of cell next's first sample, so a frame decodes to its own cells
        uint32_t lead{0};
        std::unique_ptr<FrameEncoder> encoder;
        /// A resampler built for input at resampler_rate; kept while a segment at the output's
        /// rate needs none
        std::unique_ptr<Resampler> resampler;
        uint32_t resampler_rate{0};
        /// Its encoder is built, and the resampler for the rate it starts at
        bool configured{false};
        /// It failed and produces nothing more
        bool failed{false};
        /// The first cell it produces in the current epoch, a multiple of cells
        int64_t start{0};
        /// The next cell to produce in the current epoch
        int64_t next{0};
        /// The last cell it produced in the current epoch; -1 for none
        int64_t last{-1};
        bool final_posted{false};
        /// Its stream in the current epoch has started
        bool begun{false};
        /// The input rate of the stretch it feeds, and a position of it with the output index it
        /// maps to exactly, from which the drain position's output count is derived
        uint32_t run_rate{0};
        uint64_t origin_position{0};
        int64_t origin_output{0};
        /// The resampler for run_rate is reset and ready
        bool stage_ready{false};
        /// The resampler takes its first block as its history
        bool prime{false};
        /// The next input position to feed
        uint64_t fed{0};
        /// The output index the next sample the stages give takes, counted from the epoch's
        /// cell 0; below the start cell's during a warm-up, whose output is dropped
        int64_t produced{0};
        /// It reached the drain position and its output is cut there, at end_output
        bool flushed{false};
        int64_t end_output{0};
        /// Output samples at depth from cell next's first, and lead, on
        std::vector<uint8_t> pending;
        /// The last output sample, which extends a segment's output to its exact count
        std::vector<uint8_t> last_sample;
        /// A block of input as floats, and after the channel mix
        std::vector<float> floats;
        std::vector<float> mixed;
        /// The block in the resampler's sample format, when it was converted to get there, and the
        /// resampler's output in that format
        std::vector<uint8_t> staged;
        std::vector<uint8_t> resampled;
    };

    /// One visualizer analysis: its messages for cell index k go to core as a frame at index k,
    /// whose payload holds them in order and whose timestamp is the last one's, or the cell's
    /// when it has none
    struct Analysis {
        sched::InstanceId id;
        std::unique_ptr<VisualizerAnalysis> analysis;
        /// The most bytes one cell's messages take
        size_t cell_bytes{0};
        bool failed{false};
        /// Its input in the current epoch has started
        bool begun{false};
        /// The first cell it analyzes in the current epoch, the next, and the last; -1 for none
        int64_t start{0};
        int64_t next{0};
        int64_t last{-1};
        bool final_posted{false};
        /// The next input position it pushes
        uint64_t fed{0};
    };

    /// What an instance's resampler can do now
    enum class Stage : uint8_t {
        READY,
        /// Another resampler was built in this job
        WAIT,
        FAILED,
    };

    /// Posts what moves the pipelines from their epoch to the view's
    void follow_epochs(const JobView& view);
    /// Creates and drops instances to match the slot
    void follow_pipeline_set(const JobView& view);
    /// Creates and drops analyses to match the slot's set
    void follow_analyses(const JobView& view);
    /// The stall rebase before frame `k*` is produced, buffered or live; shifts view's anchor too
    /// @return `k*` while its input, with the resampler's lookahead, is not staged, so the rebase
    /// is not decided yet and no instance produces it
    std::optional<int64_t> rebase(JobView& view, int64_t now_us);
    /// While a live input is paused: lets the staging ring drop all but the warm-up history
    /// before the head
    void release_live_pause(const JobView& view);
    /// Posts DrainStarted once every instance produced through the drain position
    /// @return whether it posted
    bool post_drain_started(const JobView& view);
    /// The cell an instance starts at in the current epoch: requested or later, with its input
    /// still staged and past any shift it would straddle
    int64_t start_cell(const JobView& view, int64_t requested, uint32_t cells) const;
    /// The input rate at the instance's start: the anchored segment's, or the head's before the
    /// epoch is anchored
    uint32_t start_rate(const Instance& instance, const JobView& view) const;
    /// Builds the encoders and the resamplers of the instances that wait for them, at most one
    /// resampler per job
    /// @return whether an instance still waits
    bool configure_one(const JobView& view);
    /// The instance's resampler for input at rate, reset: built when it has none for that rate
    Stage ensure_resampler(Instance& instance, uint32_t rate);
    /// The instance produces nothing more; core hears of it
    void fail(Instance& instance);
    /// The instance produced through the drain position
    bool finished(const Instance& instance) const;
    /// Posts the instance's InstanceFinal once DrainStarted is out
    /// @return whether it posted
    bool post_final(Instance& instance, const JobView& view);
    /// @return whether it produced, finished or failed something
    bool produce(Instance& instance, const JobView& view);
    /// Starts the instance's stream at its start cell: from the input's position there at the
    /// output's rate; through a resampler, from real history over the reach when the staging ring
    /// holds it in the segment, at a zero-phase step of it, else primed with the first block
    /// @return false while it waits for its resampler, or once it failed
    bool begin(Instance& instance, const JobView& view);
    /// Feeds the instance until it holds a frame or the drain position's output, crossing segment
    /// boundaries: frame k once the input covers its last sample and the reach
    /// @return false when it waits for input, or for a resampler another build holds back
    bool fill(Instance& instance, const JobView& view);
    /// Converts frames input samples from fed through the instance's stages into pending
    /// @return false when they are not staged
    bool feed(Instance& instance, const JobView& view, size_t frames);
    /// At a segment whose rate differs from the instance's stretch: flushes the resampler, cuts or
    /// extends the output to the boundary's exact count and starts the next stretch there
    bool cross(Instance& instance, const JobView& view, size_t segment);
    /// At the drain position: flushes the resampler and cuts the output there
    bool finish_drain(Instance& instance, uint64_t drain_end);
    /// The output index of cell index's first sample
    static int64_t frame_start(const Instance& instance, int64_t index);
    /// Appends count output samples, dropping those before cell next's first and the lead
    void append(Instance& instance, std::span<const uint8_t> input, SampleFormat format,
                size_t count);
    /// append() of the resampler's output in instance.resampled
    void append_resampled(Instance& instance);
    /// Ends the instance's output at output index total: drops what it gave past it, or repeats
    /// its last sample up to it
    /// @return false when that would repeat more than MAX_BOUNDARY_EXTENSION samples
    bool cut_to(Instance& instance, int64_t total);
    /// Encodes the instance's next frame from pending and posts it
    /// @param samples the frame's output samples: its frame length, or a drained epoch's last
    /// frame's
    /// @return false when the encoder refused
    bool emit(Instance& instance, const JobView& view, size_t samples);
    /// Analyzes the analysis's next cell once its input is staged: the onset check at the cell's
    /// start and the periodic instant in it, each over the window before it. An analysis whose next
    /// cell ended by now restarts at the cell playing now, unless that is at or past held, `k*`
    /// while its rebase is not decided; its FFT and tables are built under the job's one build.
    /// @return whether it produced, finished or failed something
    bool analyze(Analysis& analysis, const JobView& view, int64_t now_us,
                 std::optional<int64_t> held);
    /// Pushes the input up to position into the analysis, mixed to mono: from its start's
    /// segment, with no history before that segment, and afresh at a segment of another rate
    /// @return false while that input is not staged, while a rate change waits for the job's
    /// build, or once it failed
    bool feed_analysis(Analysis& analysis, const JobView& view, uint64_t position);
    /// The analysis has analyzed through the drain position
    static bool analysis_finished(const Analysis& analysis, const JobView& view);
    /// Posts the analysis's InstanceFinal once DrainStarted is out
    bool post_analysis_final(Analysis& analysis);
    /// The analysis produces nothing more; core hears of it
    void fail(Analysis& analysis);

    /// `h_p` in input samples at rate: one cell, or the cells the reach needs
    uint64_t warmup_samples(const Instance& instance, uint32_t rate) const;
    /// The input history at rate a warm-up reads behind its start: warmup_samples(), and up to a
    /// zero-phase step less one more, which the warm-up's start rounds down by
    uint64_t history_samples(const Instance& instance, uint32_t rate) const;
    /// The cell holding the last input sample of a staged drain; nullopt with no input
    std::optional<int64_t> last_cell(const JobView& view) const;

    std::shared_ptr<HostView> view_;
    GroupToCoreFifo::Producer fifo_;
    std::shared_ptr<const PipelineSetSlot> slot_;
    ResamplerQuality quality_;
    uint64_t slot_seen_{0};
    PipelineSet requests_;
    std::vector<Instance> instances_;
    std::vector<Analysis> analyses_;
    /// The epoch the pipelines follow
    sched::RunId run_;
    sched::EpochId epoch_;
    /// EpochStarted was posted for epoch_
    bool started_{false};
    /// Core was told the run plays (an EpochStarted, then any EpochCut) and not that it ended
    bool active_{false};
    /// DrainStarted was posted for epoch_, alone or within its EpochStarted
    bool drain_posted_{false};
    /// A resampler or an analysis's FFT was built in this job, and an instance or an analysis waits
    /// for another
    bool built_this_job_{false};
    bool waiting_{false};
    std::vector<uint8_t> input_;
    /// A block of input as floats, and mixed to mono, for the analyses
    std::vector<float> analysis_floats_;
    std::vector<float> analysis_mono_;
    std::vector<sched::InstanceNext> nexts_;
    std::vector<int64_t> frontiers_;
    /// Each job's view of the host view, its storage reused
    JobView job_;
};

}  // namespace sendspin::server
