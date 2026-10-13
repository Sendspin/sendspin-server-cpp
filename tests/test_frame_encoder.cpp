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

/// @file test_frame_encoder.cpp
/// @brief An output format's shape and codec_header, and the quantize stage and encoders of one
/// pipeline instance

#include "flac_reader.h"
#include "frame_encoder.h"
#include "opus_reader.h"
#include "pcm_convert.h"
#include <gtest/gtest.h>
#include <micro_flac/flac_encoder.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using sendspin::server::FrameEncoder;
using sendspin::server::SampleFormat;
using sendspin::server::test::FlacReader;
namespace sched = sendspin::server::sched;
using Bytes = std::vector<uint8_t>;

constexpr sched::AudioFormat FLAC48_16{sched::Codec::FLAC, 48000, 2, 16};
constexpr sched::AudioFormat FLAC44_24{sched::Codec::FLAC, 44100, 2, 24};

/// The bound the encoder gives for the format at its frame length
int64_t flac_bound(const sched::AudioFormat& format) {
    micro_flac::FLACEncoderOptions options;
    options.block_size = format.sample_rate / 50;
    options.lpc = true;
    options.max_lpc_order = 12;
    options.lpc_stereo_search = true;
    options.max_rice_partition_order = 6;
    const micro_flac::FLACEncoder encoder(
        micro_flac::PcmFormat{format.sample_rate, format.channels, format.bit_depth}, options);
    return static_cast<int64_t>(encoder.get_max_output_bytes());
}

/// count stereo frames of float music-like input: two tones and a little noise, past full scale
/// now and then so the quantize stage clips
Bytes float_signal(uint32_t rate, size_t count, size_t from) {
    Bytes out(count * 2 * sizeof(float));
    uint32_t noise = 12345;
    for (size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(from + i) / rate;
        noise = (noise * 1103515245U) + 12345U;
        const double jitter = (static_cast<double>(noise >> 16) / 65536.0 - 0.5) * 0.01;
        const auto left =
            static_cast<float>((1.1 * std::sin(2 * std::numbers::pi * 440 * t)) + jitter);
        const auto right =
            static_cast<float>((0.5 * std::sin(2 * std::numbers::pi * 1250 * t)) - jitter);
        std::memcpy(&out[i * 8], &left, sizeof(float));
        std::memcpy(&out[(i * 8) + 4], &right, sizeof(float));
    }
    return out;
}

// B_f is exact for PCM, the encoder's bound for FLAC and the largest one-frame packet for Opus; the
// quantize stage writes whole bytes, so PCM takes 16, 24 and 32 bits and FLAC 16 and 24; Opus takes
// its five rates (RFC 6716 section 2) and one or two channels, whatever the bit depth (spec
// player/v1.md: ignored for `opus`)
TEST(OutputShape, GivesEachEncodersFrameBound) {
    struct Row {
        const char* name;
        sched::AudioFormat format;
        bool available;
        int64_t max_frame_bytes;
    };
    const Row rows[] = {
        {"Control: PCM 48/16/2", {sched::Codec::PCM, 48000, 2, 16}, true, 960 * 4},
        {"PCM 44.1/24/2", {sched::Codec::PCM, 44100, 2, 24}, true, 882 * 6},
        {"PCM 48/32/1", {sched::Codec::PCM, 48000, 1, 32}, true, 960 * 4},
        {"PCM at 20 bits", {sched::Codec::PCM, 48000, 2, 20}, false, 960 * 6},
        {"FLAC 48/16/2", FLAC48_16, true, flac_bound(FLAC48_16)},
        {"FLAC 44.1/24/2", FLAC44_24, true, flac_bound(FLAC44_24)},
        {"FLAC at 32 bits", {sched::Codec::FLAC, 48000, 2, 32}, false, 0},
        {"FLAC at 20 bits", {sched::Codec::FLAC, 48000, 2, 20}, false, 0},
        {"FLAC at 8 bits", {sched::Codec::FLAC, 48000, 2, 8}, false, 0},
        {"Opus 48/2", {sched::Codec::OPUS, 48000, 2, 16}, true, 1276},
        {"Opus 8/1 at 24 bits", {sched::Codec::OPUS, 8000, 1, 24}, true, 1276},
        {"Opus at 44.1 kHz", {sched::Codec::OPUS, 44100, 2, 16}, false, 1276},
        {"Opus with three channels", {sched::Codec::OPUS, 48000, 3, 16}, false, 1276},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const sched::FormatShape shape = sendspin::server::output_shape(row.format);
        EXPECT_EQ(shape.format, row.format);
        EXPECT_EQ(shape.encoder_available, row.available);
        EXPECT_EQ(shape.max_frame_bytes, row.max_frame_bytes);
        EXPECT_EQ(shape.layout.samples, row.format.sample_rate / 50);
        EXPECT_EQ(static_cast<bool>(FrameEncoder::create(row.format)), row.available);
    }
    // A FLAC bound is a real one: above the PCM frame of the same format
    EXPECT_GT(flac_bound(FLAC48_16), 960 * 4);
    // 11,025 Hz FLAC frames are two cells, 441 samples
    const sched::FormatShape slow =
        sendspin::server::output_shape({sched::Codec::FLAC, 11025, 2, 16});
    EXPECT_EQ(slow.layout.cells, 2U);
    EXPECT_EQ(slow.layout.samples, 441U);
    EXPECT_TRUE(slow.encoder_available);
    // FLAC deeper than 24 bits is never producible, whatever the shape says
    sched::FormatShape deep = sendspin::server::output_shape(FLAC48_16);
    deep.format.bit_depth = 32;
    EXPECT_TRUE(sched::producible(sendspin::server::output_shape(FLAC48_16), 100000));
    EXPECT_FALSE(sched::producible(deep, 100000));
}

