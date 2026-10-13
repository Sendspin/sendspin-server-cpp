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

#include "flac_file.h"

#include <micro_flac/flac_decoder.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace sendspin::server::examples {

namespace {

/// RFC 9639 section 8: a metadata block header, its last-block flag and the SEEKTABLE type
constexpr size_t MAGIC_BYTES = 4;
constexpr size_t BLOCK_HEADER_BYTES = 4;
constexpr uint8_t LAST_BLOCK = 0x80;
constexpr uint8_t BLOCK_TYPE_MASK = 0x7F;
constexpr uint8_t BLOCK_TYPE_SEEKTABLE = 3;
constexpr uint8_t BLOCK_TYPE_INVALID = 127;
/// RFC 9639 section 8.5: a seek point, and the sample number of a placeholder point
constexpr size_t SEEK_POINT_BYTES = 18;
constexpr uint64_t PLACEHOLDER_POINT = ~uint64_t{0};
/// Bytes read from the file at a time
constexpr size_t READ_BYTES = 16384;
constexpr unsigned BITS_PER_BYTE = 8;
constexpr uint32_t BITS_16 = 16;
constexpr uint32_t BITS_24 = 24;
constexpr uint32_t MAX_CHANNELS = 2;

uint64_t read_be(const uint8_t* p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value = (value << BITS_PER_BYTE) | p[i];
    }
    return value;
}

/// A seek point: the first sample of a frame and the frame's offset from the first frame
struct SeekPoint {
    uint64_t sample{0};
    uint64_t offset{0};
};

class FlacFile : public AudioSource {
public:
    explicit FlacFile(std::FILE* file) : file_(file) {}

    ~FlacFile() override {
        std::fclose(this->file_);
    }

    FlacFile(const FlacFile&) = delete;
    FlacFile& operator=(const FlacFile&) = delete;

    /// Reads the metadata blocks and the STREAMINFO
    /// @return why the file cannot be played; empty when it can
    std::string open() {
        std::array<uint8_t, MAGIC_BYTES> magic{};
        if (std::fread(magic.data(), 1, magic.size(), this->file_) != magic.size() ||
            std::memcmp(magic.data(), "fLaC", MAGIC_BYTES) != 0) {
            return "not a native FLAC file";
        }
        uint64_t position = MAGIC_BYTES;
        for (;;) {
            std::array<uint8_t, BLOCK_HEADER_BYTES> header{};
            if (std::fread(header.data(), 1, header.size(), this->file_) != header.size()) {
                return "a truncated metadata block";
            }
            const uint8_t type = header[0] & BLOCK_TYPE_MASK;
            const auto length = static_cast<size_t>(read_be(&header[1], 3));
            if (type == BLOCK_TYPE_INVALID) {
                return "an invalid metadata block";
            }
            if (type == BLOCK_TYPE_SEEKTABLE) {
                std::vector<uint8_t> table(length);
                if (std::fread(table.data(), 1, length, this->file_) != length) {
                    return "a truncated seek table";
                }
                for (size_t at = 0; at + SEEK_POINT_BYTES <= length; at += SEEK_POINT_BYTES) {
                    const uint64_t sample = read_be(&table[at], sizeof(uint64_t));
                    if (sample != PLACEHOLDER_POINT) {
                        this->seek_points_.push_back(
                            {sample, read_be(&table[at + sizeof(uint64_t)], sizeof(uint64_t))});
                    }
                }
            } else if (::fseeko(this->file_, static_cast<off_t>(length), SEEK_CUR) != 0) {
                return "a truncated metadata block";
            }
            position += BLOCK_HEADER_BYTES + length;
            if ((header[0] & LAST_BLOCK) != 0) {
                break;
            }
        }
        this->audio_offset_ = position;
        std::sort(this->seek_points_.begin(), this->seek_points_.end(),
                  [](const SeekPoint& a, const SeekPoint& b) { return a.sample < b.sample; });

        if (!this->restart({0, 0})) {
            return "a STREAMINFO block micro-flac cannot read";
        }
        const micro_flac::FLACStreamInfo& stream = this->decoder_->get_stream_info();
        const uint32_t bits = stream.bits_per_sample();
        if (bits != BITS_16 && bits != BITS_24) {
            return std::to_string(bits) + "-bit samples; only 16-bit and 24-bit are supported";
        }
        if (stream.num_channels() > MAX_CHANNELS) {
            return std::to_string(stream.num_channels()) +
                   " channels; only mono and stereo are supported";
        }
        if (stream.total_samples_per_channel() == 0) {
            return "no length in its STREAMINFO";
        }
        this->info_.format = {stream.sample_rate(), static_cast<uint8_t>(stream.num_channels()),
                              bits == BITS_16 ? SampleFormat::S16 : SampleFormat::S24};
        this->info_.frames = stream.total_samples_per_channel();
        this->block_.resize(size_t{stream.max_block_size()} * stream.num_channels() *
                            stream.bytes_per_sample());
        return {};
    }

    AudioInfo info() const override {
        return this->info_;
    }

