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

/// @file test_source_decoder.cpp
/// @brief A routed source's feed and decoder: the formats it takes, PCM and FLAC decoded bit for
/// bit and Opus decoded, each block with its capture time, and the chunks that break the stream's
/// framing or are longer than 150 ms reported as protocol errors (roles/source/v1.md
/// "client-stream/start", "Source Audio Chunks (Binary)")

#include "crypto/base64url.h"
#include "dispatcher.h"
#include "frame_encoder.h"
#include "host_view.h"
#include "job_view.h"
#include "source_decoder.h"
#include <gtest/gtest.h>
#include <micro_flac/flac_encoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using sendspin::server::AudioFormat;
using sendspin::server::Codec;
using sendspin::server::InputFormat;
using sendspin::server::SampleFormat;
using sendspin::server::SourceDecoder;
using sendspin::server::SourceFault;
using sendspin::server::SourceFeed;
using sendspin::server::SourceItem;
using sendspin::server::test::job_view_of;
using Bytes = std::vector<uint8_t>;

// ==============================================================================
// The feed
// ==============================================================================

// A bounded FIFO: items come out in order with their bytes, across the storage's wrap, and a push
// past the items or the bytes it holds is refused
TEST(SourceFeed, HoldsItemsInOrderWithinItsBounds) {
    SourceFeed feed(10, 3);
    const auto push = [&](uint64_t stream, size_t size) {
        SourceItem item;
        item.kind = SourceItem::Kind::CHUNK;
        item.stream = stream;
        return feed.push(item, Bytes(size, static_cast<uint8_t>(stream)));
    };
    EXPECT_TRUE(push(1, 4));
    EXPECT_TRUE(push(2, 4));
    EXPECT_FALSE(push(3, 4)) << "past the bytes";
    EXPECT_TRUE(push(3, 2));
    EXPECT_FALSE(push(4, 1)) << "past the items";
    for (uint64_t expected = 1; expected <= 2; ++expected) {
        SourceItem item;
        std::span<const uint8_t> bytes;
        ASSERT_TRUE(feed.front(item, bytes));
        EXPECT_EQ(item.stream, expected);
        EXPECT_TRUE(
            std::all_of(bytes.begin(), bytes.end(), [&](uint8_t b) { return b == expected; }));
        feed.pop();
    }
    // Wraps to the start of the storage, behind the item left
    EXPECT_TRUE(push(5, 6));
    SourceItem item;
    std::span<const uint8_t> bytes;
    ASSERT_TRUE(feed.front(item, bytes));
    EXPECT_EQ(item.stream, 3U);
    feed.pop();
    ASSERT_TRUE(feed.front(item, bytes));
    EXPECT_EQ(item.stream, 5U);
    EXPECT_EQ(bytes.size(), 6U);
    feed.pop();
    EXPECT_TRUE(feed.empty());
    EXPECT_TRUE(push(6, 10)) << "an empty feed holds its whole storage";
}

// ==============================================================================
// Formats
// ==============================================================================