// spec player/v1.md "Codec framing": FLAC's codec_header is `fLaC` and STREAMINFO in standard
// Base64 with padding, for the stream's rate, channels, depth and block size; PCM has none
TEST(CodecHeader, IsTheStreaminfoOfTheFormatInStandardBase64) {
    for (const sched::AudioFormat& format : {FLAC48_16, FLAC44_24}) {
        SCOPED_TRACE(format.sample_rate);
        const std::optional<std::string> header = sendspin::server::codec_header(format);
        ASSERT_TRUE(header);
        // 42 bytes, a whole number of 3-byte groups: 56 characters and no padding
        EXPECT_EQ(header->size(), 56U);
        FlacReader reader;
        ASSERT_TRUE(reader.open(*header));
        ASSERT_EQ(reader.header.size(), 42U);
        EXPECT_EQ(std::string(reader.header.begin(), reader.header.begin() + 4), "fLaC");
        // The only metadata block
        EXPECT_EQ(reader.header[4] & 0x80, 0x80);
        EXPECT_EQ(reader.info().sample_rate(), format.sample_rate);
        EXPECT_EQ(reader.info().num_channels(), format.channels);
        EXPECT_EQ(reader.info().bits_per_sample(), format.bit_depth);
        EXPECT_EQ(reader.info().min_block_size(), format.sample_rate / 50);
        EXPECT_EQ(reader.info().max_block_size(), format.sample_rate / 50);
    }
    EXPECT_FALSE(sendspin::server::codec_header({sched::Codec::PCM, 48000, 2, 16}));
    EXPECT_FALSE(sendspin::server::codec_header({sched::Codec::FLAC, 48000, 2, 32}));
}

// The PCM encoder's frame is the quantize stage's output
TEST(FrameEncoderPcm, IsTheQuantizedInput) {
    const sched::AudioFormat format{sched::Codec::PCM, 48000, 2, 24};
    const std::unique_ptr<FrameEncoder> encoder = FrameEncoder::create(format);
    ASSERT_TRUE(encoder);
    const Bytes input = float_signal(48000, 960, 0);
    Bytes expected(960 * 2 * 3);
    sendspin::server::convert_pcm(input, SampleFormat::F32, 24, expected);
    Bytes payload;
    ASSERT_TRUE(encoder->encode(input, SampleFormat::F32, payload));
    EXPECT_EQ(payload, expected);
}

// One FLAC frame per full cell, numbered from 0 in the stream; reset() restarts the numbers;
// finish() encodes a short last frame and ends the stream. Every frame decodes to the quantize
// stage's samples, bit for bit, through one decoder.
TEST(FrameEncoderFlac, EncodesCellsThatDecodeToTheQuantizedInput) {
    struct Row {
        const char* name;
        sched::AudioFormat format;
        SampleFormat input;
    };
    const Row rows[] = {
        {"Control: float to 16 bits", FLAC48_16, SampleFormat::F32},
        {"float to 24 bits", FLAC44_24, SampleFormat::F32},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const uint32_t block = row.format.sample_rate / 50;
        const size_t out_bytes = row.format.bit_depth / 8;
        const std::unique_ptr<FrameEncoder> encoder = FrameEncoder::create(row.format);
        ASSERT_TRUE(encoder);
        FlacReader reader;
        ASSERT_TRUE(reader.open(*sendspin::server::codec_header(row.format)));

        // frames per stream: a full one, then the rest of its cells
        const auto check = [&](size_t position, size_t frames, uint64_t number) {
            const Bytes input = float_signal(row.format.sample_rate, frames, position);
            Bytes expected(frames * 2 * out_bytes);
            sendspin::server::convert_pcm(input, row.input, row.format.bit_depth, expected);
            Bytes payload;
            ASSERT_TRUE(encoder->encode(input, row.input, payload));
            EXPECT_LT(payload.size(), expected.size());
            const auto decoded = reader.decode(payload);
            ASSERT_TRUE(decoded);
            ASSERT_EQ(decoded->size(), 1U);
            EXPECT_EQ(decoded->front().number, number);
            EXPECT_EQ(decoded->front().pcm, expected);
        };
        check(0, block, 0);
        check(block, block, 1);
        check(2 * block, block, 2);
        // An epoch start
        encoder->reset();
        check(5 * block, block, 0);
        // A drained epoch's last frame: 100 samples
        check(6 * block, 100, 1);
        // The stream ended until the next reset
        Bytes payload;
        const Bytes more = float_signal(row.format.sample_rate, block, 0);
        EXPECT_FALSE(encoder->encode(more, row.input, payload));
        EXPECT_TRUE(payload.empty());
        encoder->reset();
        check(0, block, 0);
    }
}

