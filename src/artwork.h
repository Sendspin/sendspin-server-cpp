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

/// @file artwork.h
/// @brief The artwork role's values (spec roles/artwork/v1.md): channel configurations, the
/// binary messages, the fit of an image into a channel, and a group's images per source. Pure.

#pragma once

#include "sendspin/server/types.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sendspin::server {

/// @brief The most channels an artwork client has (spec roles/artwork/v1.md "client/state artwork
/// object")
constexpr size_t MAX_ARTWORK_CHANNELS = 4;

/// @brief Channel 0's binary message type; channel n uses this plus n
constexpr uint8_t MESSAGE_ID_ARTWORK = 8;
constexpr uint8_t ARTWORK_FLAG_CANCEL = 0x01;
constexpr uint8_t ARTWORK_FLAG_ANNOUNCE = 0x02;
/// @brief Type, flags, timestamp and total_size
constexpr size_t ARTWORK_ANNOUNCE_BYTES = 14;
/// @brief Type and flags, ahead of a part's data
constexpr size_t ARTWORK_PART_HEADER_BYTES = 2;
/// @brief The largest artwork message (spec roles/artwork/v1.md "Artwork (Binary)")
constexpr size_t MAX_ARTWORK_MESSAGE_BYTES = 65519;

/// @brief A scheduled image is announced at most this long before its timestamp (spec
/// roles/artwork/v1.md "Server rules for scheduled artwork"), which sets it apart from
/// SCHEDULE_AHEAD_US for metadata and colors
constexpr int64_t ARTWORK_SCHEDULE_AHEAD_US = 20000000;

enum class ImageFormat : uint8_t { JPEG, PNG };

/// @brief One channel's configuration, as a client/state declares it or a stream/start echoes it
struct ArtworkChannel {
    /// nullopt for source `none`, whose format and size are 0
    std::optional<ArtworkSource> source;
    ImageFormat format{ImageFormat::JPEG};
    uint32_t width{0};
    uint32_t height{0};

    bool operator==(const ArtworkChannel&) const = default;
};

/// @brief Every channel's configuration; a channel a client/state does not cover is `none`
using ArtworkChannels = std::array<ArtworkChannel, MAX_ARTWORK_CHANNELS>;

/// @brief The channels of a client/state artwork object, padded with `none`
ArtworkChannels pad_channels(std::span<const ArtworkChannel> declared);

/// @brief The channels a stream/start lists: up to the last one with a source (spec
/// roles/artwork/v1.md "stream/start artwork object": the server SHOULD truncate)
std::span<const ArtworkChannel> streamed_prefix(const ArtworkChannels& channels);

// ==============================================================================
// Binary messages (spec roles/artwork/v1.md "Artwork (Binary)")
// ==============================================================================

std::vector<uint8_t> artwork_announce(size_t channel, int64_t timestamp_us, uint32_t total_size);
std::vector<uint8_t> artwork_part(size_t channel, std::span<const uint8_t> data);
std::vector<uint8_t> artwork_cancel(size_t channel);

// ==============================================================================
// Fitting an image into a channel
// ==============================================================================

/// @brief Where a scaled image sits in a channel; the rest is padded black
struct Placement {
    uint32_t x{0};
    uint32_t y{0};
    uint32_t width{0};
    uint32_t height{0};

    bool operator==(const Placement&) const = default;
};

/// @brief The largest size of a src_width by src_height image, with its aspect ratio, that fits a
/// width by height channel, centred (spec roles/artwork/v1.md "client/state artwork object":
/// scaled to fit, aspect ratio preserved, padded black, never cropped). The scaled side is rounded
/// to the nearest pixel and is at least one; the padding is split with the extra pixel after.
/// Every dimension is at least 1.
Placement letterbox(uint32_t src_width, uint32_t src_height, uint32_t width, uint32_t height);

// ==============================================================================
// A group's images
// ==============================================================================

/// @brief An image the host gave, as its encoded bytes and their SHA-256, which the server's
/// encodings of it are cached under
struct SourceImage {
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 32> hash{};
};

/// @brief One image a group shows for a source from at_us: a host's image, or none (cleared)
struct ArtworkImage {
    /// Unique per server; a client's channel shows this image once an announce of it completed
    uint64_t id{0};
    int64_t at_us{0};
    /// nullptr for no image: channels are cleared with an empty image
    std::shared_ptr<const SourceImage> image;
};

/// @brief What a source shows at one time: the current image and the one scheduled
struct ArtworkSchedule {
    ArtworkImage current;
    std::optional<ArtworkImage> pending;
};

/// @brief A group's images for one source, with at most one scheduled (spec roles/artwork/v1.md
/// "Server rules for scheduled artwork": a new scheduled image replaces the one held)
class ArtworkTimeline {
public:
    /// @param initial the image before any is set: none, from the group's creation
    explicit ArtworkTimeline(ArtworkImage initial) : current_(std::move(initial)) {}

    /// @brief The schedule at now_us, a scheduled image whose time came being current
    ArtworkSchedule at(int64_t now_us) const;

    /// @brief Sets image from its at_us: past or present, it is current and drops a scheduled
    /// image, keeping the current image's id when it holds the same image; to come, it replaces
    /// the scheduled image
    void set(ArtworkImage image, int64_t now_us);

    /// @brief Drops the scheduled image
    void cancel(int64_t now_us);

private:
    void promote(int64_t now_us);

    ArtworkImage current_;
    std::optional<ArtworkImage> pending_;
};

}  // namespace sendspin::server