// What each announced format decodes to: 16 and 24 bits bit for bit, other depths as floats, Opus
// at libopus's rates as 16 bits, and nothing past what a GroupInput takes
TEST(SourceInputFormat, MapsEachStreamToAnInputFormat) {
    struct Row {
        const char* name;
        AudioFormat format;
        std::optional<SampleFormat> sample;
    };
    const Row rows[] = {
        {"Control: pcm 16", {Codec::PCM, 2, 48000, 16}, SampleFormat::S16},
        {"pcm 24", {Codec::PCM, 2, 48000, 24}, SampleFormat::S24},
        {"pcm 32", {Codec::PCM, 2, 48000, 32}, SampleFormat::F32},
        {"pcm 8", {Codec::PCM, 2, 48000, 8}, std::nullopt},
        {"flac 16", {Codec::FLAC, 1, 44100, 16}, SampleFormat::S16},
        {"flac 24", {Codec::FLAC, 2, 96000, 24}, SampleFormat::S24},
        {"flac 20", {Codec::FLAC, 2, 48000, 20}, SampleFormat::F32},
        {"flac 33", {Codec::FLAC, 2, 48000, 33}, std::nullopt},
        {"opus 48 kHz", {Codec::OPUS, 2, 48000, 0}, SampleFormat::S16},
        {"opus 44.1 kHz", {Codec::OPUS, 2, 44100, 0}, std::nullopt},
        {"opus 3 channels", {Codec::OPUS, 3, 48000, 0}, std::nullopt},
        {"33 channels", {Codec::PCM, 33, 48000, 16}, std::nullopt},
        {"past 384 kHz", {Codec::PCM, 2, 384001, 16}, std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const std::optional<InputFormat> input = sendspin::server::source_input_format(row.format);
        ASSERT_EQ(input.has_value(), row.sample.has_value());
        if (input) {
            EXPECT_EQ(input->sample_format, *row.sample);
            EXPECT_EQ(input->sample_rate, row.format.sample_rate);
            EXPECT_EQ(input->channels, row.format.channels);
        }
    }
}

// ==============================================================================
// The decoder
// ==============================================================================

/// What the decoder handed on
class RecordingSink : public sendspin::server::SourcePcmSink {
public:
    struct Write {
        Bytes pcm;
        size_t frames;
        int64_t capture_us;
        int64_t rx_us;
    };

    bool begin(const InputFormat& format) override {
        this->formats.push_back(format);
        this->frame_bytes =
            size_t{format.channels} * (format.sample_format == SampleFormat::S16   ? 2
                                       : format.sample_format == SampleFormat::S24 ? 3
                                                                                   : 4);
        return this->accept;
    }

    void write(const uint8_t* pcm, size_t frames, int64_t capture_us, int64_t rx_us) override {
        this->writes.push_back(
            {Bytes(pcm, pcm + frames * this->frame_bytes), frames, capture_us, rx_us});
    }

    void end() override {
        ++this->ends;
    }

    bool accept{true};
    size_t frame_bytes{0};
    std::vector<InputFormat> formats;
    std::vector<Write> writes;
    int ends{0};
};

/// A decoder over its own feed, recording into sink
struct Rig {
    Rig() {
        auto owned = std::make_unique<RecordingSink>();
        this->sink = owned.get();
        this->decoder = std::make_unique<SourceDecoder>(this->feed, std::move(owned), nullptr);
    }

    void start(const AudioFormat& format, const Bytes& header = {}, uint64_t stream = 1) {
        SourceItem item;
        item.kind = SourceItem::Kind::START;
        item.stream = stream;
        item.format = format;
        ASSERT_TRUE(this->feed->push(item, header));
        this->run();
    }

    void chunk(const Bytes& audio, int64_t capture_us, uint64_t stream = 1) {
        SourceItem item;
        item.kind = SourceItem::Kind::CHUNK;
        item.stream = stream;
        item.capture_us = capture_us;
        item.rx_us = capture_us + 1;
        ASSERT_TRUE(this->feed->push(item, audio));
        this->run();
    }

    void end(uint64_t stream = 1) {
        SourceItem item;
        item.kind = SourceItem::Kind::END;
        item.stream = stream;
        ASSERT_TRUE(this->feed->push(item, {}));
        this->run();
    }

    void run() {
        EXPECT_FALSE(this->decoder->run()) << "one item per run, none left";
    }

    std::vector<SourceFault::Kind> faults() {
        std::vector<SourceFault::Kind> kinds;
        for (const SourceFault& fault : this->feed->take_faults()) {
            kinds.push_back(fault.kind);
        }
        return kinds;
    }

    std::shared_ptr<SourceFeed> feed = std::make_shared<SourceFeed>(1 << 20, 16);
    RecordingSink* sink{nullptr};
    std::unique_ptr<SourceDecoder> decoder;
};

/// Stereo 16-bit frames whose samples count up from first
Bytes pcm16(size_t frames, int16_t first = 1) {
    Bytes out;
    for (size_t i = 0; i < frames * 2; ++i) {
        const auto value = static_cast<uint16_t>(first + static_cast<int16_t>(i));
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8));
    }
    return out;
}

// roles/source/v1.md "PCM Encoding Convention": little-endian, 24-bit packed; taken as written,
// with the capture time it came with, and 32-bit samples as floats at full scale
TEST(SourceDecoder, PassesPcmOnBitForBit) {
    Rig r;
    r.start({Codec::PCM, 2, 48000, 16});
    const Bytes audio = pcm16(960);
    r.chunk(audio, 5000);
    ASSERT_EQ(r.sink->writes.size(), 1U);
    EXPECT_EQ(r.sink->writes[0].pcm, audio);
    EXPECT_EQ(r.sink->writes[0].capture_us, 5000);
    EXPECT_EQ(r.sink->writes[0].rx_us, 5001);

    Rig wide;
    wide.start({Codec::PCM, 1, 48000, 32});
    wide.chunk({0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0xC0}, 0);
    ASSERT_EQ(wide.sink->writes.size(), 1U);
    float samples[2];
    std::memcpy(samples, wide.sink->writes[0].pcm.data(), sizeof(samples));
    EXPECT_EQ(samples[0], 0.5F);
    EXPECT_EQ(samples[1], -0.5F);
    EXPECT_TRUE(r.faults().empty());
}

