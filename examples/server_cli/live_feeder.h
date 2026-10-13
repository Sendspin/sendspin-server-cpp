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

/// @file live_feeder.h
/// @brief LiveFeeder, a thread that plays PCM read from a file descriptor through a live
/// GroupInput

#pragma once

#include <sendspin/server/server.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace sendspin::server::examples {

/// @brief Reads raw PCM from a file descriptor (server_cli's standard input) on its own thread and
/// writes it, as it arrives, into a live GroupInput (InputKind::LIVE): no seek, and what the input
/// refuses is dropped, since a live source cannot wait, so the producer must deliver in real time.
/// It reads with no input attached too, dropping the data, so the producer never blocks. At the
/// end of the data it drains the input. Every method may be called from any thread.
class LiveFeeder {
public:
    /// @brief What the feeder reports: the data ended and a drain was issued, the drain played,
    /// the server stopped the group's playback (acknowledged, and nothing is written until
    /// resume()), the input closed, or the data ended with nothing left to play it (no input
    /// attached, stopped by the server, or detached while its drain played). Once the data ended,
    /// one of PLAYED, STOPPED_BY_SERVER, CLOSED or ENDED follows unless the feeder is destroyed
    /// first.
    enum class Event : uint8_t { END_OF_INPUT, PLAYED, STOPPED_BY_SERVER, CLOSED, ENDED };

    struct Status {
        bool paused{false};
        /// The server stopped playback; resume() plays again
        bool stopped{false};
        /// The data ended
        bool ended{false};
        /// Samples written to the attached input, and those it refused
        uint64_t written{0};
        uint64_t dropped{0};
        int64_t latency_us{0};
        InputState input_state{InputState::OPEN};
    };

    /// @param fd read until the end of its data; the feeder does not close it
    /// @param on_event called on the feeder's thread, from on_drained() or from detach(), with no
    /// lock held, with the group of the input the event is about (GroupId{} with none attached);
    /// it must not destroy the feeder or call it
    LiveFeeder(int fd, const InputFormat& format, std::function<void(GroupId, Event)> on_event);
    /// @brief Stops the thread and joins it, then stops and releases the input
    ~LiveFeeder();
    LiveFeeder(const LiveFeeder&) = delete;
    LiveFeeder& operator=(const LiveFeeder&) = delete;

    const InputFormat& format() const {
        return this->format_;
    }

    /// @brief Writes what is read from now on into input, a live input of format(); the input
    /// attached before is stopped and released
    /// @return false once the data ended, leaving input unused
    bool attach(std::shared_ptr<GroupInput> input);

    /// @brief Stops and releases the input attached to group; what is read is then dropped.
    /// Reports ENDED when the data ended and its drain had no outcome yet.
    /// @return false when none is attached there
    bool detach(GroupId group);

    /// @return the paused position (GroupInput::pause()), or nullopt when no input of group is
    /// attached, it is paused already or the server stopped it
    std::optional<uint64_t> pause(GroupId group);

    /// @brief Resumes the input of group at the newest data, or plays again after a stop by the
    /// server
    /// @return false when none is attached there, or it is neither paused nor stopped
    bool resume(GroupId group);

    /// @brief Takes the ServerListener::on_drained() of every input
    void on_drained(InputId input, DrainId drain, DrainOutcome outcome);

    /// @return nullopt when no input of group is attached
    std::optional<Status> status(GroupId group) const;

private:
    void run();
    /// Writes frames whole frames into the attached input, under the lock
    /// @param released takes the input once it closed, to be released after the lock
    /// @return what to report once the lock is released
    std::optional<Event> write_locked(const uint8_t* pcm, size_t frames,
                                      std::shared_ptr<GroupInput>& released);
    /// The data ended: drains the attached input, or reports that nothing is left to play
    Event end_locked();
    void report(const std::optional<Event>& event, GroupId group);

    int fd_;
    InputFormat format_;
    size_t frame_bytes_;
    std::function<void(GroupId, Event)> on_event_;
    /// A pipe the destructor writes to, which ends the thread's wait for data
    int wake_[2]{-1, -1};

    mutable std::mutex mutex_;
    std::shared_ptr<GroupInput> input_;
    bool paused_{false};
    bool stopped_{false};
    bool ended_{false};
    std::optional<DrainId> drain_;
    uint64_t written_{0};
    uint64_t dropped_{0};
    std::thread thread_;
};

}  // namespace sendspin::server::examples
