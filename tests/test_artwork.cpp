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

/// @file test_artwork.cpp
/// @brief The artwork role's pure parts as tables (spec roles/artwork/v1.md): the letterbox fit,
/// the binary messages and stream/start, the client/state artwork object, a group's images per
/// source, the cache of encodings, the image codec, and one session's artwork stream as a
/// transfer state machine over a fake group with the clock passed in.

#include "artwork.h"
#include "artwork_cache.h"
#include "artwork_stream.h"
#include "artwork_worker.h"
#include "fragmentation.h"
#include "image_codec.h"
#include "messages.h"
#include <gtest/gtest.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using sendspin::server::ArtworkCache;
using sendspin::server::ArtworkChannel;
using sendspin::server::ArtworkChannels;
using sendspin::server::ArtworkImage;
using sendspin::server::ArtworkJob;
using sendspin::server::ArtworkResult;
using sendspin::server::ArtworkSchedule;
using sendspin::server::ArtworkSource;
using sendspin::server::ArtworkStep;
using sendspin::server::ArtworkStream;
using sendspin::server::ArtworkTimeline;
using sendspin::server::ArtworkView;
using sendspin::server::ClientState;
using sendspin::server::EncodedImage;
using sendspin::server::ImageFormat;
using sendspin::server::Placement;
using sendspin::server::SourceImage;

// ==============================================================================
// Letterbox
// ==============================================================================