    size_t read(uint64_t frame, uint8_t* out, size_t frames) override {
        const size_t bytes = frame_bytes(this->info_.format);
        size_t done = 0;
        while (done < frames && frame + done < this->info_.frames) {
            const uint64_t at = frame + done;
            const uint64_t end = this->block_start_ + this->block_frames_;
            if (at >= this->block_start_ && at < end) {
                const auto n = static_cast<size_t>(std::min<uint64_t>(frames - done, end - at));
                std::memcpy(out + done * bytes,
                            this->block_.data() + (at - this->block_start_) * bytes, n * bytes);
                done += n;
                continue;
            }
            const SeekPoint point = this->nearest_point(at);
            // Restarts unless decoding on from here reaches the frame as soon
            const bool restart = at < this->block_start_ || point.sample > end;
            if ((restart && !this->restart(point)) || !this->next_block()) {
                break;
            }
        }
        return done;
    }

private:
    /// The last seek point at or before frame; the first frame when none is
    SeekPoint nearest_point(uint64_t frame) const {
        const auto after =
            std::upper_bound(this->seek_points_.begin(), this->seek_points_.end(), frame,
                             [](uint64_t f, const SeekPoint& point) { return f < point.sample; });
        return after == this->seek_points_.begin() ? SeekPoint{} : *(after - 1);
    }

    /// Decodes the header again, then reads from the frame at point
    bool restart(const SeekPoint& point) {
        if (!this->decoder_) {
            this->decoder_ = std::make_unique<micro_flac::FLACDecoder>();
        }
        this->decoder_->reset();
        this->block_start_ = point.sample;
        this->block_frames_ = 0;
        if (!this->seek(0)) {
            return false;
        }
        for (;;) {
            if (this->input_begin_ == this->input_end_ && !this->refill()) {
                return false;
            }
            size_t used = 0;
            size_t samples = 0;
            const micro_flac::FLACDecoderResult result = this->decoder_->decode(
                this->input_.data() + this->input_begin_, this->input_end_ - this->input_begin_,
                static_cast<uint8_t*>(nullptr), 0, used, samples);
            this->input_begin_ += used;
            if (result == micro_flac::FLAC_DECODER_HEADER_READY) {
                break;
            }
            if (result != micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
                return false;
            }
            if (!this->refill()) {
                return false;
            }
        }
        return this->seek(this->audio_offset_ + point.offset);
    }

    /// Decodes the frame after the one held
    bool next_block() {
        const uint64_t next = this->block_start_ + this->block_frames_;
        for (;;) {
            if (this->input_begin_ == this->input_end_ && !this->refill()) {
                return false;
            }
            size_t used = 0;
            size_t samples = 0;
            const micro_flac::FLACDecoderResult result = this->decoder_->decode(
                this->input_.data() + this->input_begin_, this->input_end_ - this->input_begin_,
                this->block_.data(), this->block_.size(), used, samples);
            this->input_begin_ += used;
            if (result == micro_flac::FLAC_DECODER_SUCCESS && samples > 0) {
                this->block_start_ = next;
                this->block_frames_ = samples / this->info_.format.channels;
                return true;
            }
            if (result == micro_flac::FLAC_DECODER_NEED_MORE_DATA && !this->refill()) {
                return false;
            }
            if (result != micro_flac::FLAC_DECODER_SUCCESS &&
                result != micro_flac::FLAC_DECODER_NEED_MORE_DATA) {
                return false;
            }
        }
    }

    /// Reads on from offset of the file
    bool seek(uint64_t offset) {
        this->input_begin_ = 0;
        this->input_end_ = 0;
        return ::fseeko(this->file_, static_cast<off_t>(offset), SEEK_SET) == 0;
    }

    /// Reads more of the file after what the input holds
    /// @return false at the end of the file
    bool refill() {
        if (this->input_.empty()) {
            this->input_.resize(READ_BYTES);
        }
        const size_t left = this->input_end_ - this->input_begin_;
        std::memmove(this->input_.data(), this->input_.data() + this->input_begin_, left);
        this->input_begin_ = 0;
        this->input_end_ = left;
        const size_t got =
            std::fread(this->input_.data() + left, 1, this->input_.size() - left, this->file_);
        this->input_end_ += got;
        return got > 0;
    }

    std::FILE* file_;
    std::unique_ptr<micro_flac::FLACDecoder> decoder_;
    AudioInfo info_;
    /// Where the first frame starts in the file
    uint64_t audio_offset_{0};
    std::vector<SeekPoint> seek_points_;
    /// Bytes read from the file and not yet decoded: input_[input_begin_, input_end_)
    std::vector<uint8_t> input_;
    size_t input_begin_{0};
    size_t input_end_{0};
    /// The last frame decoded: block_frames_ frames from block_start_
    std::vector<uint8_t> block_;
    uint64_t block_start_{0};
    size_t block_frames_{0};
};

}  // namespace

OpenedAudio open_flac_file(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {nullptr, "cannot read " + path};
    }
    auto flac = std::make_shared<FlacFile>(file);
    std::string error = flac->open();
    if (!error.empty()) {
        return {nullptr, std::move(error)};
    }
    return {std::move(flac), {}};
}

}  // namespace sendspin::server::examples
