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

/// @file test_audio_source.cpp
/// @brief server_cli's audio files read as they play: FLAC files encoded here with micro-flac's
/// encoder decode to the samples encoded, from any frame, with and without a seek table; WAV
/// files read from any frame; neither is held in memory; and the files neither takes are refused

#include "audio_source.h"
#include "temp_dir.h"
#include <gtest/gtest.h>
#include <micro_flac/flac_encoder.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using sendspin::server::InputFormat;
using sendspin::server::SampleFormat;
using sendspin::server::examples::AudioInfo;
using sendspin::server::examples::AudioSource;
using sendspin::server::examples::frame_bytes;
using sendspin::server::examples::open_audio_file;
using sendspin::server::examples::OpenedAudio;
using sendspin::server::test::TempDir;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t RATE = 48000;
constexpr size_t STREAMINFO_HEADER_AT = 4;
constexpr uint8_t LAST_BLOCK = 0x80;
constexpr uint8_t SEEKTABLE = 3;

/// frames of samples that differ from frame to frame and channel to channel, over the depth's
/// whole range, packed little-endian in whole bytes as micro-flac packs them
Bytes samples(uint32_t frames, uint32_t channels, uint32_t bits) {
    const uint32_t bytes = (bits + 7) / 8;
    Bytes out;
    out.reserve(size_t{frames} * channels * bytes);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint32_t channel = 0; channel < channels; ++channel) {
            const uint32_t value = (frame * 7919U + channel * 104729U + 13U) * 2654435761U;
            // The top bits, as a signed sample of the depth, sign-extended to the bytes
            const auto sample = static_cast<uint32_t>(static_cast<int32_t>(value) >> (32 - bits));
            for (uint32_t i = 0; i < bytes; ++i) {
                out.push_back(static_cast<uint8_t>(sample >> (8 * i)));
            }
        }
    }
    return out;
}

struct EncodedFlac {
    Bytes file;
    /// The header's length: "fLaC" and STREAMINFO
    size_t header{0};
    /// Each frame's offset from the first frame
    std::vector<uint64_t> frame_offsets;
};

/// pcm encoded with micro-flac's encoder in frames of block, its finished STREAMINFO (with the
/// length) written over the first unless unfinished
EncodedFlac encode_flac(const Bytes& pcm, uint32_t rate, uint32_t channels, uint32_t bits,
                        uint32_t block, bool unfinished = false) {
    micro_flac::FLACEncoderOptions options;
    options.block_size = block;
    auto encoder = std::make_unique<micro_flac::FLACEncoder>(
        micro_flac::PcmFormat{rate, channels, bits}, options);
    EncodedFlac out;
    Bytes buffer(std::max(micro_flac::FLACEncoder::HEADER_BYTES, encoder->get_max_output_bytes()));
    size_t written = 0;
    EXPECT_EQ(encoder->write_header(buffer.data(), buffer.size(), written),
              micro_flac::FLAC_ENCODER_SUCCESS);
    out.file.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
    out.header = written;
    const size_t block_bytes = encoder->get_input_block_bytes();
    size_t at = 0;
    while (at < pcm.size()) {
        out.frame_offsets.push_back(out.file.size() - out.header);
        const size_t left = pcm.size() - at;
        if (left >= block_bytes) {
            size_t consumed = 0;
            EXPECT_EQ(encoder->encode(pcm.data() + at, block_bytes, buffer.data(), buffer.size(),
                                      consumed, written),
                      micro_flac::FLAC_ENCODER_SUCCESS);
            at += consumed;
        } else {
            EXPECT_EQ(encoder->finish(pcm.data() + at, left, buffer.data(), buffer.size(), written),
                      micro_flac::FLAC_ENCODER_SUCCESS);
            at = pcm.size();
        }
        out.file.insert(out.file.end(), buffer.begin(),
                        buffer.begin() + static_cast<std::ptrdiff_t>(written));
    }
    if (pcm.size() % block_bytes == 0) {
        EXPECT_EQ(encoder->finish(nullptr, 0, buffer.data(), buffer.size(), written),
                  micro_flac::FLAC_ENCODER_SUCCESS);
    }
    if (!unfinished) {
        EXPECT_EQ(encoder->write_header(out.file.data(), out.header, written),
                  micro_flac::FLAC_ENCODER_SUCCESS);
    }
    return out;
}

void put_be(Bytes& out, uint64_t value, size_t bytes) {
    for (size_t i = bytes; i > 0; --i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * (i - 1))));
    }
}

/// encoded with a SEEKTABLE block after its STREAMINFO, a point at every stride-th frame
Bytes with_seek_table(const EncodedFlac& encoded, uint32_t block, size_t stride) {
    Bytes table;
    for (size_t i = 0; i < encoded.frame_offsets.size(); i += stride) {
        put_be(table, uint64_t{block} * i, 8);
        put_be(table, encoded.frame_offsets[i], 8);
        put_be(table, block, 2);
    }
    Bytes out(encoded.file.begin(),
              encoded.file.begin() + static_cast<std::ptrdiff_t>(encoded.header));
    // STREAMINFO is then not the last block
    out[STREAMINFO_HEADER_AT] &= static_cast<uint8_t>(~LAST_BLOCK);
    out.push_back(LAST_BLOCK | SEEKTABLE);
    put_be(out, table.size(), 3);
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), encoded.file.begin() + static_cast<std::ptrdiff_t>(encoded.header),
               encoded.file.end());
    return out;
}

