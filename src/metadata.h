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

/// @file metadata.h
/// @brief A group's metadata over plain data (spec metadata/v1.md): the current state and at most
/// one scheduled update, when each is sent, and the states a pause and a resume call for. Pure:
/// every decision takes `now`.

#pragma once

#include "messages.h"
#include "scheduled_updates.h"
#include "sendspin/server/types.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace sendspin::server {

/// @brief How far ahead of its timestamp a scheduled update is sent at most (metadata/v1.md
/// "Server rules for scheduled metadata")
constexpr int64_t METADATA_SCHEDULE_AHEAD_US = SCHEDULE_AHEAD_US;

/// @brief state with its progress carried forward to at_us at its playback speed, and its
/// timestamp at_us (metadata/v1.md "Calculating current track position")
MetadataState metadata_at(const MetadataState& state, int64_t at_us);

/// @brief What one step asks the group to send to every member with the metadata role
struct MetadataSends {
    /// In order
    std::vector<MetadataState> states;
};

/// @brief A group's current metadata and its scheduled update, as its members hold them
class MetadataTimeline {
public:
    /// @brief The current state: the last set at a past or present time, or an empty one at
    /// now_us when none was
    MetadataState current(int64_t now_us) const;

    /// @brief The scheduled update while its members hold it, which a joiner gets after current()
    std::optional<MetadataState> sent_pending() const;

    /// @brief Sets metadata taking effect at at_us. Past or present: it is current and a
    /// scheduled update is dropped, as members drop it on receiving it. To come: it replaces the
    /// scheduled update, and is sent once at_us is at most METADATA_SCHEDULE_AHEAD_US away; a
    /// sent update it replaces before then is cancelled by resending the current state. While
    /// paused it is sent at speed 0, and its speed, when not 0, is the one resume() restores.
    MetadataSends set(const Metadata& metadata, int64_t at_us, int64_t now_us);

    /// @brief Drops the scheduled update; one already sent is cancelled by resending the current
    /// state
    MetadataSends cancel(int64_t now_us);

    /// @brief The run ended: the current state at now_us with its speed 0, and the scheduled
    /// update dropped. With paused, until resume() the timeline is paused.
    MetadataSends stop(int64_t now_us, bool paused);

    /// @brief A run started at start_us, the time its first sample plays: after a pause the
    /// last speed above 0 that was set comes back then, as a scheduled update
    MetadataSends resume(int64_t start_us, int64_t now_us);

    /// @brief Promotes a scheduled update whose time has come and sends one that came within
    /// METADATA_SCHEDULE_AHEAD_US
    MetadataSends advance(int64_t now_us);

    /// @brief When advance() has something to do; nullopt for never
    std::optional<int64_t> next_due_us() const;

private:
    ScheduledUpdates<MetadataState> updates_;
    /// The last speed above 0 set, which a resume restores
    std::optional<uint32_t> speed_;
    /// Between stop() for a pause and resume()
    bool paused_{false};
};

}  // namespace sendspin::server