// spec roles/artwork/v1.md "client/state artwork object": scaled to fit preserving the aspect
// ratio, padded black, never cropped
TEST(ArtworkLetterbox, FitsTheImageAndCentresIt) {
    struct Row {
        const char* name;
        uint32_t src_width;
        uint32_t src_height;
        uint32_t width;
        uint32_t height;
        Placement expected;
    };
    const Row rows[] = {
        {"Control: the same aspect fills the channel", 400, 400, 200, 200, {0, 0, 200, 200}},
        {"Control: the same aspect at another ratio", 640, 480, 320, 240, {0, 0, 320, 240}},
        {"a wider image is padded above and below", 300, 200, 100, 100, {0, 16, 100, 67}},
        {"a taller image is padded left and right", 200, 300, 100, 100, {16, 0, 67, 100}},
        {"a small image is scaled up", 10, 5, 100, 100, {0, 25, 100, 50}},
        {"a square image in a wide channel", 100, 100, 320, 240, {40, 0, 240, 240}},
        {"the scaled side rounds to the nearest pixel", 3, 2, 5, 5, {0, 1, 5, 3}},
        {"a half rounds up", 2, 1, 3, 3, {0, 0, 3, 2}},
        {"odd padding leaves the extra pixel after", 4, 2, 4, 5, {0, 1, 4, 2}},
        {"an extreme aspect keeps one pixel", 10000, 1, 100, 100, {0, 49, 100, 1}},
        {"a one-pixel channel", 50, 20, 1, 1, {0, 0, 1, 1}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        EXPECT_EQ(sendspin::server::letterbox(row.src_width, row.src_height, row.width, row.height),
                  row.expected);
    }
}

// ==============================================================================
// Messages
// ==============================================================================

// spec roles/artwork/v1.md "Artwork (Binary)": channel n is type 8 + n; bit 1 announces, bit 0
// cancels; the timestamp and total_size are big-endian
TEST(ArtworkMessages, BuildTheBinaryWireFormat) {
    EXPECT_EQ(sendspin::server::artwork_announce(2, 0x0102030405060708, 0x0A0B0C0D),
              (std::vector<uint8_t>{10, 2, 1, 2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D}));
    EXPECT_EQ(
        sendspin::server::artwork_announce(0, -1, 0),
        (std::vector<uint8_t>{8, 2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0}));
    const std::array<uint8_t, 3> data{7, 8, 9};
    EXPECT_EQ(sendspin::server::artwork_part(3, data), (std::vector<uint8_t>{11, 0, 7, 8, 9}));
    EXPECT_EQ(sendspin::server::artwork_cancel(1), (std::vector<uint8_t>{9, 1}));
}

// spec roles/artwork/v1.md "stream/start artwork object": each channel as the client declared
// it, the array truncated after the last streamed channel
TEST(ArtworkMessages, BuildStreamStartTruncatedAfterTheLastStreamedChannel) {
    ArtworkChannels channels{};
    channels[0] = {ArtworkSource::ALBUM, ImageFormat::JPEG, 300, 300};
    channels[2] = {ArtworkSource::ARTIST, ImageFormat::PNG, 64, 32};
    EXPECT_EQ(sendspin::server::build_stream_start_artwork(
                  5, sendspin::server::streamed_prefix(channels)),
              R"({"type":"stream/start","payload":{"server_transmitted":5,"artwork":{"channels":[)"
              R"({"source":"album","format":"jpeg","width":300,"height":300},{"source":"none"},)"
              R"({"source":"artist","format":"png","width":64,"height":32}]}}})");
    ArtworkChannels none{};
    EXPECT_TRUE(sendspin::server::streamed_prefix(none).empty());
    EXPECT_EQ(
        sendspin::server::build_stream_start_artwork(6, sendspin::server::streamed_prefix(none)),
        R"({"type":"stream/start","payload":{"server_transmitted":6,"artwork":{"channels":[]}}})");
}

ClientState parse_state(const std::string& artwork) {
    const std::string json =
        R"({"type":"client/state","payload":{"available":true,"artwork":)" + artwork + "}}";
    const sendspin::server::ClientMessage message = sendspin::server::parse_client_message(json, 1);
    const auto* state = std::get_if<ClientState>(&message);
    EXPECT_NE(state, nullptr);
    return state != nullptr ? *state : ClientState{};
}

// spec roles/artwork/v1.md "client/state artwork object": 1 to 4 channels; format, width and
// height required unless the source is `none`. More than 4 is a protocol error, which the
// session closes on while the role is active; an object invalid otherwise is ignored.
TEST(ArtworkState, ValidatesTheChannels) {
    const ArtworkChannel album{ArtworkSource::ALBUM, ImageFormat::JPEG, 300, 200};
    const ArtworkChannel none{};
    struct Row {
        const char* name;
        std::string artwork;
        std::optional<std::vector<ArtworkChannel>> expected;
        bool too_many{false};
    };
    const std::string channel = R"({"source":"album","format":"jpeg","width":300,"height":200})";
    const Row rows[] = {
        {"Control: one channel", R"({"channels":[)" + channel + "]}", {{album}}},
        {"Control: none needs nothing else",
         R"({"channels":[{"source":"none"},)" + channel + "]}",
         {{none, album}}},
        {"Control: none ignores a format",
         R"({"channels":[{"source":"none","format":"bmp"}]})",
         {{none}}},
        {"Control: an artist png",
         R"({"channels":[{"source":"artist","format":"png","width":1,"height":2}]})",
         {{{ArtworkSource::ARTIST, ImageFormat::PNG, 1, 2}}}},
        {"Control: four channels",
         R"({"channels":[)" + channel + "," + channel + "," + channel + "," + channel + "]}",
         {{album, album, album, album}}},
        {"five channels",
         R"({"channels":[)" + channel + "," + channel + "," + channel + "," + channel + "," +
             channel + "]}",
         std::nullopt, true},
        {"no channel", R"({"channels":[]})", std::nullopt},
        {"channels not an array", R"({"channels":{}})", std::nullopt},
        {"not an object", R"([])", std::nullopt},
        {"an unknown source",
         R"({"channels":[{"source":"cover","format":"jpeg","width":1,)"
         R"("height":1}]})",
         std::nullopt},
        {"an unknown format",
         R"({"channels":[{"source":"album","format":"bmp","width":1,)"
         R"("height":1}]})",
         std::nullopt},
        {"a missing format", R"({"channels":[{"source":"album","width":1,"height":1}]})",
         std::nullopt},
        {"a missing width", R"({"channels":[{"source":"album","format":"png","height":1}]})",
         std::nullopt},
        {"a zero height",
         R"({"channels":[{"source":"album","format":"png","width":1,"height":0}]})", std::nullopt},
        {"a width that is a string",
         R"({"channels":[{"source":"album","format":"png","width":"1","height":1}]})",
         std::nullopt},
        {"none with an invalid width", R"({"channels":[{"source":"none","width":-1}]})",
         std::nullopt},
        {"a source that is not a string", R"({"channels":[{"source":1}]})", std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const ClientState state = parse_state(row.artwork);
        EXPECT_EQ(state.artwork, row.expected);
        EXPECT_EQ(state.artwork_too_many_channels, row.too_many);
    }
}

// ==============================================================================
// ArtworkTimeline
// ==============================================================================

std::shared_ptr<const SourceImage> image_with(uint8_t tag) {
    auto image = std::make_shared<SourceImage>();
    image->bytes = {tag};
    image->hash.fill(tag);
    return image;
}

// spec roles/artwork/v1.md "Server rules for scheduled artwork": a scheduled image replaces the
// one held; a current image discards it. The same image again keeps its id, so no channel
// shows it again.
TEST(ArtworkTimelineSteps, KeepOneCurrentAndOneScheduledImage) {
    ArtworkTimeline timeline(ArtworkImage{1, 0, nullptr});
    EXPECT_EQ(timeline.at(10).current.id, 1U);
    EXPECT_FALSE(timeline.at(10).pending);

    timeline.set({2, 10, image_with(2)}, 10);
    EXPECT_EQ(timeline.at(10).current.id, 2U);

    timeline.set({3, 50, image_with(3)}, 20);
    ArtworkSchedule schedule = timeline.at(20);
    EXPECT_EQ(schedule.current.id, 2U);
    ASSERT_TRUE(schedule.pending);
    EXPECT_EQ(schedule.pending->id, 3U);
    // Its time come, the scheduled image reads as current
    EXPECT_EQ(timeline.at(50).current.id, 3U);
    EXPECT_FALSE(timeline.at(50).pending);

    timeline.set({4, 60, image_with(4)}, 30);
    EXPECT_EQ(timeline.at(30).pending->id, 4U);

    // The same image as the current one keeps its id, and drops the scheduled one
    timeline.set({5, 30, image_with(2)}, 30);
    schedule = timeline.at(30);
    EXPECT_EQ(schedule.current.id, 2U);
    EXPECT_FALSE(schedule.pending);

    timeline.set({6, 90, image_with(6)}, 40);
    timeline.cancel(40);
    EXPECT_FALSE(timeline.at(40).pending);
    EXPECT_EQ(timeline.at(100).current.id, 2U);

    // A scheduled image that came due is promoted before the next change
    timeline.set({7, 45, nullptr}, 41);
    timeline.set({8, 46, image_with(8)}, 50);
    EXPECT_EQ(timeline.at(50).current.id, 8U);
    timeline.set({9, 60, nullptr}, 60);
    EXPECT_EQ(timeline.at(60).current.id, 9U);
    // No image again: the same
    timeline.set({10, 61, nullptr}, 61);
    EXPECT_EQ(timeline.at(61).current.id, 9U);
}

// ==============================================================================
// ArtworkCache
// ==============================================================================

struct Submitted {
    std::vector<ArtworkJob> jobs;
    bool accept{true};
};

ArtworkCache make_cache(Submitted& submitted) {
    return ArtworkCache([&submitted](ArtworkJob job) {
        if (!submitted.accept) {
            return false;
        }
        submitted.jobs.push_back(std::move(job));
        return true;
    });
}

ArtworkResult encoded_result(const ArtworkJob& job, size_t size) {
    return ArtworkResult{job, std::vector<uint8_t>(size, 1), {}};
}

// Encodings cached by (hash, format, size); a miss asks the worker once, a refusal asks again
// later, a failure is an empty image, and the least recently used goes first
TEST(ArtworkCacheLookups, CacheEachEncodingByHashFormatAndSize) {
    Submitted submitted;
    ArtworkCache cache = make_cache(submitted);
    const ArtworkChannel channel{ArtworkSource::ALBUM, ImageFormat::JPEG, 100, 80};
    const ArtworkImage image{cache.next_image_id(), 0, image_with(1)};

    // No image, no source and a channel past the limit need no encoding
    EXPECT_EQ(cache.encoded({1, 0, nullptr}, channel), EncodedImage{});
    EXPECT_EQ(cache.encoded(image, ArtworkChannel{}), EncodedImage{});
    ArtworkChannel huge = channel;
    huge.width = sendspin::server::artwork_limits::MAX_CHANNEL_SIDE + 1;
    EXPECT_EQ(cache.encoded(image, huge), EncodedImage{});
    EXPECT_TRUE(submitted.jobs.empty());

    // Control: a miss asks once and is pending until the result
    EXPECT_FALSE(cache.encoded(image, channel));
    EXPECT_FALSE(cache.encoded(image, channel));
    ASSERT_EQ(submitted.jobs.size(), 1U);
    ASSERT_TRUE(submitted.jobs[0].variant);
    EXPECT_EQ(submitted.jobs[0].variant->width, 100U);
    EXPECT_EQ(submitted.jobs[0].variant->height, 80U);
    cache.on_result(encoded_result(submitted.jobs[0], 300));
    std::optional<EncodedImage> found = cache.encoded(image, channel);
    ASSERT_TRUE(found && *found);
    EXPECT_EQ((*found)->size(), 300U);
    EXPECT_EQ(cache.bytes(), 300U);

    // Another image with the same bytes shares it; another size or format does not
    EXPECT_TRUE(cache.encoded({cache.next_image_id(), 5, image_with(1)}, channel));
    ArtworkChannel png = channel;
    png.format = ImageFormat::PNG;
    EXPECT_FALSE(cache.encoded(image, png));
    EXPECT_EQ(submitted.jobs.size(), 2U);

    // A refused job is asked for again by the next lookup
    submitted.accept = false;
    ArtworkChannel other = channel;
    other.width = 50;
    EXPECT_FALSE(cache.encoded(image, other));
    submitted.accept = true;
    EXPECT_FALSE(cache.encoded(image, other));
    ASSERT_EQ(submitted.jobs.size(), 3U);

    // A failed encoding, and one past MAX_ENCODED_BYTES, are empty images
    cache.on_result(ArtworkResult{submitted.jobs[1], std::nullopt, {}});
    EXPECT_EQ(cache.encoded(image, png), EncodedImage{});
    cache.on_result(encoded_result(submitted.jobs[2], ArtworkCache::MAX_ENCODED_BYTES + 1));
    EXPECT_EQ(cache.encoded(image, other), EncodedImage{});
    EXPECT_EQ(cache.bytes(), 300U);
}

TEST(ArtworkCacheLookups, DropTheLeastRecentlyUsedEncodingPastTheCap) {
    Submitted submitted;
    ArtworkCache cache = make_cache(submitted);
    const ArtworkImage image{cache.next_image_id(), 0, image_with(1)};
    const auto channel_of = [](uint32_t width) {
        return ArtworkChannel{ArtworkSource::ALBUM, ImageFormat::JPEG, width, 10};
    };
    for (uint32_t width = 1; width <= ArtworkCache::MAX_VARIANTS; ++width) {
        EXPECT_FALSE(cache.encoded(image, channel_of(width)));
        cache.on_result(encoded_result(submitted.jobs.back(), 10));
    }
    // Width 1 is read again, so width 2 is the least recently used
    EXPECT_TRUE(cache.encoded(image, channel_of(1)));
    EXPECT_FALSE(cache.encoded(image, channel_of(ArtworkCache::MAX_VARIANTS + 1)));
    cache.on_result(encoded_result(submitted.jobs.back(), 10));
    const size_t jobs = submitted.jobs.size();
    EXPECT_TRUE(cache.encoded(image, channel_of(1)));
    EXPECT_TRUE(cache.encoded(image, channel_of(ArtworkCache::MAX_VARIANTS + 1)));
    EXPECT_EQ(submitted.jobs.size(), jobs);
    EXPECT_FALSE(cache.encoded(image, channel_of(2)));
    EXPECT_EQ(submitted.jobs.size(), jobs + 1);
}

// An encoding still being made is never dropped, however long it takes: its result is kept and
// the next lookup finds it without asking again
TEST(ArtworkCacheLookups, KeepAnEncodingBeingMadePastTheCap) {
    Submitted submitted;
    ArtworkCache cache = make_cache(submitted);
    const ArtworkImage image{cache.next_image_id(), 0, image_with(1)};
    const auto channel_of = [](uint32_t width) {
        return ArtworkChannel{ArtworkSource::ALBUM, ImageFormat::JPEG, width, 10};
    };
    const ArtworkChannel slow = channel_of(1000);
    EXPECT_FALSE(cache.encoded(image, slow));
    const ArtworkJob slow_job = submitted.jobs.back();
    // Past the cap while it is made, the least recently used of them all
    for (uint32_t width = 1; width <= ArtworkCache::MAX_VARIANTS; ++width) {
        EXPECT_FALSE(cache.encoded(image, channel_of(width)));
        cache.on_result(encoded_result(submitted.jobs.back(), 10));
    }
    cache.on_result(encoded_result(slow_job, 10));
    const size_t jobs = submitted.jobs.size();
    EXPECT_TRUE(cache.encoded(image, slow));
    EXPECT_EQ(submitted.jobs.size(), jobs);
    // Control: the least recently used one done went instead
    EXPECT_FALSE(cache.encoded(image, channel_of(1)));
    EXPECT_EQ(submitted.jobs.size(), jobs + 1);
}

TEST(ArtworkCacheLookups, CacheEachImagesPalette) {
    Submitted submitted;
    ArtworkCache cache = make_cache(submitted);
    const std::shared_ptr<const SourceImage> image = image_with(4);
    EXPECT_FALSE(cache.palette(image));
    EXPECT_FALSE(cache.palette(image));
    ASSERT_EQ(submitted.jobs.size(), 1U);
    EXPECT_FALSE(submitted.jobs[0].variant);
    sendspin::server::Colors colors;
    colors.primary = sendspin::server::RgbColor{1, 2, 3};
    cache.on_result(ArtworkResult{submitted.jobs[0], std::nullopt, colors});
    EXPECT_EQ(cache.palette(image), colors);
    EXPECT_EQ(cache.palette(image_with(4)), colors);
    EXPECT_EQ(submitted.jobs.size(), 1U);
}

// ==============================================================================
// The image codec
// ==============================================================================

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

/// A width by height PNG of one RGBA color
std::vector<uint8_t> png_of(int width, int height, std::array<uint8_t, 4> rgba) {
    std::vector<uint8_t> pixels;
    for (int i = 0; i < width * height; ++i) {
        pixels.insert(pixels.end(), rgba.begin(), rgba.end());
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&append, &png, width, height, 4, pixels.data(), width * 4);
    return png;
}

std::array<uint8_t, 4> pixel(const sendspin::server::DecodedImage& image, uint32_t x, uint32_t y) {
    const size_t at = (size_t{y} * image.width + x) * 4;
    return {image.rgba()[at], image.rgba()[at + 1], image.rgba()[at + 2], image.rgba()[at + 3]};
}

/// A PNG signature, an IHDR declaring width by height, and an empty IDAT
std::vector<uint8_t> png_header(uint32_t width, uint32_t height) {
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
                             0,    0,   0,   13,  'I',  'H',  'D',  'R'};
    for (const uint32_t value : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    png.insert(png.end(), {8, 6, 0, 0, 0, 0, 0, 0, 0});
    png.insert(png.end(), {0, 0, 0, 0, 'I', 'D', 'A', 'T', 0, 0, 0, 0});
    return png;
}

// set_artwork() takes a JPEG or PNG within the limits, judged from its header
TEST(ArtworkCodec, ProbesTheHeaderAgainstTheLimits) {
    EXPECT_TRUE(sendspin::server::probe_image(png_of(4, 2, {1, 2, 3, 255})));
    EXPECT_TRUE(sendspin::server::probe_image(png_header(4000, 4000)));
    EXPECT_FALSE(sendspin::server::probe_image(png_header(5000, 5000)));
    EXPECT_FALSE(sendspin::server::probe_image(std::vector<uint8_t>{1, 2, 3, 4}));
    EXPECT_FALSE(sendspin::server::probe_image({}));
    // A header that probes and data that does not decode
    EXPECT_FALSE(sendspin::server::decode_image(png_header(10, 10)));
}

/// The CRC-32 of a PNG chunk's type and data (ISO 3309)
uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/// png with its IHDR declaring width by height, its CRC fixed; the data is left as it was
std::vector<uint8_t> redeclared(std::vector<uint8_t> png, uint32_t width, uint32_t height) {
    constexpr size_t IHDR_TYPE = 12;
    constexpr size_t IHDR_DATA = 16;
    constexpr size_t IHDR_BYTES = 13;
    size_t at = IHDR_DATA;
    for (const uint32_t value : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png[at++] = static_cast<uint8_t>(value >> shift);
        }
    }
    const uint32_t crc = crc32(&png[IHDR_TYPE], 4 + IHDR_BYTES);
    at = IHDR_DATA + IHDR_BYTES;
    for (int shift = 24; shift >= 0; shift -= 8) {
        png[at++] = static_cast<uint8_t>(crc >> shift);
    }
    return png;
}

// A PNG whose data inflates far past what its header declares is refused, not decoded into
// memory without bound: here a 1024 by 1024 image's 4 MiB of data under a 16 by 16 header
TEST(ArtworkCodec, RefusesAPngThatInflatesPastItsHeader) {
    const std::vector<uint8_t> bomb = redeclared(png_of(1024, 1024, {0, 0, 0, 255}), 16, 16);
    ASSERT_TRUE(sendspin::server::probe_image(bomb));
    EXPECT_FALSE(sendspin::server::decode_image(bomb));
    // Control: stb_image alone decodes it, inflating all the data
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bomb.data(), static_cast<int>(bomb.size()), &width,
                                            &height, &channels, 4);
    ASSERT_NE(pixels, nullptr);
    EXPECT_EQ(width, 16);
    stbi_image_free(pixels);
    // Control: data the size its header declares decodes
    EXPECT_TRUE(sendspin::server::decode_image(png_of(16, 16, {0, 0, 0, 255})));
}