/// FLAC frames of 20 ms at 48 kHz, stereo 16-bit, of audio, and the stream's header
struct FlacStream {
    Bytes header;
    std::vector<Bytes> frames;
};

FlacStream encode_flac(const Bytes& audio) {
    const sendspin::server::sched::AudioFormat format{sendspin::server::sched::Codec::FLAC, 48000,
                                                      2, 16};
    FlacStream stream;
    std::string header = *sendspin::server::codec_header(format);
    std::replace(header.begin(), header.end(), '+', '-');
    std::replace(header.begin(), header.end(), '/', '_');
    stream.header = *sendspin::server::b64url_decode(header);
    const std::unique_ptr<sendspin::server::FrameEncoder> encoder =
        sendspin::server::FrameEncoder::create(format);
    constexpr size_t FRAME_BYTES = 960 * 4;
    for (size_t at = 0; at < audio.size(); at += FRAME_BYTES) {
        Bytes payload;
        EXPECT_TRUE(
            encoder->encode(std::span(audio).subspan(at, FRAME_BYTES), SampleFormat::S16, payload));
        stream.frames.push_back(payload);
    }
    return stream;
}

/// FLAC frames of block frames each, of stereo 16-bit 48 kHz audio, and a header announcing
/// that block size
FlacStream encode_flac_blocks(const Bytes& audio, uint32_t block) {
    micro_flac::FLACEncoderOptions options;
    options.block_size = block;
    micro_flac::FLACEncoder encoder({48000, 2, 16}, options);
    FlacStream stream;
    stream.header.resize(micro_flac::FLACEncoder::HEADER_BYTES);
    size_t written = 0;
    EXPECT_EQ(encoder.write_header(stream.header.data(), stream.header.size(), written),
              micro_flac::FLAC_ENCODER_SUCCESS);
    Bytes out(encoder.get_max_output_bytes());
    size_t at = 0;
    while (at < audio.size()) {
        size_t used = 0;
        EXPECT_EQ(encoder.encode(audio.data() + at, audio.size() - at, out.data(), out.size(), used,
                                 written),
                  micro_flac::FLAC_ENCODER_SUCCESS);
        if (used == 0) {
            break;
        }
        stream.frames.emplace_back(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(written));
        at += used;
    }
    return stream;
}

// roles/source/v1.md "Codec framing": one or more complete FLAC frames per chunk, after the
// codec_header; decoded bit for bit, each chunk's frames in one write with the chunk's capture time
TEST(SourceDecoder, DecodesFlacBitForBit) {
    const Bytes audio = pcm16(3 * 960);
    const FlacStream stream = encode_flac(audio);
    ASSERT_EQ(stream.frames.size(), 3U);
    Rig r;
    r.start({Codec::FLAC, 2, 48000, 16}, stream.header);
    Bytes two = stream.frames[0];
    two.insert(two.end(), stream.frames[1].begin(), stream.frames[1].end());
    r.chunk(two, 100000);
    r.chunk(stream.frames[2], 140000);
    EXPECT_TRUE(r.faults().empty());
    ASSERT_EQ(r.sink->writes.size(), 2U);
    Bytes decoded;
    for (const RecordingSink::Write& write : r.sink->writes) {
        decoded.insert(decoded.end(), write.pcm.begin(), write.pcm.end());
    }
    EXPECT_EQ(decoded, audio);
    EXPECT_EQ(r.sink->writes[0].frames, 2U * 960);
    EXPECT_EQ(r.sink->writes[0].capture_us, 100000);
    EXPECT_EQ(r.sink->writes[1].capture_us, 140000);
}

