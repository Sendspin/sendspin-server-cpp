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

/// @file host_view.h
/// @brief A group input's host view: the staged PCM, the run, the current epoch's anchor and the
/// group parameters, shared under one mutex by the host's calls, the group's pipelines and core.
/// Each GroupInput call is one step under the mutex, but for a live write through the slew, which
/// resamples between its steps (HostView::write()). The mutex is a leaf: what a step signals or
/// posts happens after it is released.

#pragma once

#include "audio_workers.h"
#include "dispatcher.h"
#include "live_slew.h"
#include "memory.h"
#include "profile.h"
#include "ring.h"
#include "sched/admission.h"
#include "sched/lowering.h"
#include "sched/run_view.h"
#include "sched/timeline.h"
#include "sendspin/server/server.h"
#include "transport.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief The staging ring holds this much input, so admission never passes it
constexpr int64_t STAGING_RING_US = profile::STAGING_RING_US;
/// @brief ...or this many bytes, whichever is less
constexpr size_t STAGING_RING_MAX_BYTES = profile::STAGING_RING_MAX_BYTES;

constexpr uint32_t MAX_INPUT_RATE = 384000;
constexpr uint8_t MAX_INPUT_CHANNELS = 32;

/// @brief The input segments the staging ring holds at once: a format change every 0.4 s across the
/// ring's input; set_format() is refused past it
constexpr size_t MAX_STAGED_SEGMENTS = 32;

/// @brief The shifts an epoch's anchor holds at once: one per cell at most, folded once the staging
/// ring no longer holds their cell, so about 20 s of cells between the position playing and the
/// head. A shift past it is dropped, and the next frame decides again.
constexpr size_t MAX_ANCHOR_SHIFTS = 1024;

/// @brief Whether a GroupInput takes this format (InputFormat)
bool is_valid_input(const InputFormat& format);

/// @brief The group parameters core writes into the view
struct GroupParams {
    /// `G_next`: the group lead the next anchor uses
    int64_t group_lead_us{0};
    /// `od_max`: the largest output delay over the eligible members
    int64_t output_delay_max_us{0};
    /// `W`: the write-ahead
    int64_t write_ahead_us{0};

    bool operator==(const GroupParams&) const = default;
};

/// @brief What a live input takes beyond its InputOptions
struct LiveOptions {
    /// The least lead its anchor and its lowering keep, whatever `G_next` is
    /// (SourceRouteOptions::min_latency_us)
    int64_t min_lead_us{0};
    /// The quality of the ASRC its slew runs through (LiveSlew)
    ResamplerQuality quality{ResamplerQuality::MEDIUM};
};

/// @brief A stretch of staged input in one format, from position on
struct StagedSegment {
    uint64_t position{0};
    InputFormat format;
    /// The staging ring's byte of position, counted from the byte of position 0, so its slot is
    /// this modulo the ring's size
    uint64_t byte{0};
};

/// @brief What the group's pipelines read of the view in one step
struct JobView {
    sched::RunView run;
    /// The current epoch's anchor; meaningful while run.run is PLAYING
    sched::Anchor anchor;
    /// The current epoch's first position
    uint64_t p0{0};
    /// The next position to be written
    uint64_t head{0};
    /// The lowest position the staging ring holds
    uint64_t retain_from{0};
    /// `P_end`, the drain position, while a drain is staged
    std::optional<uint64_t> drain_end;
    /// The staged input's segments, in position order, from the one holding retain_from; the last
    /// holds the head
    std::vector<StagedSegment> segments;
    /// The host released its GroupInput, or core closed the input
    bool gone{false};
    /// `G_eff` of the anchored epoch and `od_max`, for the stall rebase
    int64_t group_lead_eff_us{0};
    int64_t output_delay_max_us{0};
    InputKind kind{InputKind::BUFFERED};
    /// `J_in`, a live input's write jitter
    int64_t write_jitter_us{0};
};

class HostView : public std::enable_shared_from_this<HostView> {
public:
    /// @param options a valid format, and for a live input a write jitter in range
    /// @param dispatcher where on_drained() and on_writable() are posted, in the order the view
    /// decided them; a task posted after the dispatcher finished is dropped. Construct with
    /// std::make_shared: a view posts a reference to itself.
    /// @param listener nullptr for none
    /// @param live ignored for a buffered input
    HostView(GroupId group, InputId id, const InputOptions& options, const GroupParams& initial,
             std::shared_ptr<Dispatcher> dispatcher, ServerListener* listener,
             const LiveOptions& live = {});
    HostView(const HostView&) = delete;
    HostView& operator=(const HostView&) = delete;