// spec player/v1.md "Codec framing": one Opus packet per frame, which decodes to the frame's 20 ms
// of the input, behind the encoder's lookahead: libopus reports encoder_delay_samples(), so the
// pipelines' compensation matches the encoder's. Lossy, so compared by SNR. reset() starts a stream
// again, and a short last frame is padded to a whole packet.
TEST(OpusFrameEncoder, DecodesToTheInputBehindItsLookahead) {
    using sendspin::server::test::OpusReader;
    for (const uint32_t rate : {48000U, 24000U, 8000U}) {
        SCOPED_TRACE(rate);
        const sched::AudioFormat format{sched::Codec::OPUS, rate, 2, 24};
        const std::unique_ptr<FrameEncoder> encoder = FrameEncoder::create(format);
        ASSERT_TRUE(encoder);
        EXPECT_FALSE(sendspin::server::codec_header(format));
        const uint32_t delay = sendspin::server::encoder_delay_samples(format);
        EXPECT_EQ(encoder->delay_samples(), delay);
        EXPECT_EQ(delay, rate / 400);
        EXPECT_EQ(sendspin::server::quantized_depth(format), 16U);
        const uint32_t block = rate / 50;
        constexpr size_t FRAMES = 50;
        const Bytes input = sendspin::server::test::tone_frames(0, FRAMES * block, rate);
        OpusReader reader(rate, 2);
        std::vector<int16_t> decoded;
        for (size_t k = 0; k < FRAMES; ++k) {
            Bytes payload;
            const std::span<const uint8_t> frame(input.data() + (k * block * 4), block * 4);
            ASSERT_TRUE(encoder->encode(frame, SampleFormat::S16, payload));
            EXPECT_LE(payload.size(), 1276U);
            const std::optional<std::vector<int16_t>> samples = reader.decode(payload);
            ASSERT_TRUE(samples);
            ASSERT_EQ(samples->size(), size_t{block} * 2);
            decoded.insert(decoded.end(), samples->begin(), samples->end());
        }
        std::vector<int16_t> reference(input.size() / 2);
        std::memcpy(reference.data(), input.data(), input.size());
        // From the fifth frame on, clear of the stream's start
        const std::span<const int16_t> tail(reference.data() + (5 * block * 2),
                                            reference.size() - (6 * block * 2));
        const std::span<const int16_t> heard(decoded.data() + (5 * block * 2),
                                             decoded.size() - (5 * block * 2));
        EXPECT_EQ(sendspin::server::test::best_lag(tail, heard, int64_t{4} * delay, 2),
                  int64_t{2} * delay);
        // 8 kHz has the least bitrate
        const double aligned = sendspin::server::test::snr_db(tail, heard, int64_t{2} * delay);
        EXPECT_GT(aligned, rate == 8000 ? 10.0 : 20.0);
        // Control: without the lookahead the decoded audio is far off
        EXPECT_LT(sendspin::server::test::snr_db(tail, heard), aligned - 6.0);

        // A short last frame is a whole packet, and reset() starts a new stream
        Bytes payload;
        ASSERT_TRUE(encoder->encode(std::span(input.data(), 100 * 4), SampleFormat::S16, payload));
        const std::optional<std::vector<int16_t>> last = reader.decode(payload);
        ASSERT_TRUE(last);
        EXPECT_EQ(last->size(), size_t{block} * 2);
        encoder->reset();
        ASSERT_TRUE(
            encoder->encode(std::span(input.data(), block * 4), SampleFormat::S16, payload));
        EXPECT_FALSE(payload.empty());
    }
}

/// The level in dB of 16-bit samples
double level_db(std::span<const int16_t> samples) {
    double sum = 0;
    for (const int16_t sample : samples) {
        sum += static_cast<double>(sample) * sample;
    }
    return 10.0 * std::log10(sum / static_cast<double>(samples.size()));
}

