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

#include "artwork_stream.h"

#include "fragmentation.h"
#include "sched/selection.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace sendspin::server {

namespace {

constexpr std::array<std::string_view, 1> ARTWORK_ROLES{"artwork"};

OutboundPlaintext binary(std::vector<uint8_t> bytes) {
    return {MessageKind::BINARY, std::move(bytes)};
}

ArtworkStep step_of(ArtworkStep::Kind kind, size_t channel = 0) {
    ArtworkStep step;
    step.kind = kind;
    step.channel = channel;
    return step;
}

}  // namespace

// ==============================================================================
// Inputs
// ==============================================================================

void ArtworkStream::on_activation_written(bool artwork_active) {
    if (artwork_active && !this->role_written_) {
        // messaging.md "stream/start": wait for the client/state the activation requires
        this->state_since_activation_ = false;
    }
    this->role_written_ = artwork_active;
    if (!artwork_active && this->active_) {
        // A marker ahead of every activation ends a removed role's stream first, so this is a
        // bug; the stream is dropped all the same
        this->active_ = false;
        this->transfer_.reset();
    }
}

void ArtworkStream::on_client_state(const ClientState& state) {
    const bool returned = state.available && this->state_received_ && !this->available_;
    this->available_ = state.available;
    this->state_received_ = true;
    if (returned) {
        this->left_ = false;
    }
    // messaging.md "server/activate": an inactive role's object is ignored
    if (this->role_written_ && state.artwork) {
        this->declared_ = *state.artwork;
        this->state_since_activation_ = true;
    }
}

// ==============================================================================
// Steps
// ==============================================================================

bool ArtworkStream::wanted(bool role_ahead) const {
    return this->role_written_ && role_ahead && this->state_since_activation_ &&
           this->declared_.has_value() && this->available_ && !this->left_;
}

ArtworkChannels ArtworkStream::requested() const {
    return this->declared_ ? pad_channels(*this->declared_) : ArtworkChannels{};
}

std::optional<ArtworkStep> ArtworkStream::end_step(bool role_ahead) const {
    if (!this->active_ || this->wanted(role_ahead)) {
        return std::nullopt;
    }
    // spec roles/artwork/v1.md "Artwork (Binary)": a transfer in flight is cancelled before
    // stream/end
    if (this->transfer_) {
        return step_of(ArtworkStep::Kind::CANCEL, this->transfer_->channel);
    }
    return step_of(ArtworkStep::Kind::END);
}

void ArtworkStream::promote(int64_t now_us) {
    for (ChannelSide& side : this->sides_) {
        if (side.pending && side.pending->at_us <= now_us) {
            side.current = side.pending->id;
            side.pending.reset();
        }
    }
}

ArtworkStep ArtworkStream::next_step(ArtworkView* view, bool role_ahead, int64_t now_us) {
    if (std::optional<ArtworkStep> end = this->end_step(role_ahead)) {
        return *end;
    }
    if (!this->wanted(role_ahead) || view == nullptr) {
        return {};
    }
    // messaging.md "stream/start": only while the client is available, which wanted() holds
    if (!this->active_) {
        return step_of(ArtworkStep::Kind::START);
    }
    const ArtworkChannels requested = this->requested();
    if (requested != this->sent_) {
        // spec roles/artwork/v1.md "client/state artwork object": a changed configuration gets
        // a stream/start. Every transfer in flight is cancelled first, since no channel may be
        // cleared while one is, and a channel the stream/start sets to `none` is cleared first.
        if (this->transfer_) {
            return step_of(ArtworkStep::Kind::CANCEL, this->transfer_->channel);
        }
        for (size_t i = 0; i < MAX_ARTWORK_CHANNELS; ++i) {
            if (this->sent_[i].source && !requested[i].source && !this->cleared_[i]) {
                return step_of(ArtworkStep::Kind::CLEAR, i);
            }
        }
        return step_of(ArtworkStep::Kind::START);
    }
    this->promote(now_us);
    if (this->transfer_) {
        const size_t channel = this->transfer_->channel;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): a transfer runs on a streamed channel
        const ArtworkSchedule wanted = view->artwork(*this->sent_[channel].source, now_us);
        // spec roles/artwork/v1.md "Artwork (Binary)": an image no longer wanted is cancelled
        // before another is announced
        if (this->transfer_->id != wanted.current.id &&
            (!wanted.pending || this->transfer_->id != wanted.pending->id)) {
            return step_of(ArtworkStep::Kind::CANCEL, channel);
        }
        if (now_us < this->next_part_us_) {
            ArtworkStep wait;
            wait.wake_us = this->next_part_us_;
            return wait;
        }
        return step_of(ArtworkStep::Kind::PART, channel);
    }
    int64_t wake = sched::NEVER;
    for (size_t i = 0; i < MAX_ARTWORK_CHANNELS; ++i) {
        const ArtworkChannel& channel = this->sent_[i];
        if (!channel.source) {
            continue;
        }
        const ChannelSide& side = this->sides_[i];
        const ArtworkSchedule wanted = view->artwork(*channel.source, now_us);
        // spec roles/artwork/v1.md "stream/start artwork object", "Late join": the current image
        // first, then the scheduled one
        const ArtworkImage* announce = nullptr;
        if (side.current != wanted.current.id) {
            announce = &wanted.current;
        } else if (wanted.pending && (!side.pending || side.pending->id != wanted.pending->id)) {
            // "Server rules for scheduled artwork": announced at most 20 s ahead; a scheduled
            // image it replaces before then is cancelled
            if (wanted.pending->at_us - now_us <= ARTWORK_SCHEDULE_AHEAD_US) {
                announce = &*wanted.pending;
            } else if (side.pending) {
                return step_of(ArtworkStep::Kind::CANCEL, i);
            } else {
                wake = std::min(wake, wanted.pending->at_us - ARTWORK_SCHEDULE_AHEAD_US);
            }
        } else if (!wanted.pending && side.pending) {
            return step_of(ArtworkStep::Kind::CANCEL, i);
        }
        if (announce == nullptr) {
            continue;
        }
        // An encoding still being made holds this channel only; its result pumps the session
        std::optional<EncodedImage> encoded = view->encoded(*announce, channel);
        if (!encoded) {
            continue;
        }
        ArtworkStep step = step_of(ArtworkStep::Kind::ANNOUNCE, i);
        step.image = *announce;
        step.encoded = std::move(*encoded);
        return step;
    }
    ArtworkStep none;
    none.wake_us = wake;
    return none;
}

