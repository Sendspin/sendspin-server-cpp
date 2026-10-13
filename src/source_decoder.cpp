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

#include "source_decoder.h"

#include "sched/constants.h"
#include <micro_flac/flac_decoder.h>
#if SENDSPIN_SERVER_ENABLE_OPUS
#include <opus.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <utility>

namespace sendspin::server {

namespace {

/// The most samples per channel an Opus packet decodes to: 120 ms at 48 kHz (RFC 6716 section 3.2)
constexpr int OPUS_MAX_PACKET_SAMPLES = 5760;
constexpr uint32_t OPUS_MAX_CHANNELS = 2;
constexpr std::array<uint32_t, 5> OPUS_RATES{8000, 12000, 16000, 24000, 48000};
/// FLAC's depths (RFC 9639 section 8.2)
constexpr uint32_t FLAC_MIN_DEPTH = 4;
constexpr uint32_t FLAC_MAX_DEPTH = 32;
/// Full scale of a left-justified 32-bit sample
constexpr float FULL_SCALE_32 = 2147483648.0F;

// libopus, or, built without Opus (SENDSPIN_SERVER_ENABLE_OPUS off), no Opus decoder
#if SENDSPIN_SERVER_ENABLE_OPUS
constexpr bool OPUS_BUILT = true;

OpusDecoder* opus_create(uint32_t rate, uint32_t channels) {
    int error = OPUS_OK;
    OpusDecoder* decoder =
        opus_decoder_create(static_cast<opus_int32>(rate), static_cast<int>(channels), &error);
    return error == OPUS_OK ? decoder : nullptr;
}

void opus_destroy(OpusDecoder* decoder) {
    opus_decoder_destroy(decoder);
}

/// @return the samples per channel decoded, or a negative libopus error
int opus_decode_packet(OpusDecoder* decoder, std::span<const uint8_t> packet, int16_t* out) {
    return opus_decode(decoder, packet.data(), static_cast<opus_int32>(packet.size()), out,
                       OPUS_MAX_PACKET_SAMPLES, 0);
}
#else
constexpr bool OPUS_BUILT = false;

OpusDecoder* opus_create(uint32_t /*rate*/, uint32_t /*channels*/) {
    return nullptr;
}

void opus_destroy(OpusDecoder* /*decoder*/) {}

int opus_decode_packet(OpusDecoder* /*decoder*/, std::span<const uint8_t> /*packet*/,
                       int16_t* /*out*/) {
    return -1;
}
#endif

/// Depths that map bit for bit onto a GroupInput's integer formats
std::optional<SampleFormat> integer_format(uint32_t depth) {
    if (depth == 16) {
        return SampleFormat::S16;
    }
    if (depth == 24) {
        return SampleFormat::S24;
    }
    return std::nullopt;
}

}  // namespace

bool source_opus_available() {
    return OPUS_BUILT;
}

std::optional<InputFormat> source_input_format(const AudioFormat& format) {
    std::optional<SampleFormat> sample;
    switch (format.codec) {
        case Codec::PCM:
            sample = format.bit_depth == 32 ? std::optional(SampleFormat::F32)
                                            : integer_format(format.bit_depth);
            break;
        case Codec::FLAC:
            if (format.bit_depth >= FLAC_MIN_DEPTH && format.bit_depth <= FLAC_MAX_DEPTH) {
                sample = integer_format(format.bit_depth).value_or(SampleFormat::F32);
            }
            break;
        case Codec::OPUS:
            if (OPUS_BUILT && format.channels <= OPUS_MAX_CHANNELS &&
                std::find(OPUS_RATES.begin(), OPUS_RATES.end(), format.sample_rate) !=
                    OPUS_RATES.end()) {
                sample = SampleFormat::S16;
            }
            break;
    }
    constexpr uint32_t BYTE_MAX = 255;
    if (!sample || format.channels > BYTE_MAX) {
        return std::nullopt;
    }
    const InputFormat input{format.sample_rate, static_cast<uint8_t>(format.channels), *sample};
    if (!is_valid_input(input)) {
        return std::nullopt;
    }
    return input;
}

void SourceDecoder::OpusDeleter::operator()(OpusDecoder* decoder) const {
    opus_destroy(decoder);
}

SourceDecoder::SourceDecoder(std::shared_ptr<SourceFeed> feed, std::unique_ptr<SourcePcmSink> sink,
                             Wakeup::Handle faults)
    : feed_(std::move(feed)), sink_(std::move(sink)), faults_(std::move(faults)) {}

SourceDecoder::~SourceDecoder() = default;

bool SourceDecoder::run() {
    SourceItem item;
    std::span<const uint8_t> bytes;
    if (!this->feed_->front(item, bytes)) {
        return false;
    }
    const bool same = item.connection == this->connection_ && item.stream == this->stream_number_;
    switch (item.kind) {
        case SourceItem::Kind::START:
            this->connection_ = item.connection;
            this->stream_number_ = item.stream;
            this->stream_.reset();
            if (!this->start(item, bytes)) {
                this->report(SourceFault::Kind::UNSUPPORTED, item);
            }
            break;
        case SourceItem::Kind::CHUNK:
            if (same && this->stream_) {
                const Decoded decoded = this->decode(item, bytes);
                if (decoded != Decoded::TAKEN) {
                    this->stream_.reset();
                    this->report(decoded == Decoded::BROKEN ? SourceFault::Kind::PROTOCOL
                                                            : SourceFault::Kind::UNSUPPORTED,
                                 item);
                }
            }
            break;
        case SourceItem::Kind::END:
            if (same) {
                this->stream_.reset();
                this->sink_->end();
            }
            break;
    }
    this->feed_->pop();
    return !this->feed_->empty();
}

void SourceDecoder::report(SourceFault::Kind kind, const SourceItem& item) {
    this->feed_->report({kind, item.connection, item.stream});
    Wakeup::signal(this->faults_);
}

bool SourceDecoder::reserve(size_t bytes) {
    if (bytes <= this->buffer_bytes_) {
        return true;
    }
    this->buffer_.reset();
    this->buffer_bytes_ = 0;
    // A size the heap cannot give refuses the stream rather than ending the process
    this->buffer_.reset(static_cast<uint8_t*>(
        placed_malloc(bytes, new_alignment<uint8_t>(), MemoryLocation::PREFER_EXTERNAL)));
    if (!this->buffer_) {
        return false;
    }
    this->buffer_bytes_ = bytes;
    return true;
}

bool SourceDecoder::start(const SourceItem& item, std::span<const uint8_t> header) {
    const std::optional<InputFormat> output = source_input_format(item.format);
    if (!output) {
        return false;
    }
    const AudioFormat& format = item.format;
    this->flac_.reset();
    this->opus_.reset();
    const size_t sample_bytes = output->sample_format == SampleFormat::S16   ? 2
                                : output->sample_format == SampleFormat::S24 ? 3
                                                                             : 4;
    const size_t max_frames =
        static_cast<size_t>(int64_t{output->sample_rate} * MAX_SOURCE_CHUNK_US / 1000000);
    bool reserved = true;
    switch (format.codec) {
        case Codec::PCM:
            if (output->sample_format == SampleFormat::F32) {
                reserved = this->reserve(max_frames * format.channels * sample_bytes);
            }
            break;
        case Codec::FLAC: {
            const size_t block = this->start_flac(header, max_frames);
            if (block == 0) {
                return false;
            }
            // A frame decodes only with a largest frame's room left
            this->flac_frames_ = max_frames + block;
            reserved = this->reserve(this->flac_frames_ * format.channels * sample_bytes);
            break;
        }
        case Codec::OPUS:
            this->opus_.reset(opus_create(format.sample_rate, format.channels));
            if (!this->opus_) {
                return false;
            }
            reserved =
                this->reserve(size_t{OPUS_MAX_PACKET_SAMPLES} * format.channels * sample_bytes);
            break;
    }
    if (!reserved || !this->sink_->begin(*output)) {
        return false;
    }
    this->stream_ = format;
    this->output_ = *output;
    return true;
}

size_t SourceDecoder::start_flac(std::span<const uint8_t> header, size_t max_frames) {
    // NOLINTBEGIN(readability-magic-numbers): RFC 9639 section 8.1 and 8.2's byte layout
    constexpr size_t STREAMINFO_HEADER = 42;
    constexpr size_t LAST_BLOCK_BYTE = 4;
    constexpr uint8_t LAST_BLOCK = 0x80;
    constexpr size_t MIN_BLOCK_BYTE = 8;
    constexpr size_t MAX_BLOCK_BYTE = 10;
    if (header.size() < STREAMINFO_HEADER) {
        return 0;
    }
    // A size the heap cannot give refuses the stream rather than ending the process
    const std::unique_ptr<uint8_t[]> owned(new (std::nothrow) uint8_t[header.size()]);
    if (!owned) {
        return 0;
    }
    std::copy(header.begin(), header.end(), owned.get());
    const std::span<uint8_t> copy(owned.get(), header.size());
    // roles/source/v1.md "client-stream/start": the header may end at STREAMINFO without marking
    // it the last metadata block, which micro-flac would wait past for more
    if (copy.size() == STREAMINFO_HEADER) {
        copy[LAST_BLOCK_BYTE] |= LAST_BLOCK;
    }
    // A frame longer than a chunk may be is never valid, so the decoder's buffers stop at a chunk's
    // length and a longer frame fails to decode
    const auto cap = static_cast<uint16_t>(std::min<size_t>(max_frames, UINT16_MAX));
    for (const size_t at : {MIN_BLOCK_BYTE, MAX_BLOCK_BYTE}) {
        const auto size = static_cast<uint16_t>((copy[at] << 8) | copy[at + 1]);
        if (size > cap) {
            copy[at] = static_cast<uint8_t>(cap >> 8);
            copy[at + 1] = static_cast<uint8_t>(cap);
        }
    }
    // NOLINTEND(readability-magic-numbers)
    this->flac_.reset(new (std::nothrow) micro_flac::FLACDecoder());
    if (!this->flac_) {
        return 0;
    }
    size_t used = 0;
    size_t samples = 0;
    const micro_flac::FLACDecoderResult result = this->flac_->decode(
        copy.data(), copy.size(), static_cast<uint8_t*>(nullptr), 0, used, samples);
    if (result != micro_flac::FLAC_DECODER_HEADER_READY || used != copy.size()) {
        return 0;
    }
    return this->flac_->get_stream_info().max_block_size();
}

SourceDecoder::Decoded SourceDecoder::decode(const SourceItem& item,
                                             std::span<const uint8_t> audio) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only called with a stream
    switch (this->stream_->codec) {
        case Codec::PCM:
            return this->decode_pcm(item, audio);
        case Codec::FLAC:
            return this->decode_flac(item, audio);
        case Codec::OPUS:
            return this->decode_opus(item, audio);
    }
    return Decoded::BROKEN;
}