/// A baseline JPEG from stb_image_write with its one scan repeated so it holds scans of them,
/// each after fill 0xFF fill bytes (ITU-T T.81 B.1.1.2)
std::vector<uint8_t> jpeg_with_scans(size_t scans, size_t fill = 0) {
    std::vector<uint8_t> pixels(size_t{16} * 16 * 3);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 37);
    }
    std::vector<uint8_t> jpeg;
    stbi_write_jpg_to_func(&append, &jpeg, 16, 16, 3, pixels.data(), 90);
    size_t sos = 2;
    while (!(jpeg[sos] == 0xFF && jpeg[sos + 1] == 0xDA)) {
        sos += 2 + ((size_t{jpeg[sos + 2]} << 8) | jpeg[sos + 3]);
    }
    const std::vector<uint8_t> scan(jpeg.begin() + static_cast<std::ptrdiff_t>(sos),
                                    jpeg.end() - 2);
    std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(sos));
    for (size_t i = 0; i < scans; ++i) {
        out.insert(out.end(), fill, 0xFF);
        out.insert(out.end(), scan.begin(), scan.end());
    }
    out.insert(out.end(), {0xFF, 0xD9});
    return out;
}

// A JPEG of more scans than MAX_JPEG_SCANS is refused from its bytes, before any decode: each
// scan of a progressive JPEG is a pass over the whole image
TEST(ArtworkCodec, RefusesAJpegOfTooManyScans) {
    EXPECT_TRUE(sendspin::server::probe_image(jpeg_with_scans(1)));
    EXPECT_TRUE(sendspin::server::decode_image(jpeg_with_scans(1)));
    EXPECT_TRUE(sendspin::server::probe_image(
        jpeg_with_scans(sendspin::server::artwork_limits::MAX_JPEG_SCANS)));
    EXPECT_FALSE(sendspin::server::probe_image(
        jpeg_with_scans(sendspin::server::artwork_limits::MAX_JPEG_SCANS + 1)));
    // Fill bytes before each scan's marker are skipped, not read as a segment
    EXPECT_TRUE(sendspin::server::decode_image(jpeg_with_scans(1, 2)));
    EXPECT_FALSE(sendspin::server::probe_image(
        jpeg_with_scans(sendspin::server::artwork_limits::MAX_JPEG_SCANS + 1, 2)));
}