// spec player/v1.md "No startup warble": a decoder whose state is from another point in the
// stream decodes each packet at the input's level from the first on. Joining: a player joins an
// instance that has been encoding, with a new decoder. Seek: the instance's encoder starts a new
// stream at another position, while the player's decoder keeps its state across stream/clear.
TEST(OpusFrameEncoder, DecodesAtTheInputsLevelFromAnyPacket) {
    using sendspin::server::test::OpusReader;
    constexpr uint32_t RATE = 48000;
    constexpr uint32_t BLOCK = RATE / 50;
    const sched::AudioFormat format{sched::Codec::OPUS, RATE, 2, 16};
    const std::unique_ptr<FrameEncoder> encoder = FrameEncoder::create(format);
    ASSERT_TRUE(encoder);
    const auto encode = [&](uint64_t position) {
        const Bytes input = sendspin::server::test::tone_frames(position, BLOCK, RATE);
        Bytes payload;
        EXPECT_TRUE(encoder->encode(input, SampleFormat::S16, payload));
        return payload;
    };
    // The tones' level, which every frame's input has
    const Bytes reference_bytes = sendspin::server::test::tone_frames(0, RATE, RATE);
    std::vector<int16_t> reference(reference_bytes.size() / 2);
    std::memcpy(reference.data(), reference_bytes.data(), reference_bytes.size());
    const double reference_db = level_db(reference);
    const auto heard_db = [&](OpusReader& reader, const Bytes& packet) {
        const std::optional<std::vector<int16_t>> samples = reader.decode(packet);
        EXPECT_TRUE(samples);
        return samples ? level_db(*samples) : 0.0;
    };

    OpusReader first(RATE, 2);
    OpusReader joining(RATE, 2);
    for (uint64_t k = 0; k < 20; ++k) {
        const Bytes packet = encode(k * BLOCK);
        const double level = heard_db(first, packet);
        // The stream's first packet holds the encoder's lookahead of silence
        if (k > 0) {
            EXPECT_NEAR(level, reference_db, 1.0) << "frame " << k;
        }
        if (k >= 10) {
            EXPECT_NEAR(heard_db(joining, packet), reference_db, 1.0) << "joined, frame " << k;
        }
    }
    // A seek: the encoder starts at a position half a tone period off, its lookahead ahead of the
    // first frame's, as the pipeline gives it
    encoder->reset();
    const uint64_t seek = 100000;
    const uint32_t lead = encoder->delay_samples();
    Bytes primed;
    {
        const Bytes input = sendspin::server::test::tone_frames(seek - lead, BLOCK, RATE);
        ASSERT_TRUE(encoder->encode(input, SampleFormat::S16, primed));
    }
    EXPECT_NEAR(heard_db(first, primed), reference_db, 1.0) << "seek, frame 0";
    for (uint64_t k = 1; k < 10; ++k) {
        EXPECT_NEAR(heard_db(first, encode(seek - lead + (k * BLOCK))), reference_db, 1.0)
            << "seek, frame " << k;
    }
}

// The packets carry 64 kbit/s per channel, so even a mono stream's are at the 160 bytes
// sendspin-cpp designs its receive path for
TEST(OpusFrameEncoder, CarriesSixtyFourKilobitsPerChannel) {
    constexpr uint32_t RATE = 48000;
    constexpr uint32_t BLOCK = RATE / 50;
    for (const uint8_t channels : {uint8_t{1}, uint8_t{2}}) {
        SCOPED_TRACE(int{channels});
        const std::unique_ptr<FrameEncoder> encoder =
            FrameEncoder::create({sched::Codec::OPUS, RATE, channels, 16});
        ASSERT_TRUE(encoder);
        const Bytes stereo = sendspin::server::test::tone_frames(0, size_t{BLOCK} * 50, RATE);
        size_t bytes = 0;
        for (size_t k = 0; k < 50; ++k) {
            Bytes input;
            for (size_t i = 0; i < BLOCK; ++i) {
                const size_t at = ((k * BLOCK) + i) * 4;
                input.insert(input.end(), stereo.begin() + static_cast<std::ptrdiff_t>(at),
                             stereo.begin() + static_cast<std::ptrdiff_t>(at + (2 * channels)));
            }
            Bytes payload;
            ASSERT_TRUE(encoder->encode(input, SampleFormat::S16, payload));
            bytes += payload.size();
        }
        // 50 packets a second: 160 bytes per channel per packet at 64 kbit/s
        EXPECT_NEAR(static_cast<double>(bytes) / 50, 160.0 * channels, 16.0 * channels);
    }
}

}  // namespace
