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

/// @file group.h
/// @brief A group on core: its members, its input and pipelines, the frame rings, the timeline as
/// the group-to-core FIFO reports it, drain completion and the server-initiated stop, its
/// controller and metadata state, and its images and colors. Core thread only.

#pragma once

#include "artwork.h"
#include "artwork_cache.h"
#include "artwork_stream.h"
#include "audio_workers.h"
#include "colors.h"
#include "controller.h"
#include "dispatcher.h"
#include "group_audio.h"
#include "host_view.h"
#include "memory.h"
#include "metadata.h"
#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/timeline.h"
#include "sched/types.h"
#include "scheduled_updates.h"
#include "sendspin/server/server.h"
#include "session.h"
#include "transport.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace sendspin::server {

/// @brief A connection as its group reaches it
class GroupMember {
public:
    virtual ~GroupMember() = default;

    virtual Session& session() = 0;

    /// @brief Runs the connection's pump after a change to its session; may close the connection
    virtual void pump() = 0;

    virtual const ClientInfo& client_info() const = 0;
};

/// @brief What a group reports to the server around it
struct GroupHooks {
    /// The group's playback state or members changed
    std::function<void()> changed;
    /// An error to report to the host
    std::function<void(const ServerError&)> error;
    /// The input detached: the host released it or the server closed it
    std::function<void()> detached;
    /// A line for the server's log at INFO: the stall report (Group::report_stalls())
    std::function<void(std::string)> info;
};

/// In PSRAM where there is one
class Group : public ExternalNew {
public:
    /// @param next_instance the server's instance id counter
    /// @param workers the group audio threads its pipelines run on
    /// @param opus whether a player may be streamed Opus (ServerConfig::enable_opus)
    /// @param artwork the server's encoded images, which outlives the group
    Group(EventLoop& loop, AudioWorkers& workers, GroupId id, std::string name,
          uint64_t& next_instance, ResamplerQuality quality, bool opus, GroupHooks hooks,
          ArtworkCache& artwork);
    ~Group();
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;

    GroupId id() const {
        return this->id_;
    }

    GroupInfo info() const;

    /// @brief A new name, which every member gets in a group/update (messaging.md "group/update":
    /// on any change)
    void rename(const std::string& name);

    size_t member_count() const {
        return this->members_.size();
    }

    bool has_input() const {
        return this->input_ != nullptr;
    }

    /// @brief Joins member: its stream follows the group, it gets a group/update, and a marker with
    /// a join trigger
    void add_member(GroupMember& member);

    /// @brief member leaves; its session no longer follows the group
    void remove_member(GroupMember& member);

    /// @brief A member's session took in a client/state
    void on_client_state(GroupMember& member);

    /// @brief Attaches an input; false when the group holds one that is not gone
    bool attach_input(const std::shared_ptr<HostView>& view);

    /// @brief Closes the input for good
    void close_input();

    /// @brief Closes view, when it is the group's input, as a release would end it: a playing run
    /// ends, and the input detaches (a route replaced, SendspinServer::route_source())
    /// @return whether view was the group's input
    bool close_input_of(const HostView& view);

    /// @brief The stop messaging.md "Server behavior when a client becomes unavailable" asks for
    /// when a solo member turns unavailable
    void server_stop();

    bool playing() const {
        return this->playing_;
    }

    // ------------------------------------------------------------------------------
    // Controller and metadata (controller/v1.md, metadata/v1.md)
    // ------------------------------------------------------------------------------

    /// @brief The server/state controller object for the members' state and the host's caps
    ControllerState controller_state() const;

    void set_controller_caps(const ControllerCaps& caps);

    const ControllerCaps& controller_caps() const {
        return this->caps_;
    }

    /// @brief One volume command to each player that supports volume (controller/v1.md "Setting
    /// group volume")
    void apply_volume(uint32_t volume);

    /// @brief One mute command to each player that supports mute
    void apply_mute(bool muted);

    void set_metadata(const Metadata& metadata, int64_t at_us);
    void cancel_scheduled_metadata();

    /// @brief The current metadata, carried forward to now
    MetadataState metadata_now() const;

    /// @brief Sends member the state of its state roles: after an activation adds them, or on
    /// joining (messaging.md "server/state"; metadata/v1.md, color/v1.md: current, then a
    /// scheduled update)
    void send_role_state(GroupMember& member);

    // ------------------------------------------------------------------------------
    // Artwork and colors (artwork/v1.md, color/v1.md)
    // ------------------------------------------------------------------------------