SourceDecoder::Decoded SourceDecoder::emit(const SourceItem& item, const uint8_t* pcm,
                                           size_t frames) {
    if (static_cast<int64_t>(frames) * sched::US_PER_SECOND >
        MAX_SOURCE_CHUNK_US * int64_t{this->output_.sample_rate}) {
        return Decoded::BROKEN;
    }
    if (frames > 0) {
        this->sink_->write(pcm, frames, item.capture_us, item.rx_us);
    }
    return Decoded::TAKEN;
}

void SourceDecoder::widen(size_t samples) {
    uint8_t* bytes = this->buffer_.get();
    for (size_t i = 0; i < samples; ++i) {
        int32_t value = 0;
        std::memcpy(&value, bytes + i * 4, 4);
        const float sample = static_cast<float>(value) / FULL_SCALE_32;
        std::memcpy(bytes + i * 4, &sample, 4);
    }
}

// roles/source/v1.md "Codec framing": any whole number of frames, interleaved, little-endian
SourceDecoder::Decoded SourceDecoder::decode_pcm(const SourceItem& item,
                                                 std::span<const uint8_t> audio) {
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): only called with a stream
    const AudioFormat& format = *this->stream_;
    const size_t frame_bytes = size_t{format.channels} * (format.bit_depth / 8);
    if (audio.size() % frame_bytes != 0) {
        return Decoded::BROKEN;
    }
    const size_t frames = audio.size() / frame_bytes;
    if (this->output_.sample_format != SampleFormat::F32) {
        return this->emit(item, audio.data(), frames);
    }
    const size_t samples = frames * format.channels;
    if (samples * 4 > this->buffer_bytes_) {
        return Decoded::BROKEN;
    }
    for (size_t i = 0; i < samples; ++i) {
        const uint8_t* in = audio.data() + i * 4;
        const auto value = static_cast<int32_t>(uint32_t{in[0]} | (uint32_t{in[1]} << 8) |
                                                (uint32_t{in[2]} << 16) | (uint32_t{in[3]} << 24));
        std::memcpy(this->buffer_.get() + i * 4, &value, 4);
    }
    this->widen(samples);
    return this->emit(item, this->buffer_.get(), frames);
}