    GroupId group() const {
        return this->group_;
    }

    InputId id() const {
        return this->id_;
    }

    InputKind kind() const {
        return this->kind_;
    }

    /// @brief `J_in`: a live input's write jitter; 0 for a buffered input
    int64_t write_jitter_us() const {
        return this->write_jitter_us_;
    }

    /// @brief The input's format, and how many set_format() calls changed it
    struct FormatState {
        InputFormat format;
        uint64_t generation{0};
    };

    FormatState format() const;

    // ------------------------------------------------------------------------------
    // Host calls (GroupInput)
    // ------------------------------------------------------------------------------

    /// @brief Admits up to `P_max` and anchors a run or pending epoch it begins. A playing live
    /// input owing latency plays its frames fast (sched::live_slew_ppm(), LiveSlew), with the
    /// mutex released between the steps that stage each block, and reports the positions they
    /// took; a write made meanwhile takes nothing. No allocation: the copy goes into the
    /// preallocated staging ring.
    WriteResult write(const void* pcm, size_t frames);
    InputState state() const;
    InputState wait_writable(int64_t timeout_us);
    void flush();
    DrainId drain();
    uint64_t pause();
    void resume();
    void stop();
    /// @brief Starts a new input segment at the head while a run plays or is paused; otherwise the
    /// next run's format, the staging ring sized for it
    /// @return false for an invalid format, while a drain is staged, once closed, or with
    /// MAX_STAGED_SEGMENTS segments staged
    bool set_format(const InputFormat& format);
    int64_t time_of(uint64_t position) const;
    int64_t latency_us() const;
    /// @brief The GroupInput is destroyed: stops the run, resolving a staged drain as STOPPED
    void release();

    // ------------------------------------------------------------------------------
    // Core
    // ------------------------------------------------------------------------------

    /// @brief Calls that change what the pipelines read signal wakeup (core) and wake audio (the
    /// group's pipelines) from now on, until close()
    void attach(const Wakeup::Handle& wakeup, std::shared_ptr<AudioWorkers::Task> audio);

    /// @brief The input closes for good (server stop, or refused by its group): a staged drain
    /// resolves as STOPPED, writes return CLOSED, and nothing is signalled from now on
    void close();

    /// @brief Writes the group parameters; a raised lead also raises the anchored epoch's `G_eff`
    void set_group_params(const GroupParams& params);

    /// @brief A server-initiated stop: resolves a staged drain as STOPPED and latches
    sched::ServerStopResult server_stop();

    /// @brief Delivers drain completion for epoch: the run ends and on_drained() reports PLAYED,
    /// unless a flush, a pause or a stop came first. Nothing the view decides reaches the host
    /// until release_drained(), so core ends the run on its side first.
    /// @return whether it was delivered
    bool complete_drain(sched::EpochId epoch);

    /// @brief Posts what complete_drain() held back
    void release_drained();

    /// @brief Posts on_writable() once a write() that came up short is answered by room; one
    /// already posted may reach the host after its GroupInput is released
    /// @return when to call again; sched::NEVER when nothing is waiting or only an event can
    /// make room
    int64_t poll_writable(int64_t now_us);

    /// @brief The group's pipelines shift epoch's timeline by delta_us from index on
    /// @return false when epoch is no longer the view's playing epoch, or the anchor holds
    /// MAX_ANCHOR_SHIFTS
    bool add_shift(sched::EpochId epoch, int64_t index, int64_t delta_us);

    /// @brief `k*` of epoch and the `L_p` its rebase decides with, for admission as if rebased;
    /// index nullopt when no instance produces frames, so admission takes the plain horizon
    void publish_shift_index(sched::EpochId epoch, std::optional<int64_t> index,
                             int64_t pipeline_latency_us);

    JobView job_view() const;

    /// @brief job_view() into view, reusing its storage
    void read_job_view(JobView& view) const;

    /// @brief The host released its GroupInput, or core closed the input. Reads no lock, so a
    /// caller holding its own lock may call it.
    bool gone() const;