// roles/source/v1.md "Codec framing": exactly one Opus packet per chunk, the decoder configured
// from the announced rate and channels
TEST(SourceDecoder, DecodesOpusPackets) {
    if (!sendspin::server::source_opus_available()) {
        GTEST_SKIP() << "built without Opus";
    }
    const sendspin::server::sched::AudioFormat format{sendspin::server::sched::Codec::OPUS, 48000,
                                                      2, 16};
    const std::unique_ptr<sendspin::server::FrameEncoder> encoder =
        sendspin::server::FrameEncoder::create(format);
    Bytes packet;
    ASSERT_TRUE(encoder->encode(pcm16(960), SampleFormat::S16, packet));
    Rig r;
    r.start({Codec::OPUS, 2, 48000, 0});
    ASSERT_EQ(r.sink->formats.size(), 1U);
    EXPECT_EQ(r.sink->formats[0].sample_format, SampleFormat::S16);
    r.chunk(packet, 0);
    EXPECT_TRUE(r.faults().empty());
    ASSERT_EQ(r.sink->writes.size(), 1U);
    EXPECT_EQ(r.sink->writes[0].frames, 960U);
    // An Opus packet is at least one byte (RFC 6716 section 3): an empty chunk is not one
    r.chunk({}, 20000);
    EXPECT_EQ(r.faults(), std::vector{SourceFault::Kind::PROTOCOL});
}

// roles/source/v1.md "Codec framing" and "Source Audio Chunks (Binary)": a chunk that is not whole
// codec units, or longer than 150 ms, is a protocol error; exactly 150 ms is not
TEST(SourceDecoder, ReportsChunksThatBreakTheFramingOrTheDuration) {
    const FlacStream flac = encode_flac(pcm16(960));
    struct Row {
        const char* name;
        AudioFormat format;
        Bytes header;
        Bytes chunk;
        bool fault;
    };
    const Row rows[] = {
        {"Control: 150 ms of PCM", {Codec::PCM, 2, 48000, 16}, {}, pcm16(7200), false},
        {"past 150 ms of PCM", {Codec::PCM, 2, 48000, 16}, {}, pcm16(7201), true},
        {"part of a PCM frame", {Codec::PCM, 2, 48000, 16}, {}, Bytes(6, 1), true},
        {"Control: a FLAC frame", {Codec::FLAC, 2, 48000, 16}, flac.header, flac.frames[0], false},
        {"part of a FLAC frame",
         {Codec::FLAC, 2, 48000, 16},
         flac.header,
         Bytes(flac.frames[0].begin(), flac.frames[0].end() - 3),
         true},
        {"not FLAC", {Codec::FLAC, 2, 48000, 16}, flac.header, Bytes(64, 0x55), true},
        // "Codec framing": PCM is any whole number of frames, none among them; FLAC one or more
        // frames
        {"Control: an empty PCM chunk", {Codec::PCM, 2, 48000, 16}, {}, {}, false},
        {"an empty FLAC chunk", {Codec::FLAC, 2, 48000, 16}, flac.header, {}, true},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        Rig r;
        r.start(row.format, row.header);
        r.chunk(row.chunk, 0);
        EXPECT_EQ(r.faults(), row.fault ? std::vector{SourceFault::Kind::PROTOCOL}
                                        : std::vector<SourceFault::Kind>{});
        if (row.chunk.empty()) {
            EXPECT_TRUE(r.sink->writes.empty()) << "nothing to write";
        }
        if (row.fault) {
            // Nothing past the fault until the next stream starts
            r.chunk(pcm16(1), 0);
            EXPECT_TRUE(r.faults().empty());
        }
    }
}

// A header allowing frames longer than a chunk may be is taken, the decoder's buffers stopping at a
// chunk's length (150 ms): its frames up to that decode, and a longer one is a protocol error. A
// header that ends at STREAMINFO without marking it the last metadata block is taken as one that
// does.
TEST(SourceDecoder, BoundsFlacFramesByTheLongestChunk) {
    const Bytes audio = pcm16(8000);
    const FlacStream long_blocks = encode_flac_blocks(audio, 8000);
    const FlacStream short_blocks = encode_flac_blocks(audio, 1000);
    ASSERT_EQ(long_blocks.frames.size(), 1U);
    ASSERT_EQ(short_blocks.frames.size(), 8U);

    Rig r;
    r.start({Codec::FLAC, 2, 48000, 16}, long_blocks.header);
    EXPECT_TRUE(r.faults().empty()) << "the header is taken";
    Bytes chunk;
    for (size_t i = 0; i < 7; ++i) {
        chunk.insert(chunk.end(), short_blocks.frames[i].begin(), short_blocks.frames[i].end());
    }
    r.chunk(chunk, 0);
    EXPECT_TRUE(r.faults().empty()) << "Control: 7000 frames in 1000-frame blocks";
    ASSERT_EQ(r.sink->writes.size(), 1U);
    EXPECT_EQ(r.sink->writes[0].pcm, Bytes(audio.begin(), audio.begin() + (7000 * 4)));
    r.chunk(long_blocks.frames[0], 200000);
    EXPECT_EQ(r.faults(), std::vector{SourceFault::Kind::PROTOCOL}) << "an 8000-frame block";

    Bytes not_last = short_blocks.header;
    not_last[4] &= 0x7F;
    Rig unmarked;
    unmarked.start({Codec::FLAC, 2, 48000, 16}, not_last);
    EXPECT_TRUE(unmarked.faults().empty());
    unmarked.chunk(short_blocks.frames[0], 0);
    EXPECT_TRUE(unmarked.faults().empty());
    EXPECT_EQ(unmarked.sink->writes.size(), 1U);
}