void put_u16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(Bytes& out, uint32_t value) {
    put_u16(out, static_cast<uint16_t>(value));
    put_u16(out, static_cast<uint16_t>(value >> 16));
}

/// A 16-bit PCM WAV file of pcm
Bytes wav(const Bytes& pcm, uint16_t channels) {
    Bytes out{'R', 'I', 'F', 'F'};
    put_u32(out, static_cast<uint32_t>(36 + pcm.size()));
    for (const char c : std::string("WAVEfmt ")) {
        out.push_back(static_cast<uint8_t>(c));
    }
    put_u32(out, 16);
    put_u16(out, 1);
    put_u16(out, channels);
    put_u32(out, RATE);
    put_u32(out, RATE * channels * 2);
    put_u16(out, static_cast<uint16_t>(channels * 2));
    put_u16(out, 16);
    for (const char c : std::string("data")) {
        out.push_back(static_cast<uint8_t>(c));
    }
    put_u32(out, static_cast<uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

std::string write_file(const TempDir& dir, const std::string& name, const Bytes& bytes) {
    const std::string path = dir.file(name);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

/// frames read from source at frame, as bytes; fewer when the source gives fewer
Bytes read_at(AudioSource& source, uint64_t frame, size_t frames) {
    const size_t size = frame_bytes(source.info().format);
    Bytes out(frames * size);
    out.resize(source.read(frame, out.data(), frames) * size);
    return out;
}

/// The reference's frames from frame on, at most frames of them
Bytes slice(const Bytes& pcm, size_t size, uint64_t frame, size_t frames) {
    const size_t begin = std::min(pcm.size(), static_cast<size_t>(frame) * size);
    const size_t end = std::min(pcm.size(), begin + frames * size);
    return {pcm.begin() + static_cast<std::ptrdiff_t>(begin),
            pcm.begin() + static_cast<std::ptrdiff_t>(end)};
}

TEST(AudioSource, DecodesAFlacFileToTheSamplesEncoded) {
    struct Row {
        uint32_t rate;
        uint8_t channels;
        uint32_t bits;
        uint32_t block;
        SampleFormat format;
    };
    const Row rows[] = {
        {48000, 2, 16, 4096, SampleFormat::S16},
        {44100, 1, 24, 1152, SampleFormat::S24},
        {32000, 2, 24, 576, SampleFormat::S24},
    };
    TempDir dir;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.rate);
        // Not a whole number of blocks, so the last frame is short
        constexpr uint32_t FRAMES = 20000;
        const Bytes pcm = samples(FRAMES, row.channels, row.bits);
        const std::string path = write_file(
            dir, "decode.flac", encode_flac(pcm, row.rate, row.channels, row.bits, row.block).file);
        const OpenedAudio opened = open_audio_file(path);
        ASSERT_TRUE(opened.source) << opened.error;
        const AudioInfo info = opened.source->info();
        EXPECT_EQ(info.format, (InputFormat{row.rate, row.channels, row.format}));
        EXPECT_EQ(info.frames, FRAMES);
        // Reads of a size no frame has, one after another
        Bytes decoded;
        for (uint64_t frame = 0; frame < FRAMES; frame += 1000) {
            const Bytes part = read_at(*opened.source, frame, 1000);
            decoded.insert(decoded.end(), part.begin(), part.end());
        }
        EXPECT_TRUE(decoded == pcm);
    }
}

// A seek goes back or ahead, into the middle of a frame, and up to the end: with a seek table it
// starts from the point before the frame, without one from the first frame or where decoding is
TEST(AudioSource, SeeksInAFlacFileWithOrWithoutASeekTable) {
    constexpr uint32_t FRAMES = 30000;
    constexpr uint32_t BLOCK = 1152;
    const Bytes pcm = samples(FRAMES, 2, 16);
    const EncodedFlac encoded = encode_flac(pcm, RATE, 2, 16, BLOCK);
    TempDir dir;
    for (const bool table : {true, false}) {
        SCOPED_TRACE(table);
        const std::string path =
            write_file(dir, "seek.flac", table ? with_seek_table(encoded, BLOCK, 4) : encoded.file);
        const OpenedAudio opened = open_audio_file(path);
        ASSERT_TRUE(opened.source) << opened.error;
        for (const uint64_t frame : {25000, 3, 12345, 12345 + 300, 13000, 29990, 0}) {
            SCOPED_TRACE(frame);
            EXPECT_TRUE(read_at(*opened.source, frame, 300) == slice(pcm, 4, frame, 300));
        }
        EXPECT_EQ(read_at(*opened.source, FRAMES, 300).size(), 0U);
    }
}

// With a seek table, a seek starts at its point and decodes nothing before it: past a first frame
// that does not decode, the frames after its point read; without the table (Control), decoding
// from the first frame stops there
TEST(AudioSource, ASeekStartsAtTheSeekTablePoint) {
    constexpr uint32_t FRAMES = 30000;
    constexpr uint32_t BLOCK = 1152;
    const Bytes pcm = samples(FRAMES, 2, 16);
    const EncodedFlac encoded = encode_flac(pcm, RATE, 2, 16, BLOCK);
    TempDir dir;
    for (const bool table : {true, false}) {
        SCOPED_TRACE(table);
        Bytes file = table ? with_seek_table(encoded, BLOCK, 4) : encoded.file;
        const size_t first_frame = file.size() - (encoded.file.size() - encoded.header);
        std::fill(file.begin() + static_cast<std::ptrdiff_t>(first_frame),
                  file.begin() + static_cast<std::ptrdiff_t>(first_frame + 8), 0);
        const OpenedAudio opened = open_audio_file(write_file(dir, "damaged.flac", file));
        ASSERT_TRUE(opened.source) << opened.error;
        const uint64_t frame = 20000;
        if (table) {
            EXPECT_TRUE(read_at(*opened.source, frame, 300) == slice(pcm, 4, frame, 300));
        } else {
            EXPECT_EQ(read_at(*opened.source, frame, 300).size(), 0U);
        }
    }
}

TEST(AudioSource, ReadsAWavFileFromAnyFrame) {
    constexpr uint32_t FRAMES = 10000;
    const Bytes pcm = samples(FRAMES, 2, 16);
    TempDir dir;
    const OpenedAudio opened = open_audio_file(write_file(dir, "any.wav", wav(pcm, 2)));
    ASSERT_TRUE(opened.source) << opened.error;
    EXPECT_EQ(opened.source->info().format, (InputFormat{RATE, 2, SampleFormat::S16}));
    EXPECT_EQ(opened.source->info().frames, FRAMES);
    for (const uint64_t frame : {9000, 0, 4321, 9990}) {
        SCOPED_TRACE(frame);
        EXPECT_TRUE(read_at(*opened.source, frame, 300) == slice(pcm, 4, frame, 300));
    }
}

// Neither reader holds the file: cut short once opened and partly read, a file reads only as far
// as the cut, where a reader that loaded the whole file at open would read all of it
TEST(AudioSource, ReadsTheFileAsItPlays) {
    constexpr uint32_t FRAMES = 30000;
    const Bytes pcm = samples(FRAMES, 2, 16);
    TempDir dir;
    for (const bool flac : {true, false}) {
        SCOPED_TRACE(flac);
        const Bytes file = flac ? encode_flac(pcm, RATE, 2, 16, 1152).file : wav(pcm, 2);
        const std::string path = write_file(dir, flac ? "cut.flac" : "cut.wav", file);
        const OpenedAudio opened = open_audio_file(path);
        ASSERT_TRUE(opened.source) << opened.error;
        ASSERT_TRUE(read_at(*opened.source, 0, 1000) == slice(pcm, 4, 0, 1000));
        std::filesystem::resize_file(path, file.size() / 2);
        size_t read = 1000;
        for (uint64_t frame = 1000; frame < FRAMES; frame += 1000) {
            const size_t got = read_at(*opened.source, frame, 1000).size() / 4;
            read += got;
            if (got < 1000) {
                break;
            }
        }
        EXPECT_LT(read, FRAMES * 3 / 4);
        EXPECT_GT(read, 1000U);
    }
}

TEST(AudioSource, RefusesTheFilesItCannotPlay) {
    TempDir dir;
    const Bytes stereo = samples(5000, 2, 16);
    struct Row {
        const char* what;
        Bytes file;
        bool plays;
    };
    Bytes ogg = encode_flac(stereo, RATE, 2, 16, 1152).file;
    std::memcpy(ogg.data(), "OggS", 4);
    const Row rows[] = {
        {"Control: 16-bit stereo FLAC", encode_flac(stereo, RATE, 2, 16, 1152).file, true},
        {"Control: 16-bit stereo WAV", wav(stereo, 2), true},
        {"three channels", encode_flac(samples(5000, 3, 16), RATE, 3, 16, 1152).file, false},
        {"20-bit samples", encode_flac(samples(5000, 2, 20), RATE, 2, 20, 1152).file, false},
        {"no length in its STREAMINFO", encode_flac(stereo, RATE, 2, 16, 1152, true).file, false},
        {"Ogg", ogg, false},
        {"empty", Bytes{}, false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.what);
        const OpenedAudio opened = open_audio_file(write_file(dir, "refused", row.file));
        EXPECT_EQ(opened.source != nullptr, row.plays);
        EXPECT_EQ(opened.error.empty(), row.plays);
    }
    EXPECT_FALSE(open_audio_file(dir.file("missing.flac")).source);
}

}  // namespace