    /// @brief Shows image, nullptr for none, on every member's channels for source from at_us
    /// (ArtworkTimeline::set()). An album image also sets the colors from its palette at at_us,
    /// once the worker derived it, unless set_colors() comes first; no image clears them.
    void set_artwork(ArtworkSource source, std::shared_ptr<const SourceImage> image, int64_t at_us);

    /// @brief Drops source's scheduled image; for the album, also the scheduled colors and a
    /// palette still to come for a scheduled image
    void cancel_scheduled_artwork(ArtworkSource source);

    /// @brief Sets the colors from at_us, with the contrast the spec requires (with_contrast()),
    /// as set_metadata() sets metadata
    void set_colors(const Colors& colors, int64_t at_us);
    void cancel_scheduled_colors();

    /// @brief The artwork worker delivered an encoding or a palette: the colors a palette
    /// awaited are set, and every member with the artwork role is pumped
    void on_artwork_ready();

private:
    class MemberSource;

    struct Member {
        GroupMember* member;
        std::unique_ptr<MemberSource> source;
    };

    /// One pipeline job on a group audio thread; it then tells core it ran, so core polls for the
    /// room it may have freed
    struct PipelineJob {
        std::shared_ptr<GroupAudio> audio;
        Wakeup::Handle done;

        bool operator()() const;
    };

    void deliver(GroupEvent& event);
    void on_epoch_started(EpochStarted& started);
    void on_epoch_cut(const EpochCut& cut);
    void on_drain_started(const DrainStarted& drain);
    void on_anchor_shift(const AnchorShift& shift);
    void on_frame(FrameReady& ready);
    /// Logs the rebases and late frames counted since the last report, at most every
    /// STALL_REPORT_US, arming stall_timer_ for counts it holds back
    void report_stalls(int64_t now);
    void on_instance_final(const InstanceFinal& final);
    /// Excludes the format until the run ends, ends the streams on it and reports each player that
    /// streamed or wanted it
    void on_pipeline_failed(const PipelineFailed& failed);
    /// A visualizer analysis failed: its configuration is excluded until the run ends, its streams
    /// end and each client that streamed or wanted it is reported
    void on_analysis_failed(const PipelineFailed& failed);
    /// Excludes the configurations until the run ends and reports each client that streamed or
    /// wanted one; a marker taken after the next recompute ends the streams on them
    void fail_visualizers(std::span<const VisualizerConfig> configs);
    /// Core's side of a run's end: pause, stop, drain completion
    void end_run(sched::EpochId epoch);

    /// The group parameters and the pipeline set from the members' state
    void recompute();
    /// The analyses the members' visualizer streams read or want, keeping each one's id while its
    /// configuration is wanted, at most MAX_GROUP_ANALYSES: those running first, then the others in
    /// member order, the rest into overflow
    std::vector<AnalysisRequest> analyses(std::vector<VisualizerConfig>& overflow);
    /// `L_p` of the pipeline producing format from the input; 0 for a buffered input
    int64_t pipeline_latency(const sched::AudioFormat& format, uint32_t input_rate);
    /// The cell an instance with frames of cells added now to the playing epoch on anchor starts
    /// at: the first frame a stream starting now or a switch can read
    int64_t start_index(const sched::Anchor& anchor, uint32_t cells, int64_t now) const;
    /// Sends the controller state, when it changed, to every member with the role but except
    /// (controller/v1.md: recomputed on membership and support changes)
    void refresh_controller(GroupMember* except);
    /// Sends each state to every member with the metadata role and arms the timer for the next
    /// step
    void send_metadata(const MetadataSends& sends);
    void on_metadata_timer();
    /// Sends each state to every member with the color role and arms the timer for the next step
    void send_colors(const std::vector<ColorState>& states);
    void on_color_timer();
    /// Sets the colors from the palette awaited, once the cache has it
    void resolve_palette();
    ArtworkTimeline& timeline(ArtworkSource source);
    void pump_artwork_members();
    /// Queues json with every member select accepts, then pumps them
    template <typename Select>
    void queue_to_members(const std::string& json, Select select);
    /// Queues a marker with every member and pumps them
    void queue_markers(std::optional<sched::TriggerKind> kind);
    void pump_members(bool active_only);
    /// messaging.md "group/update": every field
    std::optional<std::string> group_update() const;
    void send_group_update(GroupMember& member);
    /// After a member's stream changed: recomputes and checks drain completion from a later
    /// handler, outside the session call that reported it
    void schedule_check();
    void check_drain();
    void arm_drain_timer();
    /// After a host call that changed the view, and after each pipeline job: follows the input's
    /// format, detaches an input the host released, and polls for room
    void on_wake();
    void poll_writable();
    void detach_input();
    /// A released input whose detach has not run: ends its run, whose end the stopped pipelines
    /// may never post, then detaches it
    void drop_released_input();
    /// A posted handler's guard: false once the group is gone
    std::function<void()> guarded(std::function<void()> fn);