// roles/source/v1.md "Codec framing": one or more complete FLAC frames, none spanning chunks
SourceDecoder::Decoded SourceDecoder::decode_flac(const SourceItem& item,
                                                  std::span<const uint8_t> audio) {
    const uint32_t channels = this->output_.channels;
    const bool wide = this->output_.sample_format == SampleFormat::F32;
    const size_t sample_bytes = wide ? 4 : this->output_.sample_format == SampleFormat::S16 ? 2 : 3;
    if (audio.empty()) {
        return Decoded::BROKEN;
    }
    size_t decoded = 0;
    size_t offset = 0;
    while (offset < audio.size()) {
        const size_t room = (this->flac_frames_ * channels) - decoded;
        uint8_t* out = this->buffer_.get() + decoded * sample_bytes;
        size_t used = 0;
        size_t samples = 0;
        const micro_flac::FLACDecoderResult result =
            wide ? this->flac_->decode(audio.data() + offset, audio.size() - offset,
                                       reinterpret_cast<int32_t*>(out), room, used, samples)
                 : this->flac_->decode(audio.data() + offset, audio.size() - offset, out,
                                       room * sample_bytes, used, samples);
        // micro-flac allocates its block buffers at the first frame it decodes
        if (result == micro_flac::FLAC_DECODER_ERROR_MEMORY_ALLOCATION) {
            return Decoded::REFUSED;
        }
        if (result != micro_flac::FLAC_DECODER_SUCCESS || used == 0 || samples % channels != 0) {
            return Decoded::BROKEN;
        }
        decoded += samples;
        offset += used;
    }
    if (wide) {
        this->widen(decoded);
    }
    return this->emit(item, this->buffer_.get(), decoded / channels);
}

// roles/source/v1.md "Codec framing": exactly one Opus packet per chunk; an Opus packet is at least
// one byte (RFC 6716 section 3), so an empty chunk is not one
SourceDecoder::Decoded SourceDecoder::decode_opus(const SourceItem& item,
                                                  std::span<const uint8_t> audio) {
    if (audio.empty()) {
        return Decoded::BROKEN;
    }
    const int samples = opus_decode_packet(this->opus_.get(), audio,
                                           reinterpret_cast<int16_t*>(this->buffer_.get()));
    if (samples <= 0) {
        return Decoded::BROKEN;
    }
    return this->emit(item, this->buffer_.get(), static_cast<size_t>(samples));
}

}  // namespace sendspin::server
