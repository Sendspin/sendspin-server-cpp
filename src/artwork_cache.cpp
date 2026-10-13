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

#include "artwork_cache.h"

#include <utility>

namespace sendspin::server {

std::optional<EncodedImage> ArtworkCache::encoded(const ArtworkImage& image,
                                                  const ArtworkChannel& channel) {
    if (!image.image || !channel.source || channel.width == 0 || channel.height == 0 ||
        channel.width > artwork_limits::MAX_CHANNEL_SIDE ||
        channel.height > artwork_limits::MAX_CHANNEL_SIDE) {
        return EncodedImage{};
    }
    const ArtworkVariant key{image.image->hash, channel.format, channel.width, channel.height};
    const auto found = this->variants_.find(key);
    if (found != this->variants_.end()) {
        found->second.used = ++this->tick_;
        return found->second.encoded;
    }
    if (this->submit_({image.image, key})) {
        this->variants_.emplace(key, Variant{std::nullopt, ++this->tick_});
    }
    return std::nullopt;
}

std::optional<Colors> ArtworkCache::palette(const std::shared_ptr<const SourceImage>& image) {
    const auto found = this->palettes_.find(image->hash);
    if (found != this->palettes_.end()) {
        found->second.used = ++this->tick_;
        return found->second.colors;
    }
    if (this->submit_({image, std::nullopt})) {
        this->palettes_.emplace(image->hash, Palette{std::nullopt, ++this->tick_});
    }
    return std::nullopt;
}

void ArtworkCache::on_result(ArtworkResult result) {
    if (result.job.variant) {
        const auto found = this->variants_.find(*result.job.variant);
        if (found == this->variants_.end() || found->second.encoded) {
            return;
        }
        // Used now, so it outlives the encodings looked up while it was being made
        found->second.used = ++this->tick_;
        if (result.encoded && result.encoded->size() <= MAX_ENCODED_BYTES) {
            this->bytes_ += result.encoded->size();
            found->second.encoded =
                std::make_shared<const std::vector<uint8_t>>(std::move(*result.encoded));
        } else {
            found->second.encoded = EncodedImage{};
        }
    } else {
        const auto found = this->palettes_.find(result.job.image->hash);
        if (found == this->palettes_.end() || found->second.colors) {
            return;
        }
        found->second.used = ++this->tick_;
        found->second.colors = result.colors;
    }
    this->evict();
}

void ArtworkCache::evict() {
    // The least recently used of those done; one being encoded is kept, and core asked for it
    while (this->variants_.size() > MAX_VARIANTS || this->bytes_ > MAX_BYTES) {
        auto oldest = this->variants_.end();
        for (auto it = this->variants_.begin(); it != this->variants_.end(); ++it) {
            if (it->second.encoded &&
                (oldest == this->variants_.end() || it->second.used < oldest->second.used)) {
                oldest = it;
            }
        }
        if (oldest == this->variants_.end()) {
            break;
        }
        const std::optional<EncodedImage>& encoded = oldest->second.encoded;
        if (encoded && *encoded) {
            this->bytes_ -= (*encoded)->size();
        }
        this->variants_.erase(oldest);
    }
    while (this->palettes_.size() > MAX_PALETTES) {
        auto oldest = this->palettes_.end();
        for (auto it = this->palettes_.begin(); it != this->palettes_.end(); ++it) {
            if (it->second.colors &&
                (oldest == this->palettes_.end() || it->second.used < oldest->second.used)) {
                oldest = it;
            }
        }
        if (oldest == this->palettes_.end()) {
            break;
        }
        this->palettes_.erase(oldest);
    }
}

}  // namespace sendspin::server