// spec roles/artwork/v1.md "client/state artwork object": exactly width by height, the image
// fitted and centred, the rest black; transparent parts show the black too
TEST(ArtworkCodec, RendersTheLetterboxedImageAtTheChannelsSize) {
    const std::optional<sendspin::server::DecodedImage> red =
        sendspin::server::decode_image(png_of(40, 20, {255, 0, 0, 255}));
    ASSERT_TRUE(red);
    for (const ImageFormat format : {ImageFormat::PNG, ImageFormat::JPEG}) {
        SCOPED_TRACE(format == ImageFormat::PNG ? "png" : "jpeg");
        const std::optional<std::vector<uint8_t>> encoded =
            sendspin::server::render_image(*red, format, 30, 30);
        ASSERT_TRUE(encoded);
        const std::optional<sendspin::server::DecodedImage> out =
            sendspin::server::decode_image(*encoded);
        ASSERT_TRUE(out);
        EXPECT_EQ(out->width, 30U);
        EXPECT_EQ(out->height, 30U);
        // Placed at rows 7 to 21: a JPEG's colors come within a tolerance
        const auto near = [](std::array<uint8_t, 4> got, std::array<uint8_t, 3> want) {
            constexpr int TOLERANCE = 24;
            for (size_t c = 0; c < 3; ++c) {
                if (std::abs(got[c] - want[c]) > TOLERANCE) {
                    return false;
                }
            }
            return true;
        };
        EXPECT_TRUE(near(pixel(*out, 15, 2), {0, 0, 0}));
        EXPECT_TRUE(near(pixel(*out, 15, 14), {255, 0, 0}));
        EXPECT_TRUE(near(pixel(*out, 15, 27), {0, 0, 0}));
        // Control: the padding is black where a full-frame image is not
        const std::optional<std::vector<uint8_t>> full =
            sendspin::server::render_image(*red, format, 40, 20);
        ASSERT_TRUE(full);
        const std::optional<sendspin::server::DecodedImage> filled =
            sendspin::server::decode_image(*full);
        ASSERT_TRUE(filled);
        EXPECT_TRUE(near(pixel(*filled, 20, 1), {255, 0, 0}));
    }
    const std::optional<sendspin::server::DecodedImage> clear =
        sendspin::server::decode_image(png_of(8, 8, {255, 255, 255, 0}));
    ASSERT_TRUE(clear);
    const std::optional<std::vector<uint8_t>> encoded =
        sendspin::server::render_image(*clear, ImageFormat::PNG, 8, 8);
    ASSERT_TRUE(encoded);
    const std::optional<sendspin::server::DecodedImage> out =
        sendspin::server::decode_image(*encoded);
    ASSERT_TRUE(out);
    EXPECT_EQ(pixel(*out, 4, 4), (std::array<uint8_t, 4>{0, 0, 0, 255}));
}

