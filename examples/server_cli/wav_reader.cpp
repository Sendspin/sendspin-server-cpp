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

#include "wav_reader.h"

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace sendspin::server::examples {

namespace {

constexpr uint16_t FORMAT_PCM = 0x0001;
constexpr uint16_t FORMAT_IEEE_FLOAT = 0x0003;
constexpr uint16_t FORMAT_EXTENSIBLE = 0xFFFE;
constexpr size_t RIFF_HEADER_BYTES = 12;
constexpr size_t CHUNK_HEADER_BYTES = 8;
constexpr size_t FMT_BYTES = 16;
constexpr size_t FMT_EXTENSIBLE_BYTES = 40;
/// Field offsets in a `fmt ` chunk
constexpr size_t FMT_CHANNELS = 2;
constexpr size_t FMT_RATE = 4;
constexpr size_t FMT_BLOCK_ALIGN = 12;
constexpr size_t FMT_BITS = 14;
/// Offset of the sub-format GUID in an extensible `fmt ` chunk; its first two bytes are the
/// format tag
constexpr size_t SUBFORMAT_OFFSET = 24;
/// The rest of the KSDATAFORMAT_SUBTYPE GUIDs, after the tag
constexpr std::array<uint8_t, 14> SUBFORMAT_SUFFIX = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                      0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read_u32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

bool has_id(const uint8_t* p, std::string_view id) {
    return std::memcmp(p, id.data(), 4) == 0;
}

WavParse fail(std::string error) {
    return {std::nullopt, std::move(error)};
}

/// The sample format of a `fmt ` chunk body, or an error
WavParse parse_fmt(std::span<const uint8_t> body) {
    if (body.size() < FMT_BYTES) {
        return fail("the fmt chunk is too short");
    }
    uint16_t tag = read_u16(&body[0]);
    const uint16_t channels = read_u16(&body[FMT_CHANNELS]);
    const uint32_t rate = read_u32(&body[FMT_RATE]);
    const uint16_t block_align = read_u16(&body[FMT_BLOCK_ALIGN]);
    const uint16_t bits = read_u16(&body[FMT_BITS]);
    if (tag == FORMAT_EXTENSIBLE) {
        if (body.size() < FMT_EXTENSIBLE_BYTES ||
            !std::equal(SUBFORMAT_SUFFIX.begin(), SUBFORMAT_SUFFIX.end(),
                        body.begin() + SUBFORMAT_OFFSET + 2)) {
            return fail("unsupported extensible format");
        }
        tag = read_u16(&body[SUBFORMAT_OFFSET]);
    }
    WavInfo info;
    if (tag == FORMAT_PCM && bits == 16) {
        info.format.sample_format = SampleFormat::S16;
    } else if (tag == FORMAT_PCM && bits == 24) {
        info.format.sample_format = SampleFormat::S24;
    } else if (tag == FORMAT_IEEE_FLOAT && bits == 32) {
        info.format.sample_format = SampleFormat::F32;
    } else if (tag == FORMAT_PCM || tag == FORMAT_IEEE_FLOAT) {
        return fail(std::to_string(bits) + "-bit samples; only 16-bit and 24-bit PCM and 32-bit "
                                           "float are supported");
    } else {
        return fail("format tag " + std::to_string(tag) +
                    "; only PCM and IEEE float are supported");
    }
    if (channels != 1 && channels != 2) {
        return fail(std::to_string(channels) + " channels; only mono and stereo are supported");
    }
    if (rate == 0) {
        return fail("a sample rate of 0");
    }
    if (block_align != channels * (bits / 8)) {
        return fail("a block align that does not match the channels and bit depth");
    }
    info.format.sample_rate = rate;
    info.format.channels = static_cast<uint8_t>(channels);
    return {info, {}};
}

/// A WAV file's samples, read from the file as they are written
class WavFile : public AudioSource {
public:
    explicit WavFile(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {
        if (this->file_ != nullptr && ::fseeko(this->file_, 0, SEEK_END) == 0) {
            const off_t end = ::ftello(this->file_);
            this->size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
        }
    }

    ~WavFile() override {
        if (this->file_ != nullptr) {
            std::fclose(this->file_);
        }
    }

    WavFile(const WavFile&) = delete;
    WavFile& operator=(const WavFile&) = delete;

    bool opened() const {
        return this->file_ != nullptr;
    }

    uint64_t size() const {
        return this->size_;
    }

    /// Reads out.size() bytes at offset
    bool get(uint64_t offset, std::span<uint8_t> out) {
        return ::fseeko(this->file_, static_cast<off_t>(offset), SEEK_SET) == 0 &&
               std::fread(out.data(), 1, out.size(), this->file_) == out.size();
    }

    void set_info(const WavInfo& info) {
        this->info_ = info;
    }

    AudioInfo info() const override {
        return {this->info_.format, this->info_.frames, this->info_.truncated};
    }

    size_t read(uint64_t frame, uint8_t* out, size_t frames) override {
        const uint64_t count =
            std::min<uint64_t>(frames, this->info_.frames - std::min(frame, this->info_.frames));
        const size_t bytes = frame_bytes(this->info_.format);
        if (count == 0 ||
            ::fseeko(this->file_, static_cast<off_t>(this->info_.data_offset + frame * bytes),
                     SEEK_SET) != 0) {
            return 0;
        }
        return std::fread(out, bytes, static_cast<size_t>(count), this->file_);
    }

private:
    std::FILE* file_;
    uint64_t size_{0};
    WavInfo info_;
};

}  // namespace

WavParse parse_wav_header(uint64_t size, const ReadAt& read) {
    std::array<uint8_t, RIFF_HEADER_BYTES> riff{};
    if (size < RIFF_HEADER_BYTES || !read(0, riff) || !has_id(&riff[0], "RIFF") ||
        !has_id(&riff[8], "WAVE")) {
        return fail("not a RIFF/WAVE file");
    }
    std::optional<WavInfo> format;
    uint64_t pos = RIFF_HEADER_BYTES;
    for (;;) {
        std::array<uint8_t, CHUNK_HEADER_BYTES> header{};
        if (size - pos < CHUNK_HEADER_BYTES) {
            return fail(format ? "no data chunk" : "no fmt chunk");
        }
        if (!read(pos, header)) {
            return fail("cannot read the file");
        }
        const uint32_t chunk = read_u32(&header[4]);
        const uint64_t body = pos + CHUNK_HEADER_BYTES;
        const uint64_t available = size - body;
        if (has_id(header.data(), "data")) {
            if (!format) {
                return fail("a data chunk before the fmt chunk");
            }
            WavInfo info = *format;
            info.data_offset = body;
            info.truncated = chunk > available;
            info.frames = std::min<uint64_t>(chunk, available) / frame_bytes(info.format);
            return {info, {}};
        }
        if (chunk > available) {
            return fail("a truncated chunk");
        }
        if (has_id(header.data(), "fmt ")) {
            // parse_fmt() reads no further than an extensible chunk's fields
            std::vector<uint8_t> fmt(std::min<size_t>(chunk, FMT_EXTENSIBLE_BYTES));
            if (!read(body, fmt)) {
                return fail("cannot read the file");
            }
            WavParse parsed = parse_fmt(fmt);
            if (!parsed.info) {
                return parsed;
            }
            format = parsed.info;
        }
        // A chunk of odd length is followed by a pad byte, which a file may omit at its end
        pos = std::min(size, body + chunk + (chunk & 1U));
    }
}

WavParse parse_wav(std::span<const uint8_t> file) {
    return parse_wav_header(file.size(), [file](uint64_t offset, std::span<uint8_t> out) {
        std::memcpy(out.data(), file.data() + offset, out.size());
        return true;
    });
}

OpenedAudio open_wav_file(const std::string& path) {
    auto file = std::make_shared<WavFile>(path);
    if (!file->opened()) {
        return {nullptr, "cannot read " + path};
    }
    const WavParse parsed = parse_wav_header(
        file->size(),
        [&](uint64_t offset, std::span<uint8_t> out) { return file->get(offset, out); });
    if (!parsed.info) {
        return {nullptr, parsed.error};
    }
    file->set_info(*parsed.info);
    return {std::move(file), {}};
}

}  // namespace sendspin::server::examples