// A chunk that decodes in parts (32-bit PCM converted to floats, FLAC frame by frame) is judged
// once against its input's write jitter, as a whole: chunks far longer than a jitter of 0 arriving
// at a steady pace reach the live input whole
TEST(SourceDecoder, WritesEachChunkWholeIntoALiveInput) {
    struct Row {
        const char* name;
        AudioFormat format;
        Bytes header;
        std::vector<Bytes> chunks;
        uint64_t frames;
    };
    const Bytes wide(size_t{4800} * 2 * 4, 0x11);
    const FlacStream flac = encode_flac(pcm16(15 * 960));
    std::vector<Bytes> flac_chunks;
    for (size_t at = 0; at < flac.frames.size(); at += 5) {
        Bytes chunk;
        for (size_t i = at; i < at + 5; ++i) {
            chunk.insert(chunk.end(), flac.frames[i].begin(), flac.frames[i].end());
        }
        flac_chunks.push_back(chunk);
    }
    const Row rows[] = {
        {"32-bit PCM in 100 ms chunks", {Codec::PCM, 2, 48000, 32}, {}, {wide, wide, wide}, 14400},
        {"FLAC in 100 ms chunks of 20 ms frames",
         {Codec::FLAC, 2, 48000, 16},
         flac.header,
         flac_chunks,
         14400},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const auto dispatcher = std::make_shared<sendspin::server::Dispatcher>();
        sendspin::server::InputOptions options{{48000, 2, SampleFormat::S16}};
        options.kind = sendspin::server::InputKind::LIVE;
        const auto view = std::make_shared<sendspin::server::HostView>(
            sendspin::server::GroupId{1}, sendspin::server::InputId{2}, options,
            sendspin::server::GroupParams{300000, 0, 2000000}, dispatcher, nullptr);
        const auto feed = std::make_shared<SourceFeed>(1 << 20, 16);
        SourceDecoder decoder(feed,
                              std::make_unique<sendspin::server::LiveSourceSink>(
                                  view, 0, sendspin::server::ResamplerQuality::MEDIUM),
                              nullptr);
        SourceItem item;
        item.format = row.format;
        ASSERT_TRUE(feed->push(item, row.header));
        int64_t capture_us = 1000000;
        for (const Bytes& chunk : row.chunks) {
            item.kind = SourceItem::Kind::CHUNK;
            item.capture_us = capture_us;
            item.rx_us = capture_us + 105000;
            ASSERT_TRUE(feed->push(item, chunk));
            capture_us += 100000;
        }
        while (decoder.run()) {}
        EXPECT_TRUE(feed->take_faults().empty());
        EXPECT_EQ(job_view_of(*view).head, row.frames);
        dispatcher->finish();
        dispatcher->join();
    }
}

// A stream the decoder or the input cannot take is reported, its chunks dropped; items of a
// stream other than the one started are ignored; the end of the stream started ends the sink's
TEST(SourceDecoder, FollowsTheStreamStarted) {
    Rig refused;
    refused.sink->accept = false;
    refused.start({Codec::PCM, 2, 48000, 16});
    EXPECT_EQ(refused.faults(), std::vector{SourceFault::Kind::UNSUPPORTED});
    refused.chunk(pcm16(10), 0);
    EXPECT_TRUE(refused.sink->writes.empty());

    Rig r;
    r.start({Codec::PCM, 2, 48000, 16}, {}, 2);
    r.chunk(pcm16(10), 0, 1);
    r.end(1);
    EXPECT_TRUE(r.sink->writes.empty());
    EXPECT_EQ(r.sink->ends, 0);
    r.chunk(pcm16(10), 0, 2);
    EXPECT_EQ(r.sink->writes.size(), 1U);
    r.end(2);
    EXPECT_EQ(r.sink->ends, 1);
    EXPECT_TRUE(r.faults().empty());
}

}  // namespace