    /// @brief Copies frames frames from position of epoch into out, in the format of the segment
    /// holding them
    /// @return false when the epoch is no longer the view's, or the range is not staged or spans
    /// two segments
    bool read(sched::EpochId epoch, uint64_t position, size_t frames,
              std::vector<uint8_t>& out) const;

    /// @brief Lets the staging ring drop positions below position of a playing epoch, or of a
    /// paused live input's, folding the anchor's shifts below the cell it then holds first
    void release_before(sched::EpochId epoch, uint64_t position);

private:
    /// What one step does once it has released the mutex
    struct AfterUnlock {
        Wakeup::Handle wakeup;
        std::shared_ptr<AudioWorkers::Task> audio;
        std::shared_ptr<HostView> outbox;
    };

    struct OutboxEntry {
        bool writable{false};
        DrainId drain;
        DrainOutcome outcome{DrainOutcome::PLAYED};
    };

    /// Signals the wakeup, wakes the pipelines and posts the outbox as the step decided
    void run_after(AfterUnlock& after) const;
    /// On the dispatch thread: calls the listener for every entry, in order
    void deliver_outbox();
    /// Queues on_drained() for each call a resolution answers, oldest first
    void queue_drained(const std::optional<sched::DrainResolution>& resolution, AfterUnlock& after);
    void queue_entry(const OutboxEntry& entry, AfterUnlock& after);
    void signal(AfterUnlock& after) const;
    int64_t time_of_locked(uint64_t position, int64_t now_us) const;
    InputState state_locked(int64_t now_us) const;
    /// A host call that changes the input's state ends every wait_writable() in progress
    void end_waits();
    /// When wait_writable() would return now, or ready earlier than ready_before_us (the
    /// readiness before a change of the pipelines'): wakes it. A waiter sleeps until its readiness
    /// on its own, so the pipelines' changes that leave it neither do not wake it each cell.
    void notify_if_sooner(int64_t ready_before_us, int64_t now_us);
    /// closed_ or released_ became true
    void mark_gone();
    sched::AdmissionInput admission_input() const;
    /// `G_next` as this input takes it: the group's, or the input's least lead where higher
    int64_t lead_next() const;
    /// What a write through the slew takes from its first step under the mutex to the rest
    struct SlewedWrite {
        /// The frames admitted, which bound the positions staged; nullopt when not slewed
        std::optional<uint64_t> frames;
        int32_t ppm{0};
        InputState state{InputState::OPEN};
        /// slew_generation_ as the write began
        uint64_t generation{0};
        /// The staged frames before the head copied into the slew's history, where it waits for
        /// start()
        std::optional<size_t> history;
    };
    /// Takes up a slew the format or epoch replaced, from set_format()'s or restarted
    void adopt_slew();
    /// Under the mutex: claims the slew for one write of frames at ppm, copying its history
    SlewedWrite begin_slewed(uint64_t frames, int32_t ppm, InputState state);
    /// Without the mutex: runs the write through the slew a block at a time, staging each block's
    /// output under the mutex unless a flush, stop, drain, close, epoch or format came since
    /// @return the positions staged, which the host is told were taken
    WriteResult write_slewed(const uint8_t* pcm, const SlewedWrite& slewed);
    /// `R_min`: one cell of input samples
    uint64_t ready_samples() const;
    /// Anchors the current epoch at p0_ with `T0 = now + G_next` (lead_next()), reusing the
    /// anchor's storage
    void anchor_at(int64_t now_us);
    /// Fills anchor with T0 at t0_us, from p0 through the staged segments
    void build_anchor(sched::Anchor& anchor, int64_t t0_us, uint64_t p0) const;
    /// The staged segment holding position: the last starting at or before it
    const StagedSegment& segment_at(uint64_t position) const;
    /// The ring byte of a staged position
    uint64_t byte_of(uint64_t position) const;
    /// Drops the segments that end at or below retain_from_, from the staging list and the anchor
    void prune_segments();
    /// Completes a drain staged on a pending epoch with no input
    void resolve_empty_drain(AfterUnlock& after);
    /// Drops every staged sample: positions restart at 0, in format_
    void restart_positions();

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    GroupId group_;
    InputId id_;
    InputKind kind_;
    int64_t write_jitter_us_;
    int64_t min_lead_us_;
    ResamplerQuality quality_;
    std::shared_ptr<Dispatcher> dispatcher_;
    ServerListener* listener_;