// The worker's job: an encoding of a variant, or a palette, or nothing for an image that did not
// decode
TEST(ArtworkCodec, RunsTheWorkersJobs) {
    auto source = std::make_shared<SourceImage>();
    source->bytes = png_of(16, 16, {0, 0, 200, 255});
    const std::optional<sendspin::server::DecodedImage> decoded =
        sendspin::server::decode_image(source->bytes);
    ASSERT_TRUE(decoded);
    const sendspin::server::ArtworkVariant variant{source->hash, ImageFormat::PNG, 4, 2};
    const ArtworkResult encoded = sendspin::server::run_artwork_job({source, variant}, &*decoded);
    ASSERT_TRUE(encoded.encoded);
    EXPECT_EQ(sendspin::server::decode_image(*encoded.encoded)->width, 4U);
    const ArtworkResult palette =
        sendspin::server::run_artwork_job({source, std::nullopt}, &*decoded);
    EXPECT_EQ(palette.colors.primary, (sendspin::server::RgbColor{4, 4, 204}));
    const ArtworkResult failed = sendspin::server::run_artwork_job({source, variant}, nullptr);
    EXPECT_FALSE(failed.encoded);
}

// The worker queues at most MAX_QUEUED_JOBS behind the job it runs and refuses more; discard()
// drops those queued and refuses later jobs, while the job running is still delivered
TEST(ArtworkWorkerQueue, RefusesPastTheCapAndDiscards) {
    std::mutex mutex;
    std::condition_variable changed;
    bool running = false;
    bool release = false;
    size_t delivered = 0;
    sendspin::server::ArtworkWorker worker([&](ArtworkResult /*result*/) {
        std::unique_lock<std::mutex> lock(mutex);
        running = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        ++delivered;
    });
    const ArtworkJob job{image_with(1), std::nullopt};
    ASSERT_TRUE(worker.post(job));
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return running; });
    }
    for (size_t i = 0; i < sendspin::server::ArtworkWorker::MAX_QUEUED_JOBS; ++i) {
        EXPECT_TRUE(worker.post(job)) << i;
    }
    EXPECT_FALSE(worker.post(job));
    worker.discard();
    EXPECT_FALSE(worker.post(job));
    {
        const std::lock_guard<std::mutex> lock(mutex);
        release = true;
    }
    changed.notify_all();
    worker.stop();
    EXPECT_EQ(delivered, 1U);
}

