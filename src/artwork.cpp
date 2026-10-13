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

#include "artwork.h"

#include "fragmentation.h"

#include <algorithm>

namespace sendspin::server {

namespace {

uint8_t message_id(size_t channel) {
    return static_cast<uint8_t>(MESSAGE_ID_ARTWORK + channel);
}

/// a * b / c rounded to the nearest integer, halves up
uint64_t scaled(uint64_t a, uint64_t b, uint64_t c) {
    return (2 * a * b + c) / (2 * c);
}

bool same_image(const ArtworkImage& a, const ArtworkImage& b) {
    if (!a.image || !b.image) {
        return !a.image && !b.image;
    }
    return a.image->hash == b.image->hash;
}

}  // namespace

ArtworkChannels pad_channels(std::span<const ArtworkChannel> declared) {
    ArtworkChannels channels{};
    std::copy_n(declared.begin(), std::min(declared.size(), channels.size()), channels.begin());
    return channels;
}

std::span<const ArtworkChannel> streamed_prefix(const ArtworkChannels& channels) {
    size_t count = channels.size();
    while (count > 0 && !channels[count - 1].source) {
        --count;
    }
    return {channels.data(), count};
}

// ==============================================================================
// Binary messages
// ==============================================================================

std::vector<uint8_t> artwork_announce(size_t channel, int64_t timestamp_us, uint32_t total_size) {
    std::vector<uint8_t> message;
    reserve_plaintext(message, ARTWORK_ANNOUNCE_BYTES);
    message.resize(ARTWORK_ANNOUNCE_BYTES);
    message[0] = message_id(channel);
    message[1] = ARTWORK_FLAG_ANNOUNCE;
    // Big-endian timestamp and total_size (spec roles/artwork/v1.md "Artwork (Binary)")
    const auto ts = static_cast<uint64_t>(timestamp_us);
    for (size_t i = 0; i < sizeof(ts); ++i) {
        message[2 + i] = static_cast<uint8_t>(ts >> (8 * (sizeof(ts) - 1 - i)));
    }
    for (size_t i = 0; i < sizeof(total_size); ++i) {
        message[2 + sizeof(ts) + i] =
            static_cast<uint8_t>(total_size >> (8 * (sizeof(total_size) - 1 - i)));
    }
    return message;
}

std::vector<uint8_t> artwork_part(size_t channel, std::span<const uint8_t> data) {
    std::vector<uint8_t> message;
    reserve_plaintext(message, ARTWORK_PART_HEADER_BYTES + data.size());
    message.push_back(message_id(channel));
    message.push_back(0);
    message.insert(message.end(), data.begin(), data.end());
    return message;
}

std::vector<uint8_t> artwork_cancel(size_t channel) {
    // A part's header with the cancel flag and no data
    std::vector<uint8_t> message;
    reserve_plaintext(message, ARTWORK_PART_HEADER_BYTES);
    message.push_back(message_id(channel));
    message.push_back(ARTWORK_FLAG_CANCEL);
    return message;
}

// ==============================================================================
// Fitting an image into a channel
// ==============================================================================

Placement letterbox(uint32_t src_width, uint32_t src_height, uint32_t width, uint32_t height) {
    Placement placement;
    // The image is wider than the channel, relative to their heights, when sw / sh >= w / h
    if (uint64_t{src_width} * height >= uint64_t{src_height} * width) {
        placement.width = width;
        placement.height = static_cast<uint32_t>(
            std::clamp<uint64_t>(scaled(width, src_height, src_width), 1, height));
    } else {
        placement.height = height;
        placement.width = static_cast<uint32_t>(
            std::clamp<uint64_t>(scaled(height, src_width, src_height), 1, width));
    }
    placement.x = (width - placement.width) / 2;
    placement.y = (height - placement.height) / 2;
    return placement;
}

// ==============================================================================
// ArtworkTimeline
// ==============================================================================

ArtworkSchedule ArtworkTimeline::at(int64_t now_us) const {
    if (this->pending_ && this->pending_->at_us <= now_us) {
        return {*this->pending_, std::nullopt};
    }
    return {this->current_, this->pending_};
}

void ArtworkTimeline::promote(int64_t now_us) {
    if (this->pending_ && this->pending_->at_us <= now_us) {
        this->current_ = std::move(*this->pending_);
        this->pending_.reset();
    }
}

void ArtworkTimeline::set(ArtworkImage image, int64_t now_us) {
    this->promote(now_us);
    if (image.at_us > now_us) {
        this->pending_ = std::move(image);
        return;
    }
    // A current image discards the scheduled one, as a client discards its pending image on any
    // announce (spec roles/artwork/v1.md "Artwork (Binary)")
    this->pending_.reset();
    // The same image again stays current, so no channel shows it a second time
    if (!same_image(image, this->current_)) {
        this->current_ = std::move(image);
    }
}

void ArtworkTimeline::cancel(int64_t now_us) {
    this->promote(now_us);
    this->pending_.reset();
}

}  // namespace sendspin::server