    EventLoop& loop_;
    AudioWorkers& workers_;
    GroupId id_;
    std::string name_;
    uint64_t& next_instance_;
    ResamplerQuality quality_;
    bool opus_;
    GroupHooks hooks_;
    sched::SchedTiming timing_;
    std::vector<Member> members_;
    /// pump_members()'s list, kept so a frame's pump allocates nothing once it has grown
    std::vector<GroupMember*> pumped_;

    std::shared_ptr<HostView> input_;
    std::optional<InputFormat> input_format_;
    /// The input's kind and `J_in`; BUFFERED with no input
    InputKind input_kind_{InputKind::BUFFERED};
    int64_t write_jitter_us_{0};
    /// `G` over the eligible players as last written to the input
    int64_t group_lead_us_{sched::timing_defaults::GROUP_DEFAULT_US};
    /// `N_p` of the resampler for each input and output rate pair, once asked for: at most one
    /// entry per rate the input took and rate a member streams at, cleared with the input
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> filter_lengths_;
    uint64_t input_generation_{0};
    std::unique_ptr<Wakeup> wakeup_;
    /// The input's pipelines (GroupAudio) on the group audio threads, while it has an input
    std::shared_ptr<AudioWorkers::Task> audio_;
    /// The attachment's group-to-core FIFO: a new one per input, so a stopped job's events never
    /// reach the group once its input is detached
    std::unique_ptr<GroupToCoreFifo> fifo_;
    std::shared_ptr<PipelineSetSlot> slot_;
    PipelineSet pipelines_;
    std::map<sched::InstanceId, sched::FrameRing> rings_;
    bool drain_check_posted_{false};
    /// Since the last report: the rebases the pipelines posted (AnchorShift: the frames about to
    /// be produced had too little lead, whether the pipelines fell behind, the input came late
    /// or a joiner raised the group's lead) and the player frames that reached their ring at or
    /// past their play time, counted per instance, so a cell late in two formats counts twice
    struct Stalls {
        static constexpr int64_t STALL_REPORT_US = 10'000'000;
        uint32_t rebases{0};
        int64_t shifted_us{0};
        uint32_t late_frames{0};
        int64_t reported_us{0};
    };
    Stalls stalls_;
    /// Fires once a report held back by STALL_REPORT_US is due
    Timer stall_timer_;
    Timer writable_timer_;
    Timer drain_timer_;
    Timer metadata_timer_;
    Timer color_timer_;

    ControllerCaps caps_;
    /// The controller state last sent to the members
    std::optional<ControllerState> controller_;
    MetadataTimeline metadata_;

    ArtworkCache& artwork_cache_;
    ArtworkTimeline album_;
    ArtworkTimeline artist_;
    ScheduledUpdates<ColorState> colors_;
    /// An album image whose palette sets the colors at at_us once the worker derived it
    struct PaletteWait {
        std::shared_ptr<const SourceImage> image;
        int64_t at_us{0};
    };
    std::optional<PaletteWait> palette_wait_;

    // The timeline as the FIFO reported it
    bool playing_{false};
    sched::RunId run_;
    sched::EpochId epoch_;
    std::optional<sched::Anchor> anchor_;
    bool draining_{false};
    std::optional<int64_t> last_cell_;
    /// The last epoch whose run ended; FIFO events of an epoch up to it are late
    sched::EpochId ended_epoch_;
    std::set<const GroupMember*> unplayable_reported_;
    /// Formats whose pipeline failed, and the config_key() of visualizer configurations whose
    /// analysis failed, excluded until the run ends
    std::vector<sched::AudioFormat> failed_formats_;
    std::vector<uint64_t> failed_visualizers_;
    /// Members in unplayable_reported_ because a failure was reported for them, which the run's
    /// end takes out again
    std::set<const GroupMember*> failure_reported_;
    /// Expires with the group, for posted handlers
    std::shared_ptr<bool> alive_;
};

}  // namespace sendspin::server
