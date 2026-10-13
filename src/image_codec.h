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

/// @file image_codec.h
/// @brief Decoding the host's JPEG and PNG images and encoding them at a channel's format and
/// size, through the vendored stb libraries (third_party/stb/VENDORED.md). Pure; decoding and
/// encoding take tens of milliseconds for large images, so they run on the artwork worker.

#pragma once

#include "artwork.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief JPEG quality of the images the server encodes, from 1 to 100
constexpr int ARTWORK_JPEG_QUALITY = 85;

/// @brief The bytes stb_image may hold while decoding an image, per pixel its header declares;
/// a 16-bit interlaced RGBA PNG, the most costly, needs 20 besides its compressed data
constexpr size_t DECODE_BYTES_PER_PIXEL = 24;
/// @brief The bytes stb_image may hold while decoding besides those per pixel and the image's own
constexpr size_t DECODE_SLACK_BYTES = size_t{1} << 20;

/// @brief Frees a DecodedImage's pixels, which stb_image allocated
struct StbPixelsFree {
    void operator()(uint8_t* pixels) const;
};

/// @brief An image decoded to 8-bit RGBA, alpha not premultiplied, in the buffer stb_image
/// decoded it into
struct DecodedImage {
    uint32_t width{0};
    uint32_t height{0};
    std::unique_ptr<uint8_t, StbPixelsFree> pixels;

    /// @brief width * height pixels of 4 bytes, row by row
    std::span<const uint8_t> rgba() const {
        return {this->pixels.get(), size_t{this->width} * this->height * 4};
    }
};

/// @brief Whether bytes start like a JPEG or PNG of at most artwork_limits::MAX_IMAGE_PIXELS,
/// from its header alone, and a JPEG of at most artwork_limits::MAX_JPEG_SCANS scans. Reads every
/// byte of a JPEG.
bool probe_image(std::span<const uint8_t> bytes);

/// @brief The image bytes holds; nullopt for one probe_image() refuses or that does not decode,
/// including one whose decode would hold more than DECODE_BYTES_PER_PIXEL per declared pixel,
/// DECODE_SLACK_BYTES and its own size (a PNG whose data inflates past its declared size)
std::optional<DecodedImage> decode_image(std::span<const uint8_t> bytes);

/// @brief image scaled into a width by height channel at its letterbox() placement, padded with
/// black, its transparent parts over black, and encoded as format; nullopt when the encoder fails.
/// width and height are at least 1.
std::optional<std::vector<uint8_t>> render_image(const DecodedImage& image, ImageFormat format,
                                                 uint32_t width, uint32_t height);

}  // namespace sendspin::server
