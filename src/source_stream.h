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

/// @file source_stream.h
/// @brief SourceStream: one session's source role (roles/source/v1.md): the start and stop
/// commands the server sends, decided when a marker reaches the head of the control FIFO, and the
/// input stream the client opens, replaces and ends, with what each of its messages does. A value
/// type with no I/O. Core thread only.

#pragma once

#include "messages.h"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief A change in a session's source role for its owner to act on
struct SourceEvent {
    enum class Kind : uint8_t {
        /// The source may be started: its role is active, the client/state source object the
        /// activation asks for arrived, and the client is available
        READY,
        /// It cannot be started
        UNREADY,
        /// An input stream opened, or one open took a new format in place; `format` is its format
        OPENED,
        /// The input stream ended
        CLOSED,
        /// The server cannot take the stream in `format` (SourceStream::fail())
        FAILED,
        /// The source's line sensing changed; `signal` is the new value
        SIGNAL,
    };

    Kind kind{Kind::READY};
    AudioFormat format;
    std::optional<SourceSignal> signal;
};

class SourceStream {
public:
    /// @brief What an input stream message does
    enum class Verdict : uint8_t {
        /// The stream takes it: a chunk goes to the source's route
        TAKE,
        /// Valid, but its audio goes nowhere: the source is not routed or its stream failed
        /// (roles/source/v1.md "Source Audio Chunks (Binary)": the server MAY discard audio
        /// payloads after a stop)
        DISCARD,
        /// A protocol error: the connection closes
        PROTOCOL_ERROR,
    };

    /// @brief Whether the host routes this source, so the server asks it to stream
    /// @return whether that changed, which a marker then acts on
    bool set_routed(bool routed);

    bool routed() const {
        return this->routed_;
    }

    /// @brief Whether the source role is in the activation last written
    bool role_written() const {
        return this->role_written_;
    }

    /// @brief An activation was taken for writing, with or without the source role
    void on_activation_written(bool role);

    /// @brief A client/state taken in
    void on_client_state(const ClientState& state);

    /// @brief A client-stream/start (roles/source/v1.md "Default streaming behavior"): opens a
    /// stream when a start sent is unanswered, or replaces an open stream's format in place
    /// @param accepted the codecs server/hello lists
    /// @param supported whether the server takes its format; a stream it does not take fails
    /// (fail()) without being reported as opened
    Verdict on_stream_start(const ClientStreamStart& start, std::span<const Codec> accepted,
                            bool supported);

    /// @brief The open stream now goes to another route: reported as opened again, unless it
    /// failed
    void on_rerouted();

    /// @brief A client-stream/end; ignored with no stream open
    void on_stream_end();

    /// @brief What a source audio message does in the current state (roles/source/v1.md "Source
    /// Audio Chunks (Binary)": a protocol error with no stream open or while the client reports
    /// itself unavailable)
    Verdict on_chunk() const;

    /// @brief The server cannot take the open stream: its audio is discarded, a stop goes out,
    /// and no start until the source is routed again, becomes available again or its role is
    /// added again
    void fail();

    /// @brief The command the marker at the head emits, if any, and records it as sent. A stop
    /// that the client does not need, because the role is not active or the client is
    /// unavailable, which each clear its start authorization, is recorded without a message.
    /// @param role_ahead whether the source role is in the activation the marker evaluates against
    std::optional<SourceCommand> take_command(bool role_ahead);

    /// @brief The open stream's client-stream/start; nullopt with none open
    const std::optional<ClientStreamStart>& stream() const {
        return this->stream_;
    }

    /// @brief Counts the streams opened, so a later report can name the one it is about
    uint64_t stream_number() const {
        return this->stream_number_;
    }

    /// @brief The open stream failed (fail())
    bool failed() const {
        return this->failed_;
    }

    /// @brief The changes since the last call
    std::vector<SourceEvent> take_events() {
        return std::exchange(this->events_, {});
    }

private:
    bool ready() const {
        return this->role_written_ && this->state_received_ && this->available_;
    }

    /// Reports READY or UNREADY when ready() changed since was
    void note_ready(bool was);

    bool routed_{false};
    /// The source role is in the activation last written
    bool role_written_{false};
    /// A client/state with a source object arrived since the activation that added the role
    bool state_received_{false};
    /// The latest client/state's `available`
    bool available_{false};
    std::optional<SourceSignal> signal_;
    /// The last command written, until a role removal or unavailability clears it
    std::optional<SourceCommand> sent_;
    /// Starts written that no client-stream/start has answered by opening a stream: a stop, an
    /// unavailability or a role removal leaves them, since a client-stream/start in flight
    /// answers one (roles/source/v1.md "Default streaming behavior")
    uint32_t unanswered_starts_{0};
    /// No start until the source is routed again, becomes available again, or its role is added
    /// again: its stream failed, or it ended the stream a start opened on its own
    bool held_{false};
    std::optional<ClientStreamStart> stream_;
    uint64_t stream_number_{0};
    bool failed_{false};
    std::vector<SourceEvent> events_;
};

}  // namespace sendspin::server
