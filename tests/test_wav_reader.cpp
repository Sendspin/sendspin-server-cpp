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

/// @file test_wav_reader.cpp
/// @brief server_cli's WAV parser over files built in memory

#include "wav_reader.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using sendspin::server::InputFormat;
using sendspin::server::SampleFormat;
using sendspin::server::examples::parse_wav;
using sendspin::server::examples::WavInfo;
using sendspin::server::examples::WavParse;
using Bytes = std::vector<uint8_t>;

constexpr uint16_t PCM = 1;
constexpr uint16_t ADPCM = 2;
constexpr uint16_t FLOAT = 3;
constexpr uint16_t EXTENSIBLE = 0xFFFE;
constexpr uint32_t RATE_44K = 44100;
constexpr uint32_t RATE_48K = 48000;
/// 100 16-bit mono frames and one byte more
constexpr size_t ODD_DATA_BYTES = 201;
/// Offset of the block align in a `fmt ` chunk
constexpr size_t FMT_BLOCK_ALIGN = 12;
/// The bytes an extensible `fmt ` chunk carries past the plain one
constexpr uint16_t EXTENSION_BYTES = 22;
/// Every sample byte of a data chunk
constexpr uint8_t SAMPLE_BYTE = 0x5A;

void put_u16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(Bytes& out, uint32_t value) {
    put_u16(out, static_cast<uint16_t>(value));
    put_u16(out, static_cast<uint16_t>(value >> 16));
}

struct Chunk {
    std::string id;
    Bytes body;
    /// The size the header declares, when not the body's
    std::optional<uint32_t> declared;
};

/// A RIFF/WAVE file of chunks, each padded to an even length
Bytes wav(const std::vector<Chunk>& chunks, const std::string& form = "WAVE") {
    Bytes out{'R', 'I', 'F', 'F'};
    put_u32(out, 0);
    out.insert(out.end(), form.begin(), form.end());
    for (const Chunk& chunk : chunks) {
        out.insert(out.end(), chunk.id.begin(), chunk.id.end());
        put_u32(out, chunk.declared.value_or(static_cast<uint32_t>(chunk.body.size())));
        out.insert(out.end(), chunk.body.begin(), chunk.body.end());
        if (chunk.body.size() % 2 == 1) {
            out.push_back(0);
        }
    }
    const auto riff_size = static_cast<uint32_t>(out.size() - 8);
    for (size_t i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<uint8_t>(riff_size >> (8 * i));
    }
    return out;
}

Chunk fmt(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits) {
    Bytes body;
    const auto block_align = static_cast<uint16_t>(channels * (bits / 8));
    put_u16(body, tag);
    put_u16(body, channels);
    put_u32(body, rate);
    put_u32(body, rate * block_align);
    put_u16(body, block_align);
    put_u16(body, bits);
    return {"fmt ", body, std::nullopt};
}

/// WAVE_FORMAT_EXTENSIBLE with sub-format tag
Chunk fmt_extensible(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits) {
    Chunk chunk = fmt(EXTENSIBLE, channels, rate, bits);
    put_u16(chunk.body, EXTENSION_BYTES);
    put_u16(chunk.body, bits);
    put_u32(chunk.body, channels == 2 ? 3 : 4);
    put_u16(chunk.body, tag);
    const Bytes guid_rest = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    chunk.body.insert(chunk.body.end(), guid_rest.begin(), guid_rest.end());
    return chunk;
}

Chunk data(size_t bytes) {
    return {"data", Bytes(bytes, SAMPLE_BYTE), std::nullopt};
}