// ==============================================================================
// ArtworkStream: the transfer state machine
// ==============================================================================

/// A group with one schedule per source and the encodings it has made; an image not in encoded
/// is still being encoded
class FakeView : public ArtworkView {
public:
    ArtworkSchedule artwork(ArtworkSource source, int64_t now_us) const override {
        const ArtworkSchedule& schedule =
            source == ArtworkSource::ALBUM ? this->album : this->artist;
        if (schedule.pending && schedule.pending->at_us <= now_us) {
            return {*schedule.pending, std::nullopt};
        }
        return schedule;
    }

    std::optional<EncodedImage> encoded(const ArtworkImage& image,
                                        const ArtworkChannel& /*channel*/) override {
        if (!image.image) {
            return EncodedImage{};
        }
        const auto found = this->encodings.find(image.id);
        if (found == this->encodings.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    /// An image of size bytes, encoded
    ArtworkImage image(uint64_t id, int64_t at_us, size_t size) {
        this->encodings[id] = std::make_shared<const std::vector<uint8_t>>(size, 0xAB);
        return {id, at_us, image_with(static_cast<uint8_t>(id))};
    }

    ArtworkSchedule album{{1, 0, nullptr}, std::nullopt};
    ArtworkSchedule artist{{2, 0, nullptr}, std::nullopt};
    std::map<uint64_t, EncodedImage> encodings;
};

constexpr size_t MAX_MESSAGE = 1026;  // parts of 1024 bytes
constexpr int64_t PACE = sendspin::server::ARTWORK_PART_INTERVAL_US;

/// A session's artwork stream over a FakeView, written out by the steps it takes
class Harness {
public:
    Harness() {
        this->stream.on_activation_written(true);
    }

    void state(bool available, const std::vector<ArtworkChannel>& channels) {
        ClientState state;
        state.available = available;
        state.artwork = channels;
        this->stream.on_client_state(state);
    }

    /// Every message the stream sends by now, each written out
    std::vector<std::string> take(int64_t now) {
        std::vector<std::string> out;
        for (;;) {
            const ArtworkStep step = this->stream.next_step(&this->view, this->role_ahead, now);
            if (step.kind == ArtworkStep::Kind::NONE) {
                this->wake = step.wake_us;
                return out;
            }
            out.push_back(this->describe(this->stream.commit(step, now, MAX_MESSAGE)));
        }
    }

    /// The next message the stream sends by now, written out, or empty for none
    std::string take_one(int64_t now) {
        const ArtworkStep step = this->stream.next_step(&this->view, this->role_ahead, now);
        if (step.kind == ArtworkStep::Kind::NONE) {
            return {};
        }
        return this->describe(this->stream.commit(step, now, MAX_MESSAGE));
    }

    /// The marker's steps, as a marker at the head takes them
    std::vector<std::string> take_end(int64_t now) {
        std::vector<std::string> out;
        while (const std::optional<ArtworkStep> step = this->stream.end_step(this->role_ahead)) {
            out.push_back(this->describe(this->stream.commit(*step, now, MAX_MESSAGE)));
        }
        return out;
    }

    std::string describe(const sendspin::server::OutboundPlaintext& message) const {
        const std::vector<uint8_t>& bytes = message.bytes;
        if (bytes[0] == sendspin::server::MESSAGE_ID_JSON) {
            const std::string json(bytes.begin() + 1, bytes.end());
            if (json.find("stream/end") != std::string::npos) {
                return "end";
            }
            const size_t channels = json.find("\"channels\":");
            return "start " + json.substr(channels + 11, json.size() - channels - 11 - 3);
        }
        const int channel = bytes[0] - sendspin::server::MESSAGE_ID_ARTWORK;
        if (bytes[1] == sendspin::server::ARTWORK_FLAG_CANCEL) {
            return "cancel " + std::to_string(channel);
        }
        if (bytes[1] == sendspin::server::ARTWORK_FLAG_ANNOUNCE) {
            uint64_t ts = 0;
            uint32_t size = 0;
            for (size_t i = 2; i < 10; ++i) {
                ts = (ts << 8) | bytes[i];
            }
            for (size_t i = 10; i < 14; ++i) {
                size = (size << 8) | bytes[i];
            }
            return "announce " + std::to_string(channel) + " @" + std::to_string(ts) + " " +
                   std::to_string(size);
        }
        return "part " + std::to_string(channel) + " " + std::to_string(bytes.size() - 2);
    }

    ArtworkStream stream;
    FakeView view;
    bool role_ahead{true};
    int64_t wake{0};
};

const ArtworkChannel ALBUM_10{ArtworkSource::ALBUM, ImageFormat::JPEG, 10, 10};
const ArtworkChannel ARTIST_10{ArtworkSource::ARTIST, ImageFormat::PNG, 10, 10};
const ArtworkChannel NONE{};

using Messages = std::vector<std::string>;

// spec roles/artwork/v1.md: no stream before the role's client/state while available; then
// stream/start and the current image of each streamed channel ("Late join"); an image's announce
// then its parts, paced (ARTWORK_PART_INTERVAL_US), each at most max_message_bytes
TEST(ArtworkStreamSteps, StartsThenSendsEachImageInPacedParts) {
    Harness h;
    EXPECT_EQ(h.take(0), Messages{});
    h.state(false, {ALBUM_10});
    EXPECT_EQ(h.take(0), Messages{});
    h.state(true, {ALBUM_10, NONE, ARTIST_10});
    EXPECT_EQ(h.take(0), (Messages{
                             R"(start [{"source":"album","format":"jpeg","width":10,"height":10},)"
                             R"({"source":"none"},{"source":"artist","format":"png","width":10,)"
                             R"("height":10}])",
                             "announce 0 @0 0",
                             "announce 2 @0 0",
                         }));
    // Control: nothing more until the group changes
    EXPECT_EQ(h.take(1), Messages{});

    h.view.album.current = h.view.image(5, 100, 2500);
    EXPECT_EQ(h.take(100), (Messages{"announce 0 @100 2500", "part 0 1024"}));
    EXPECT_EQ(h.wake, 100 + PACE);
    EXPECT_EQ(h.take(100 + PACE - 1), Messages{});
    EXPECT_EQ(h.take(100 + PACE), Messages{"part 0 1024"});
    EXPECT_EQ(h.take(100 + 2 * PACE), Messages{"part 0 452"});
    // Complete: the channel shows it, and nothing is sent again
    EXPECT_EQ(h.take(100 + 3 * PACE), Messages{});
}

// spec roles/artwork/v1.md "Artwork (Binary)": an image no longer wanted is cancelled before
// another is announced, and every transfer is cancelled before stream/end
TEST(ArtworkStreamSteps, CancelsATransferInFlightBeforeReplacingIt) {
    Harness h;
    h.state(true, {ALBUM_10});
    h.take(0);
    h.view.album.current = h.view.image(5, 10, 3000);
    EXPECT_EQ(h.take(10), (Messages{"announce 0 @10 3000", "part 0 1024"}));
    h.view.album.current = h.view.image(6, 20, 500);
    EXPECT_EQ(h.take(20), (Messages{"cancel 0", "announce 0 @20 500", "part 0 500"}));

    // While still encoding, the new image waits; the cancel goes at once
    h.view.album.current = h.view.image(7, 30, 3000);
    EXPECT_EQ(h.take(30), (Messages{"announce 0 @30 3000", "part 0 1024"}));
    h.view.album.current = {8, 40, image_with(8)};
    EXPECT_EQ(h.take(40), Messages{"cancel 0"});
    h.view.encodings[8] = std::make_shared<const std::vector<uint8_t>>(10, 1);
    EXPECT_EQ(h.take(41), (Messages{"announce 0 @40 10", "part 0 10"}));

    // Unavailable mid-transfer: cancel, then stream/end, from the marker
    h.view.album.current = h.view.image(9, 50, 3000);
    EXPECT_EQ(h.take(50), (Messages{"announce 0 @50 3000", "part 0 1024"}));
    h.state(false, {ALBUM_10});
    EXPECT_EQ(h.take_end(51), (Messages{"cancel 0", "end"}));
    EXPECT_EQ(h.take(52), Messages{});
    // Back: a new stream, and the current image again
    h.state(true, {ALBUM_10});
    EXPECT_EQ(h.take(53), (Messages{R"(start [{"source":"album","format":"jpeg","width":10,)"
                                    R"("height":10}])",
                                    "announce 0 @50 3000", "part 0 1024"}));
}

// spec roles/artwork/v1.md "client/state artwork object": a changed configuration gets a
// stream/start, then the changed channels' images; a transfer on a changed channel is cancelled
// first, and a channel set to `none` is cleared before it with a present timestamp
TEST(ArtworkStreamSteps, ReconfiguresWithACancelAClearAndTheImagesAgain) {
    Harness h;
    h.view.album.current = h.view.image(5, 0, 100);
    h.view.artist.current = h.view.image(6, 0, 3000);
    h.state(true, {ALBUM_10, ARTIST_10});
    EXPECT_EQ(h.take(0),
              (Messages{R"(start [{"source":"album","format":"jpeg","width":10,)"
                        R"("height":10},{"source":"artist","format":"png",)"
                        R"("width":10,"height":10}])",
                        "announce 0 @0 100", "part 0 100", "announce 1 @0 3000", "part 1 1024"}));
    // Mid-transfer on channel 1, channel 0 changes size and channel 1 goes to none
    ArtworkChannel bigger = ALBUM_10;
    bigger.width = 20;
    h.state(true, {bigger, NONE});
    EXPECT_EQ(h.take(5), (Messages{"cancel 1", "announce 1 @5 0",
                                   R"(start [{"source":"album","format":"jpeg","width":20,)"
                                   R"("height":10}])",
                                   "announce 0 @0 100", "part 0 100"}));
    // Control: the same configuration sends nothing
    h.state(true, {bigger});
    EXPECT_EQ(h.take(6), Messages{});
}

// spec roles/artwork/v1.md "Clearing artwork": a channel set to `none`, set back before the
// stream/start went and so sent its image again, is cleared again when set to `none` once more
TEST(ArtworkStreamSteps, ClearsAChannelSetToNoneAgainAfterItShowedAnImage) {
    Harness h;
    h.view.album.current = h.view.image(5, 0, 100);
    h.view.artist.current = h.view.image(6, 0, 10);
    h.state(true, {ALBUM_10, ARTIST_10});
    EXPECT_EQ(h.take(0).size(), 5U);
    h.state(true, {ALBUM_10, NONE});
    EXPECT_EQ(h.take_one(1), "announce 1 @1 0");
    // Set back before the stream/start: no stream/start is owed, and the image shows again
    h.state(true, {ALBUM_10, ARTIST_10});
    EXPECT_EQ(h.take(2), (Messages{"announce 1 @0 10", "part 1 10"}));
    h.state(true, {ALBUM_10, NONE});
    EXPECT_EQ(h.take(3),
              (Messages{"announce 1 @3 0", R"(start [{"source":"album","format":"jpeg","width":10,)"
                                           R"("height":10}])"}));
}

// spec roles/artwork/v1.md "Server rules for scheduled artwork": announced at most 20 s ahead,
// after the current image; one replaced before then is cancelled; one whose time came is shown
// and not sent again
TEST(ArtworkStreamSteps, SendsAScheduledImageWithinTwentySeconds) {
    constexpr int64_t S = 1000000;
    Harness h;
    h.state(true, {ALBUM_10});
    h.view.album.pending = h.view.image(5, 30 * S, 10);
    EXPECT_EQ(h.take(0), (Messages{R"(start [{"source":"album","format":"jpeg","width":10,)"
                                   R"("height":10}])",
                                   "announce 0 @0 0"}));
    EXPECT_EQ(h.wake, 10 * S);
    EXPECT_EQ(h.take(10 * S), (Messages{"announce 0 @30000000 10", "part 0 10"}));
    // Replaced by one more than 20 s ahead: the one held is cancelled
    h.view.album.pending = h.view.image(6, 40 * S, 10);
    EXPECT_EQ(h.take(11 * S), Messages{"cancel 0"});
    EXPECT_EQ(h.take(20 * S), (Messages{"announce 0 @40000000 10", "part 0 10"}));
    // Its time comes: the client shows it, so nothing is sent
    EXPECT_EQ(h.take(40 * S), Messages{});
    h.view.album = {*h.view.album.pending, std::nullopt};
    EXPECT_EQ(h.take(40 * S), Messages{});
    // Cancelled by the host while held: a cancel
    h.view.album.pending = h.view.image(7, 50 * S, 10);
    EXPECT_EQ(h.take(41 * S), (Messages{"announce 0 @50000000 10", "part 0 10"}));
    h.view.album.pending.reset();
    EXPECT_EQ(h.take(42 * S), Messages{"cancel 0"});
}

// messaging.md "server/activate": a stream/end before the activation that removes the role,
// which the marker takes; client/leave ends the stream until the client joins a group
TEST(ArtworkStreamSteps, EndsForARoleRemovalAndALeave) {
    Harness h;
    h.state(true, {ALBUM_10});
    h.take(0);
    // Control: with the role staying, the marker owes nothing
    EXPECT_EQ(h.take_end(1), Messages{});
    h.role_ahead = false;
    EXPECT_EQ(h.take_end(2), Messages{"end"});
    EXPECT_EQ(h.take(3), Messages{});
    h.stream.on_activation_written(false);
    h.role_ahead = true;
    h.stream.on_activation_written(true);
    // Added again: it waits for the client/state the activation requires
    EXPECT_EQ(h.take(4), Messages{});
    h.state(true, {ALBUM_10});
    EXPECT_EQ(h.take(5).size(), 2U);

    h.stream.set_left(true);
    EXPECT_EQ(h.take_end(6), Messages{"end"});
    // Still available: only a move or a return to available starts it again
    h.state(true, {ALBUM_10});
    EXPECT_EQ(h.take(7), Messages{});
    h.stream.set_left(false);
    EXPECT_EQ(h.take(8).size(), 2U);
    h.stream.set_left(true);
    h.take_end(9);
    h.state(false, {ALBUM_10});
    h.state(true, {ALBUM_10});
    EXPECT_EQ(h.take(10).size(), 2U);
}

}  // namespace
