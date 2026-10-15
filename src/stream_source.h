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

/// @file stream_source.h
/// @brief StreamSource, the group a session's player and visualizer streams follow

#pragma once

#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/timeline.h"
#include "sched/types.h"
#include "sendspin/server/types.h"

#include <cstdint>
#include <optional>
#include <span>

namespace sendspin::server {

/// @brief The group a session's player and visualizer streams follow, as their markers and cursors
/// read it
class StreamSource {
public:
    /// @brief The group's playback as core knows it
    struct Playback {
        /// The group has a playing run
        bool playing{false};
        sched::RunId run;
        sched::EpochId epoch;
        /// The current epoch's anchor; null until a write anchors it
        const sched::Anchor* anchor{nullptr};
        /// The current epoch is draining
        bool draining{false};
        /// While draining: the cell holding the last input sample; nullopt with no input
        std::optional<int64_t> last_cell;
        /// The group input's format, while it has one
        std::optional<InputFormat> input;
        /// The group input's kind; BUFFERED with no input
        InputKind kind{InputKind::BUFFERED};
        /// Formats whose pipeline failed, which no stream uses until the run ends
        std::span<const sched::AudioFormat> failed;
        /// Opus may be streamed (ServerConfig::enable_opus)
        bool opus{false};
        /// The config_key() of visualizer configurations whose analysis failed, which no stream
        /// uses until the run ends
        std::span<const uint64_t> failed_visualizers;
        /// `G`, the group lead over its eligible players; `G_default` with none
        int64_t group_lead_us{sched::timing_defaults::GROUP_DEFAULT_US};
    };

    virtual ~StreamSource() = default;

    virtual Playback playback() const = 0;

    /// @brief The frame ring of the pipeline instance producing format, if the group has one
    virtual const sched::FrameRing* ring_for(const sched::AudioFormat& format) const = 0;

    /// @brief The ring of the visualizer analysis for the configuration with config_key() key, if
    /// the group has one: a frame per cell, its payload the cell's messages
    virtual const sched::FrameRing* visualizer_ring(uint64_t key) const = 0;

    /// @brief A stream's sent state or the format it switches to changed: a stream/start,
    /// stream/clear or stream/end was taken for writing, or a switch in place was planned or
    /// dropped. Must not call back into the session.
    virtual void on_stream_changed() = 0;
};

}  // namespace sendspin::server
