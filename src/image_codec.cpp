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

#include "image_codec.h"

#include "stb_budget.h"
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <memory>

namespace sendspin::server {

namespace {

constexpr int RGBA = 4;
constexpr int RGB = 3;
constexpr unsigned OPAQUE = 255;

constexpr uint8_t MARKER = 0xFF;
constexpr uint8_t SOI = 0xD8;
constexpr uint8_t EOI = 0xD9;
constexpr uint8_t SOS = 0xDA;
constexpr uint8_t TEM = 0x01;
constexpr uint8_t RST0 = 0xD0;
constexpr uint8_t RST7 = 0xD7;
constexpr unsigned BITS_PER_BYTE = 8;

bool is_jpeg(std::span<const uint8_t> bytes) {
    return bytes.size() >= 2 && bytes[0] == MARKER && bytes[1] == SOI;
}

/// Whether a JPEG holds at most artwork_limits::MAX_JPEG_SCANS scans (ITU-T T.81 B.1.1.2): its
/// marker segments are walked, and a scan's entropy-coded data byte by byte, where a 0xFF is
/// followed by a stuffed 0 or a marker. A truncated JPEG is left for the decoder to refuse.
bool scans_within_limit(std::span<const uint8_t> bytes) {
    size_t scans = 0;
    size_t at = 2;
    while (at + 1 < bytes.size()) {
        if (bytes[at] != MARKER) {
            ++at;
            continue;
        }
        const uint8_t marker = bytes[at + 1];
        if (marker == MARKER) {
            // A fill byte
            ++at;
            continue;
        }
        if (marker == 0 || marker == TEM || (marker >= RST0 && marker <= RST7)) {
            // A stuffed byte, or a marker with no segment
            at += 2;
            continue;
        }
        if (marker == EOI || at + 3 >= bytes.size()) {
            return true;
        }
        if (marker == SOS && ++scans > artwork_limits::MAX_JPEG_SCANS) {
            return false;
        }
        const size_t length = (size_t{bytes[at + 2]} << BITS_PER_BYTE) | bytes[at + 3];
        at += 2 + length;
    }
    return true;
}

/// stb_image_write's output, appended to a vector
void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

void StbPixelsFree::operator()(uint8_t* pixels) const {
    stbi_image_free(pixels);
}

bool probe_image(std::span<const uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > artwork_limits::MAX_IMAGE_BYTES) {
        return false;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                              &channels) == 0 ||
        width <= 0 || height <= 0) {
        return false;
    }
    if (uint64_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height) >
        artwork_limits::MAX_IMAGE_PIXELS) {
        return false;
    }
    return !is_jpeg(bytes) || scans_within_limit(bytes);
}

std::optional<DecodedImage> decode_image(std::span<const uint8_t> bytes) {
    if (!probe_image(bytes)) {
        return std::nullopt;
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels);
    // A decode past what the header declares fails rather than growing without bound
    const StbBudget budget(size_t{static_cast<uint32_t>(width)} * static_cast<uint32_t>(height) *
                               DECODE_BYTES_PER_PIXEL +
                           DECODE_SLACK_BYTES + bytes.size());
    DecodedImage image;
    image.pixels.reset(stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width,
                                             &height, &channels, RGBA));
    if (!image.pixels || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    return image;
}

std::optional<std::vector<uint8_t>> render_image(const DecodedImage& image, ImageFormat format,
                                                 uint32_t width, uint32_t height) {
    const Placement place = letterbox(image.width, image.height, width, height);
    std::vector<uint8_t> scaled(size_t{place.width} * place.height * RGBA);
    // Alpha weighted, so transparent pixels lend no color to their neighbours
    if (stbir_resize_uint8_srgb(image.rgba().data(), static_cast<int>(image.width),
                                static_cast<int>(image.height),
                                static_cast<int>(image.width) * RGBA, scaled.data(),
                                static_cast<int>(place.width), static_cast<int>(place.height),
                                static_cast<int>(place.width) * RGBA, STBIR_RGBA) == nullptr) {
        return std::nullopt;
    }
    // Black, then the scaled image over it
    std::vector<uint8_t> canvas(size_t{width} * height * RGB, 0);
    for (uint32_t row = 0; row < place.height; ++row) {
        const uint8_t* from = &scaled[size_t{row} * place.width * RGBA];
        uint8_t* to = &canvas[(size_t{place.y + row} * width + place.x) * RGB];
        for (uint32_t col = 0; col < place.width; ++col) {
            const unsigned alpha = from[3];
            for (int c = 0; c < RGB; ++c) {
                to[c] = static_cast<uint8_t>((from[c] * alpha + OPAQUE / 2) / OPAQUE);
            }
            from += RGBA;
            to += RGB;
        }
    }
    std::vector<uint8_t> encoded;
    const int written = format == ImageFormat::JPEG
                            ? stbi_write_jpg_to_func(&append, &encoded, static_cast<int>(width),
                                                     static_cast<int>(height), RGB, canvas.data(),
                                                     ARTWORK_JPEG_QUALITY)
                            : stbi_write_png_to_func(&append, &encoded, static_cast<int>(width),
                                                     static_cast<int>(height), RGB, canvas.data(),
                                                     static_cast<int>(width) * RGB);
    if (written == 0 || encoded.empty()) {
        return std::nullopt;
    }
    return encoded;
}

}  // namespace sendspin::server