TEST(WavReader, ParsesSupportedFilesAndRejectsTheRest) {
    struct Row {
        const char* name;
        Bytes file;
        /// nullopt when the file is refused
        std::optional<WavInfo> expected;
    };
    const Bytes control = wav({fmt(PCM, 1, 44100, 16), data(200)});
    // A data chunk of odd length that ends the file without its pad byte
    Bytes unpadded_data = wav({fmt(PCM, 1, RATE_44K, 16), data(ODD_DATA_BYTES)});
    unpadded_data.pop_back();
    // An odd-length chunk that ends the file without its pad byte, and no data chunk
    Bytes unpadded_last = wav({fmt(PCM, 1, RATE_44K, 16), {"LIST", Bytes(3, 0), std::nullopt}});
    unpadded_last.pop_back();
    Chunk misaligned = fmt(PCM, 2, RATE_48K, 16);
    misaligned.body[FMT_BLOCK_ALIGN] = 6;
    // The RIFF header and the fmt chunk's header, without its body
    constexpr size_t THROUGH_FMT_HEADER = 20;
    const Bytes truncated_header(control.begin(), control.begin() + THROUGH_FMT_HEADER);
    const Row rows[] = {
        {"Control: 16-bit mono", control,
         WavInfo{InputFormat{44100, 1, SampleFormat::S16}, 44, 100, false}},
        {"24-bit stereo", wav({fmt(PCM, 2, 48000, 24), data(60)}),
         WavInfo{InputFormat{48000, 2, SampleFormat::S24}, 44, 10, false}},
        {"32-bit float stereo", wav({fmt(FLOAT, 2, 96000, 32), data(80)}),
         WavInfo{InputFormat{96000, 2, SampleFormat::F32}, 44, 10, false}},
        {"extensible 24-bit stereo", wav({fmt_extensible(PCM, 2, 48000, 24), data(60)}),
         WavInfo{InputFormat{48000, 2, SampleFormat::S24}, 68, 10, false}},
        // An odd-length LIST chunk and its pad byte before fmt, a fact chunk between fmt and data
        {"extra chunks",
         wav({{"LIST", Bytes(5, 1), std::nullopt},
              fmt(PCM, 2, 48000, 16),
              {"fact", Bytes(4, 2), std::nullopt},
              data(40)}),
         WavInfo{InputFormat{48000, 2, SampleFormat::S16}, 70, 10, false}},
        // 402 of 1000 declared bytes: 100 whole frames
        {"data chunk shorter than declared",
         wav({fmt(PCM, 2, 48000, 16), {"data", Bytes(402, 0), 1000}}),
         WavInfo{InputFormat{48000, 2, SampleFormat::S16}, 44, 100, true}},
        {"odd data chunk without its pad byte", unpadded_data,
         WavInfo{InputFormat{44100, 1, SampleFormat::S16}, 44, 100, false}},
        {"truncated header", truncated_header, std::nullopt},
        {"odd last chunk without its pad byte", unpadded_last, std::nullopt},
        {"block align mismatch", wav({misaligned, data(60)}), std::nullopt},
        {"too short for RIFF", Bytes(control.begin(), control.begin() + 10), std::nullopt},
        {"not WAVE", wav({fmt(PCM, 1, 44100, 16), data(200)}, "AVI "), std::nullopt},
        {"wrong format tag", wav({fmt(ADPCM, 1, 44100, 16), data(200)}), std::nullopt},
        {"extensible with another sub-format", wav({fmt_extensible(ADPCM, 2, 48000, 16), data(40)}),
         std::nullopt},
        {"8-bit PCM", wav({fmt(PCM, 1, 44100, 8), data(200)}), std::nullopt},
        {"24-bit float", wav({fmt(FLOAT, 1, 44100, 24), data(200)}), std::nullopt},
        {"three channels", wav({fmt(PCM, 3, 44100, 16), data(60)}), std::nullopt},
        {"sample rate 0", wav({fmt(PCM, 1, 0, 16), data(200)}), std::nullopt},
        {"no data chunk", wav({fmt(PCM, 1, 44100, 16)}), std::nullopt},
        {"data before fmt", wav({data(200), fmt(PCM, 1, 44100, 16)}), std::nullopt},
        {"no fmt chunk", wav({{"LIST", Bytes(4, 0), std::nullopt}}), std::nullopt},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const WavParse parsed = parse_wav(row.file);
        if (!row.expected) {
            EXPECT_FALSE(parsed.info.has_value());
            EXPECT_FALSE(parsed.error.empty());
            continue;
        }
        if (!parsed.info) {
            ADD_FAILURE() << parsed.error;
            continue;
        }
        EXPECT_EQ(parsed.info->format, row.expected->format);
        EXPECT_EQ(parsed.info->data_offset, row.expected->data_offset);
        EXPECT_EQ(parsed.info->frames, row.expected->frames);
        EXPECT_EQ(parsed.info->truncated, row.expected->truncated);
    }
}

}  // namespace
