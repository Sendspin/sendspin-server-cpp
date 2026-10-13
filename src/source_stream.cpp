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

#include "source_stream.h"

#include <algorithm>

namespace sendspin::server {

void SourceStream::note_ready(bool was) {
    if (this->ready() != was) {
        this->events_.push_back(
            {this->ready() ? SourceEvent::Kind::READY : SourceEvent::Kind::UNREADY, {}, {}});
    }
}

bool SourceStream::set_routed(bool routed) {
    if (routed == this->routed_) {
        return false;
    }
    this->routed_ = routed;
    if (routed) {
        this->held_ = false;
    }
    return true;
}

void SourceStream::on_activation_written(bool role) {
    const bool was = this->ready();
    if (role && !this->role_written_) {
        // messaging.md "client/state": the role's object follows the activation that adds it
        this->state_received_ = false;
        this->held_ = false;
    }
    if (!role) {
        // roles/source/v1.md "Default streaming behavior": the client clears its authorization
        // when the role is removed
        this->sent_.reset();
    }
    this->role_written_ = role;
    this->note_ready(was);
}

void SourceStream::on_client_state(const ClientState& state) {
    const bool was = this->ready();
    if (this->available_ && !state.available) {
        // roles/source/v1.md "Default streaming behavior": unavailability is an implicit stop
        this->sent_.reset();
    }
    if (!this->available_ && state.available) {
        this->held_ = false;
    }
    this->available_ = state.available;
    // messaging.md "server/activate": an inactive role's object is ignored
    if (state.source && this->role_written_) {
        this->state_received_ = true;
        if (state.source->signal != this->signal_) {
            this->signal_ = state.source->signal;
            this->events_.push_back({SourceEvent::Kind::SIGNAL, {}, this->signal_});
        }
    }
    this->note_ready(was);
}

SourceStream::Verdict SourceStream::on_stream_start(const ClientStreamStart& start,
                                                    std::span<const Codec> accepted,
                                                    bool supported) {
    // roles/source/v1.md "client/hello source@v1 support object": only a listed codec
    if (std::find(accepted.begin(), accepted.end(), start.format.codec) == accepted.end()) {
        return Verdict::PROTOCOL_ERROR;
    }
    if (!this->stream_) {
        // roles/source/v1.md "Default streaming behavior": an opening answers a start sent,
        // possibly before a stop or a transition the client had not seen when it opened
        if (this->unanswered_starts_ == 0) {
            return Verdict::PROTOCOL_ERROR;
        }
        --this->unanswered_starts_;
        ++this->stream_number_;
    }
    // roles/source/v1.md "client-stream/start": on an open stream it replaces the format in place
    this->stream_ = start;
    this->failed_ = false;
    if (supported) {
        this->events_.push_back({SourceEvent::Kind::OPENED, start.format, {}});
    } else {
        this->fail();
    }
    return Verdict::TAKE;
}

void SourceStream::on_rerouted() {
    if (this->stream_ && !this->failed_) {
        this->events_.push_back({SourceEvent::Kind::OPENED, this->stream_->format, {}});
    }
}

void SourceStream::on_stream_end() {
    if (!this->stream_) {
        return;
    }
    this->stream_.reset();
    this->failed_ = false;
    // The source ended the stream the last start opened, with no stop or transition the server
    // knows of: it is not asked again until something changes
    if (this->sent_ == SourceCommand::START && this->unanswered_starts_ == 0) {
        this->held_ = true;
    }
    this->events_.push_back({SourceEvent::Kind::CLOSED, {}, {}});
}

SourceStream::Verdict SourceStream::on_chunk() const {
    if (!this->stream_ || !this->available_) {
        return Verdict::PROTOCOL_ERROR;
    }
    return this->routed_ && !this->failed_ ? Verdict::TAKE : Verdict::DISCARD;
}

void SourceStream::fail() {
    if (this->stream_ && !this->failed_) {
        this->events_.push_back({SourceEvent::Kind::FAILED, this->stream_->format, {}});
    }
    this->failed_ = true;
    this->held_ = true;
}

std::optional<SourceCommand> SourceStream::take_command(bool role_ahead) {
    const bool active = this->role_written_ && role_ahead;
    // roles/source/v1.md "Source command semantics": a start only while the role is active, its
    // client/state source object received and the client available
    const bool wanted = this->routed_ && !this->held_ && active && this->ready();
    if (wanted && this->sent_ != SourceCommand::START) {
        this->sent_ = SourceCommand::START;
        ++this->unanswered_starts_;
        return SourceCommand::START;
    }
    if (!wanted && this->sent_ == SourceCommand::START) {
        if (active && this->available_) {
            this->sent_ = SourceCommand::STOP;
            return SourceCommand::STOP;
        }
        this->sent_.reset();
    }
    return std::nullopt;
}

}  // namespace sendspin::server
