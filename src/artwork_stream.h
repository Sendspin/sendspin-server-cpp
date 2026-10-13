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

/// @file artwork_stream.h
/// @brief One session's artwork stream (spec roles/artwork/v1.md): what its messages have told the
/// client, the transfer in flight, and each step that moves the client toward its group's images,
/// decided when the pump takes it, as markers are. The artwork queue is these steps. Core thread
/// only.

#pragma once

#include "artwork.h"
#include "artwork_cache.h"
#include "messages.h"
#include "sched/constants.h"
#include "send_queues.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sendspin::server {

/// @brief The least time between two parts of a transfer (spec roles/artwork/v1.md "Artwork
/// (Binary)": a server SHOULD pace a transfer's parts), so that at the default part size
/// artwork takes at most about 1.6 MB/s of a connection
constexpr int64_t ARTWORK_PART_INTERVAL_US = 10000;

/// @brief A session's group, as its artwork stream reads it
class ArtworkView {
public:
    virtual ~ArtworkView() = default;

    /// @brief The group's images for source at now_us
    virtual ArtworkSchedule artwork(ArtworkSource source, int64_t now_us) const = 0;

    /// @brief image encoded for channel (ArtworkCache::encoded()); nullopt while it is encoded,
    /// and the group pumps its members once it is
    virtual std::optional<EncodedImage> encoded(const ArtworkImage& image,
                                                const ArtworkChannel& channel) = 0;
};

/// @brief One message the artwork stream sends next
struct ArtworkStep {
    enum class Kind : uint8_t {
        NONE,
        /// Discards the channel's pending image, and ends the transfer in flight on it
        CANCEL,
        /// stream/end for the artwork role
        END,
        /// An empty image, at the time it is sent, for a channel the next stream/start sets to
        /// `none`
        CLEAR,
        /// stream/start with the channels the client's latest client/state declares
        START,
        /// An image's announce: its parts follow, or none for an empty image
        ANNOUNCE,
        /// The next part of the transfer in flight
        PART,
    };

    Kind kind{Kind::NONE};
    size_t channel{0};
    /// ANNOUNCE: the image and its encoding
    ArtworkImage image;
    EncodedImage encoded;
    /// NONE: when a step becomes due by time alone, a part's pacing or a scheduled image coming
    /// within ARTWORK_SCHEDULE_AHEAD_US
    int64_t wake_us{sched::NEVER};
};

class ArtworkStream {
public:
    // ------------------------------------------------------------------------------
    // Inputs
    // ------------------------------------------------------------------------------

    /// @brief An activation was taken for writing
    void on_activation_written(bool artwork_active);

    /// @brief A validated client/state: its `available`, and the channels of an artwork object
    /// while the role is written (messaging.md "client/state": an omitted object leaves them)
    void on_client_state(const ClientState& state);

    /// @brief The client left its group with client/leave, which ends the stream as
    /// unavailability does (messaging.md "client/leave"); a move into a group, a return to
    /// `available: true` or the group's next run clears it
    void set_left(bool left) {
        this->left_ = left;
    }

    // ------------------------------------------------------------------------------
    // Steps
    // ------------------------------------------------------------------------------

    /// @brief What a marker at the head of the control FIFO owes: while the stream is active and no
    /// longer wanted, a cancel of the transfer in flight, then stream/end (spec roles/artwork/v1.md
    /// "Artwork (Binary)")
    /// @param role_ahead the role is in the activation the marker evaluates against
    std::optional<ArtworkStep> end_step(bool role_ahead) const;

    /// @brief The artwork queue's next step: ending, starting or reconfiguring the stream as the
    /// client's state asks, then the transfer in flight, then each channel's current image and
    /// scheduled image in channel order. NONE while nothing is due.
    ArtworkStep next_step(ArtworkView* view, bool role_ahead, int64_t now_us);

    /// @brief Builds step's message with the stamp and applies it in the same step
    /// @param max_message_bytes the largest part message
    OutboundPlaintext commit(const ArtworkStep& step, int64_t stamp_us, size_t max_message_bytes);

    // ------------------------------------------------------------------------------
    // Aging
    // ------------------------------------------------------------------------------

    /// @brief Records whether the queue has a step ready now, for aged()
    void note_ready(bool ready, int64_t now_us);

    /// @brief The ready step has waited age_us and no aged message was written in the last age_us
    bool aged(int64_t age_us, int64_t now_us) const;

    /// @brief An artwork message was written; aged when it went ahead of audio
    void on_written(bool aged, int64_t stamp_us);

    // ------------------------------------------------------------------------------
    // Queries
    // ------------------------------------------------------------------------------

    /// @brief A stream/start written and no stream/end since
    bool active() const {
        return this->active_;
    }

    bool role_written() const {
        return this->role_written_;
    }

    /// @brief The channels of the last stream/start written
    const ArtworkChannels& sent_channels() const {
        return this->sent_;
    }

private:
    /// What the messages written leave a channel showing: the image whose announce completed
    /// with a past timestamp, nullopt until one did in this stream configuration, and the
    /// scheduled image held
    struct ChannelSide {
        struct Pending {
            uint64_t id{0};
            int64_t at_us{0};
        };
        std::optional<uint64_t> current;
        std::optional<Pending> pending;
    };

    struct Transfer {
        size_t channel{0};
        uint64_t id{0};
        int64_t at_us{0};
        EncodedImage bytes;
        size_t offset{0};
    };

    /// The stream should be active: the role written and staying, its client/state object since the
    /// activation, the client available and not left
    bool wanted(bool role_ahead) const;
    ArtworkChannels requested() const;
    /// A scheduled image whose time came is what the client shows
    void promote(int64_t now_us);
    /// An image's transfer completed at stamp_us
    void complete(size_t channel, uint64_t id, int64_t at_us, int64_t stamp_us);

    bool role_written_{false};
    bool state_since_activation_{false};
    bool available_{false};
    bool state_received_{false};
    bool left_{false};
    std::optional<std::vector<ArtworkChannel>> declared_;

    bool active_{false};
    ArtworkChannels sent_{};
    std::array<ChannelSide, MAX_ARTWORK_CHANNELS> sides_{};
    /// Channels the server cleared for a stream/start that sets them to `none`
    std::array<bool, MAX_ARTWORK_CHANNELS> cleared_{};
    std::optional<Transfer> transfer_;
    /// The next part is written no earlier
    int64_t next_part_us_{0};

    std::optional<int64_t> ready_since_us_;
    std::optional<int64_t> last_aged_write_us_;
};

}  // namespace sendspin::server
