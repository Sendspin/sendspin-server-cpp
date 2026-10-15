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

/// @file artwork_cache.h
/// @brief Core's cache of the host's images encoded for channels, keyed by (hash, format, size),
/// and of their palettes, keyed by hash. A lookup that misses asks the artwork worker through
/// submit and reports the encoding pending; the result comes back through on_result(). Core thread
/// only.

#pragma once

#include "artwork.h"
#include "artwork_worker.h"
#include "sendspin/server/types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace sendspin::server {

class ArtworkCache {
public:
    /// @brief Encodings kept at most, and their bytes: a few channel configurations for each
    /// group's current and scheduled images. The least recently used goes first; one being
    /// encoded is never dropped.
    static constexpr size_t MAX_VARIANTS = 64;
    static constexpr size_t MAX_BYTES = size_t{64} * 1024 * 1024;
    /// @brief The largest encoding kept, a quarter of MAX_BYTES; a larger one is an empty image
    static constexpr size_t MAX_ENCODED_BYTES = MAX_BYTES / 4;
    /// @brief Palettes kept at most, the least recently used going first
    static constexpr size_t MAX_PALETTES = 64;

    /// @brief Hands a job to the worker; false when it refused it
    using Submit = std::function<bool(ArtworkJob)>;

    explicit ArtworkCache(Submit submit) : submit_(std::move(submit)) {}

    /// @brief A new ArtworkImage id
    uint64_t next_image_id() {
        return this->next_image_id_++;
    }

    /// @brief image encoded for channel: an empty image for no image, a channel with no source or
    /// one wider or taller than artwork_limits::MAX_CHANNEL_SIDE, and an image that did not
    /// decode or encode, or encoded past MAX_ENCODED_BYTES; nullopt while it is being encoded, or
    /// while the worker refused it, in which case a later lookup asks again
    std::optional<EncodedImage> encoded(const ArtworkImage& image, const ArtworkChannel& channel);

    /// @brief image's colors (palette_colors()), empty when it did not decode; nullopt while
    /// they are being derived, as encoded()
    std::optional<Colors> palette(const std::shared_ptr<const SourceImage>& image);

    /// @brief A result from the worker
    void on_result(ArtworkResult result);

    /// @brief The bytes of the encodings kept
    size_t bytes() const {
        return this->bytes_;
    }

private:
    struct Variant {
        /// Unset while it is being encoded
        std::optional<EncodedImage> encoded;
        uint64_t used{0};
    };

    struct Palette {
        std::optional<Colors> colors;
        uint64_t used{0};
    };

    void evict();

    Submit submit_;
    std::map<ArtworkVariant, Variant> variants_;
    std::map<std::array<uint8_t, 32>, Palette> palettes_;
    size_t bytes_{0};
    uint64_t tick_{0};
    uint64_t next_image_id_{1};
};

}  // namespace sendspin::server