    /// The head's format, which the next write() takes
    InputFormat format_;
    uint64_t format_generation_{0};
    /// The staging ring's bytes, allocated uninitialized (no byte is read before it is written),
    /// in PSRAM where there is one
    struct RingStorage {
        PlacedArray<uint8_t> bytes;
        size_t size{0};

        static RingStorage allocate(size_t size) {
            return {make_placed_array<uint8_t>(size, MemoryLocation::PREFER_EXTERNAL), size};
        }
        std::span<uint8_t> span() const {
            return {this->bytes.get(), this->size};
        }
    };
    RingStorage ring_;
    /// In position order, from the one holding retain_from_; never empty
    std::vector<StagedSegment> segments_;
    /// The lowest position the ring holds
    uint64_t retain_from_{0};
    uint64_t head_{0};
    uint64_t p0_{0};
    uint64_t p_pause_{0};
    std::optional<uint64_t> drain_end_;
    sched::RunView run_;
    sched::Anchor anchor_;
    /// The anchor belongs to the current epoch, or while paused to the epoch the pause ended
    bool anchor_valid_{false};
    /// time_of()'s anchor while not playing, rebuilt on each call in reserved storage
    mutable sched::Anchor provisional_;
    GroupParams params_;
    /// `G_eff` of the anchored epoch
    int64_t group_lead_eff_us_{0};
    /// A live input's latency lowering in the current epoch
    sched::LiveLowering lowering_;
    /// The slew that plays it out. A write runs it with the mutex released while slewing_, and
    /// no other call touches it: one that replaces it bumps slew_generation_, and the next write
    /// takes set_format()'s next_slew_ or restarts it (adopt_slew()).
    LiveSlew slew_;
    bool slewing_{false};
    uint64_t slew_generation_{0};
    uint64_t slew_seen_{0};
    LiveSlew next_slew_;
    bool slew_pending_{false};
    /// `k*` the pipelines published for the current epoch, and the `L_p` its rebase decides with
    std::optional<int64_t> shift_index_;
    int64_t rebase_latency_us_{0};
    /// A write() came up short and on_writable() has not answered it
    bool writable_wanted_{false};
    bool closed_{false};
    bool released_{false};
    /// closed_ || released_, for gone() without the mutex
    std::atomic<bool> gone_{false};
    /// Counts the host calls that end a wait_writable()
    uint64_t host_calls_{0};
    /// host_calls_ as the last write() or wait_writable() saw it: a call since then ends a wait,
    /// even one made before the wait began
    uint64_t host_calls_seen_{0};
    /// The ids of the drain() calls not yet answered, oldest first: as many as the host makes, room
    /// for two at first
    Ring<DrainId> drain_ids_{2, Ring<DrainId>::UNLIMITED, MemoryLocation::PREFER_EXTERNAL};
    Wakeup::Handle wakeup_;
    std::shared_ptr<AudioWorkers::Task> audio_;
    /// Listener calls decided under the mutex, in order, and not yet delivered
    std::vector<OutboxEntry> outbox_;
    /// A delivery task is posted and will take the outbox
    bool outbox_posted_{false};
    /// complete_drain() holds the outbox until release_drained()
    bool outbox_held_{false};
};

/// @brief The input each group holds on the host side, so open_input() refuses a second one at
/// once (OpenInputResult::GROUP_BUSY). An input stops holding its group when it is gone().
class GroupInputs {
public:
    /// @return false when group holds an input that is not gone; nothing is claimed
    bool available(GroupId group);

    /// @return false when group holds an input that is not gone
    bool claim(GroupId group, const std::shared_ptr<HostView>& view);

private:
    std::mutex mutex_;
    std::map<GroupId, std::weak_ptr<HostView>> inputs_;
};

/// @brief The views open_input() made whose attachment has not run on core. A view whose attach
/// lands after the core loop ended is never closed by its group, so shutdown closes it here.
class InputRegistry {
public:
    /// @return false once close_all() ran: the run is stopping
    bool add(const std::shared_ptr<HostView>& view);

    /// @brief The view's attachment ran
    void remove(const HostView& view);

    /// @brief Closes every registered view and refuses later ones
    void close_all();

private:
    std::mutex mutex_;
    std::vector<std::shared_ptr<HostView>> views_;
    bool closed_{false};
};

}  // namespace sendspin::server