void ArtworkStream::complete(size_t channel, uint64_t id, int64_t at_us, int64_t stamp_us) {
    ChannelSide& side = this->sides_[channel];
    if (at_us <= stamp_us) {
        side.current = id;
        side.pending.reset();
    } else {
        side.pending = ChannelSide::Pending{id, at_us};
    }
}

OutboundPlaintext ArtworkStream::commit(const ArtworkStep& step, int64_t stamp_us,
                                        size_t max_message_bytes) {
    switch (step.kind) {
        case ArtworkStep::Kind::CANCEL:
            // spec roles/artwork/v1.md "Cancel message": the channel's pending image goes, and
            // with it a transfer in flight on it
            if (this->transfer_ && this->transfer_->channel == step.channel) {
                this->transfer_.reset();
            }
            this->sides_[step.channel].pending.reset();
            return binary(artwork_cancel(step.channel));
        case ArtworkStep::Kind::END:
            this->active_ = false;
            this->sent_ = {};
            this->sides_ = {};
            this->cleared_ = {};
            this->transfer_.reset();
            return json_plaintext(build_stream_end(ARTWORK_ROLES));
        case ArtworkStep::Kind::CLEAR:
            // spec roles/artwork/v1.md "Clearing artwork": a past or present timestamp
            this->cleared_[step.channel] = true;
            this->sides_[step.channel] = {};
            return binary(artwork_announce(step.channel, stamp_us, 0));
        case ArtworkStep::Kind::START: {
            const ArtworkChannels requested = this->requested();
            for (size_t i = 0; i < MAX_ARTWORK_CHANNELS; ++i) {
                // "stream/start artwork object": a changed channel's pending image is discarded
                // and its image sent again
                if (!this->active_ || requested[i] != this->sent_[i]) {
                    this->sides_[i] = {};
                    this->cleared_[i] = false;
                }
            }
            this->active_ = true;
            this->sent_ = requested;
            return json_plaintext(build_stream_start_artwork(stamp_us, streamed_prefix(requested)));
        }
        case ArtworkStep::Kind::ANNOUNCE: {
            // "Artwork (Binary)": an announce discards the channel's pending image. The channel
            // shows an image again, so a later configuration setting it to `none` clears it again.
            this->sides_[step.channel].pending.reset();
            this->cleared_[step.channel] = false;
            const size_t size = step.encoded ? step.encoded->size() : 0;
            if (size == 0) {
                this->complete(step.channel, step.image.id, step.image.at_us, stamp_us);
            } else {
                this->transfer_ =
                    Transfer{step.channel, step.image.id, step.image.at_us, step.encoded, 0};
                this->next_part_us_ = stamp_us;
            }
            return binary(
                artwork_announce(step.channel, step.image.at_us, static_cast<uint32_t>(size)));
        }
        case ArtworkStep::Kind::PART: {
            // NOLINTBEGIN(bugprone-unchecked-optional-access): a part is taken in a transfer
            Transfer& transfer = *this->transfer_;
            const std::vector<uint8_t>& bytes = *transfer.bytes;
            // NOLINTEND(bugprone-unchecked-optional-access)
            const size_t room =
                std::min(max_message_bytes, MAX_ARTWORK_MESSAGE_BYTES) - ARTWORK_PART_HEADER_BYTES;
            const size_t length = std::min(room, bytes.size() - transfer.offset);
            OutboundPlaintext message =
                binary(artwork_part(transfer.channel, std::span<const uint8_t>(bytes).subspan(
                                                          transfer.offset, length)));
            transfer.offset += length;
            this->next_part_us_ = stamp_us + ARTWORK_PART_INTERVAL_US;
            if (transfer.offset == bytes.size()) {
                this->complete(transfer.channel, transfer.id, transfer.at_us, stamp_us);
                this->transfer_.reset();
            }
            return message;
        }
        case ArtworkStep::Kind::NONE:
            break;
    }
    return {};
}

// ==============================================================================
// Aging
// ==============================================================================

void ArtworkStream::note_ready(bool ready, int64_t now_us) {
    if (!ready) {
        this->ready_since_us_.reset();
    } else if (!this->ready_since_us_) {
        this->ready_since_us_ = now_us;
    }
}

bool ArtworkStream::aged(int64_t age_us, int64_t now_us) const {
    return this->ready_since_us_ &&
           sched::artwork_aged(*this->ready_since_us_, this->last_aged_write_us_, age_us, now_us);
}

void ArtworkStream::on_written(bool aged, int64_t stamp_us) {
    this->ready_since_us_.reset();
    if (aged) {
        this->last_aged_write_us_ = stamp_us;
    }
}

}  // namespace sendspin::server
