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

#include "frame_encoder.h"

#include "crypto/base64url.h"
#include "pcm_convert.h"
#include "profile.h"
#include "sched/constants.h"
#include "sched/frames.h"
#include "sched/timeline.h"
#include <micro_flac/flac_encoder.h>
#if SENDSPIN_SERVER_ENABLE_OPUS
#include <opus.h>
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace sendspin::server {

namespace {

constexpr uint8_t FLAC_LPC_ORDER = 12;

/// The quantize stage writes whole bytes; the encoder takes at most 24 bits
bool is_flac_depth(uint32_t bit_depth) {
    return is_pcm_output_depth(bit_depth) && bit_depth <= sched::MAX_FLAC_BIT_DEPTH;
}

/// One frame of the format's frame length per block
micro_flac::FLACEncoderOptions flac_options(uint32_t block_size) {
    micro_flac::FLACEncoderOptions options;
    options.block_size = block_size;
    options.lpc = profile::FLAC_LPC;
    if (options.lpc) {
        options.max_lpc_order = FLAC_LPC_ORDER;
        options.lpc_stereo_search = true;
    }
    options.max_rice_partition_order = profile::FLAC_RICE_PARTITION_ORDER;
    return options;
}

micro_flac::PcmFormat flac_input(const sched::AudioFormat& format) {
    return {format.sample_rate, format.channels, format.bit_depth};
}

/// The encoder's lookahead at the restricted low-delay application: 2.5 ms
constexpr uint32_t OPUS_DELAY_DIVISOR = 400;

// libopus, or, built without Opus (SENDSPIN_SERVER_ENABLE_OPUS off), no Opus encoder
#if SENDSPIN_SERVER_ENABLE_OPUS
constexpr bool OPUS_BUILT = true;

/// The Opus bitrate per channel, in bit/s: 128 kbit/s stereo, at or above what FFmpeg's defaults
/// through PyAV give aiosendspin's players, and 160-byte packets per channel, the least
/// sendspin-cpp designs its receive path for
constexpr opus_int32 OPUS_BITRATE_PER_CHANNEL = 64000;

/// @return nullptr when libopus refuses the format
OpusEncoder* opus_create(const sched::AudioFormat& format) {
    int error = OPUS_OK;
    OpusEncoder* encoder =
        opus_encoder_create(static_cast<opus_int32>(format.sample_rate), format.channels,
                            OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    if (encoder == nullptr || error != OPUS_OK) {
        return nullptr;
    }
    // Each frame decodes without the previous one's energy, so a decoder whose state is from
    // another point in the stream, a player joining a running instance or kept across a seek's
    // stream/clear, gives the frame's level from its first frame on (spec player/v1.md "No
    // startup warble")
    if (opus_encoder_ctl(encoder, OPUS_SET_PREDICTION_DISABLED(1)) != OPUS_OK ||
        opus_encoder_ctl(encoder, OPUS_SET_BITRATE(OPUS_BITRATE_PER_CHANNEL * format.channels)) !=
            OPUS_OK) {
        opus_encoder_destroy(encoder);
        return nullptr;
    }
    return encoder;
}

void opus_destroy(OpusEncoder* encoder) {
    opus_encoder_destroy(encoder);
}

void opus_reset(OpusEncoder* encoder) {
    opus_encoder_ctl(encoder, OPUS_RESET_STATE);
}

uint32_t opus_lookahead(OpusEncoder* encoder) {
    opus_int32 lookahead = 0;
    if (opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK) {
        return 0;
    }
    return static_cast<uint32_t>(lookahead);
}

/// @return the packet's bytes, 0 when the encoder refused
size_t opus_encode_frame(OpusEncoder* encoder, const int16_t* input, uint32_t frame_samples,
                         std::span<uint8_t> out) {
    const opus_int32 written = opus_encode(encoder, input, static_cast<int>(frame_samples),
                                           out.data(), static_cast<opus_int32>(out.size()));
    return written > 0 ? static_cast<size_t>(written) : 0;
}
#else
constexpr bool OPUS_BUILT = false;

OpusEncoder* opus_create(const sched::AudioFormat& /*format*/) {
    return nullptr;
}

void opus_destroy(OpusEncoder* /*encoder*/) {}

void opus_reset(OpusEncoder* /*encoder*/) {}

uint32_t opus_lookahead(OpusEncoder* /*encoder*/) {
    return 0;
}

size_t opus_encode_frame(OpusEncoder* /*encoder*/, const int16_t* /*input*/,
                         uint32_t /*frame_samples*/, std::span<uint8_t> /*out*/) {
    return 0;
}
#endif

}  // namespace

sched::FormatShape output_shape(const sched::AudioFormat& format) {
    if (format.codec == sched::Codec::OPUS) {
        sched::FormatShape shape;
        shape.format = format;
        shape.layout = sched::frame_layout(format.sample_rate, format.codec);
        shape.max_frame_bytes = OPUS_MAX_PACKET_BYTES;
        shape.encoder_available = OPUS_BUILT && shape.layout.usable && format.channels >= 1 &&
                                  format.channels <= sched::MAX_OPUS_CHANNELS;
        return shape;
    }
    if (format.codec != sched::Codec::FLAC) {
        sched::FormatShape shape = sched::pcm_shape(format);
        shape.encoder_available = shape.encoder_available && is_pcm_output_depth(format.bit_depth);
        return shape;
    }
    sched::FormatShape shape;
    shape.format = format;
    shape.layout = sched::frame_layout(format.sample_rate, format.codec);
    if (!is_flac_depth(format.bit_depth)) {
        return shape;
    }
    const micro_flac::FLACEncoder encoder(flac_input(format), flac_options(shape.layout.samples));
    shape.max_frame_bytes = static_cast<int64_t>(encoder.get_max_output_bytes());
    // 0 for a configuration the encoder does not take
    shape.encoder_available = shape.max_frame_bytes > 0;
    return shape;
}

uint32_t quantized_depth(const sched::AudioFormat& format) {
    return format.codec == sched::Codec::OPUS ? OPUS_BIT_DEPTH : format.bit_depth;
}

uint32_t encoder_delay_samples(const sched::AudioFormat& format) {
    return format.codec == sched::Codec::OPUS ? format.sample_rate / OPUS_DELAY_DIVISOR : 0;
}

int64_t encoder_delay_us(const sched::AudioFormat& format) {
    return sched::ceil_div(int64_t{encoder_delay_samples(format)} * sched::US_PER_SECOND,
                           int64_t{format.sample_rate});
}

std::optional<std::string> codec_header(const sched::AudioFormat& format) {
    if (format.codec != sched::Codec::FLAC || !is_flac_depth(format.bit_depth)) {
        return std::nullopt;
    }
    const sched::FrameLayout layout = sched::frame_layout(format.sample_rate, format.codec);
    micro_flac::FLACEncoder encoder(flac_input(format), flac_options(layout.samples));
    std::array<uint8_t, micro_flac::FLACEncoder::HEADER_BYTES> header{};
    size_t written = 0;
    if (encoder.write_header(header.data(), header.size(), written) !=
        micro_flac::FLAC_ENCODER_SUCCESS) {
        return std::nullopt;
    }
    return base64_encode(header.data(), written);
}

// ==============================================================================
// FrameEncoder
// ==============================================================================

std::unique_ptr<FrameEncoder> FrameEncoder::create(const sched::AudioFormat& format) {
    const sched::FormatShape shape = output_shape(format);
    if (!shape.encoder_available || !shape.layout.usable) {
        return nullptr;
    }
    std::unique_ptr<micro_flac::FLACEncoder> flac;
    std::unique_ptr<OpusEncoder, OpusDeleter> opus;
    if (format.codec == sched::Codec::FLAC) {
        flac = std::make_unique<micro_flac::FLACEncoder>(flac_input(format),
                                                         flac_options(shape.layout.samples));
    } else if (format.codec == sched::Codec::OPUS) {
        opus.reset(opus_create(format));
        if (!opus) {
            return nullptr;
        }
    }
    return std::unique_ptr<FrameEncoder>(
        new FrameEncoder(format, shape.layout.samples, std::move(flac), std::move(opus)));
}

void FrameEncoder::OpusDeleter::operator()(OpusEncoder* encoder) const {
    opus_destroy(encoder);
}

FrameEncoder::FrameEncoder(const sched::AudioFormat& format, uint32_t frame_samples,
                           std::unique_ptr<micro_flac::FLACEncoder> flac,
                           std::unique_ptr<OpusEncoder, OpusDeleter> opus)
    : format_(format),
      frame_samples_(frame_samples),
      flac_(std::move(flac)),
      opus_(std::move(opus)) {
    if (this->flac_ || this->opus_) {
        this->quantized_.resize(size_t{frame_samples} * format.channels *
                                pcm_output_bytes(quantized_depth(format)));
    }
    if (this->flac_) {
        this->encoded_.resize(this->flac_->get_max_output_bytes());
    }
    if (this->opus_) {
        this->encoded_.resize(OPUS_MAX_PACKET_BYTES);
        this->opus_input_.resize(size_t{frame_samples} * format.channels);
    }
}

FrameEncoder::~FrameEncoder() = default;

void FrameEncoder::reset() {
    if (this->flac_) {
        this->flac_->reset();
    }
    if (this->opus_) {
        opus_reset(this->opus_.get());
    }
}

uint32_t FrameEncoder::delay_samples() const {
    return this->opus_ ? opus_lookahead(this->opus_.get()) : 0;
}

bool FrameEncoder::encode(std::span<const uint8_t> input, SampleFormat in_format,
                          std::vector<uint8_t>& payload) {
    // Samples of every channel
    const size_t values = input.size() / sample_bytes(in_format);
    const uint32_t depth = quantized_depth(this->format_);
    const size_t bytes = values * pcm_output_bytes(depth);
    if (!this->flac_ && !this->opus_) {
        payload.resize(bytes);
        convert_pcm(input, in_format, depth, payload);
        return true;
    }
    payload.clear();
    if (bytes > this->quantized_.size()) {
        return false;
    }
    const std::span<uint8_t> quantized(this->quantized_.data(), bytes);
    convert_pcm(input, in_format, depth, quantized);
    return this->encode_quantized(quantized, payload);
}

bool FrameEncoder::encode_quantized(std::span<const uint8_t> samples,
                                    std::vector<uint8_t>& payload) {
    if (this->opus_) {
        return this->encode_opus(samples, payload);
    }
    if (!this->flac_) {
        payload.assign(samples.begin(), samples.end());
        return true;
    }
    payload.clear();
    const size_t full = this->quantized_.size();
    if (samples.size() > full) {
        return false;
    }
    size_t written = 0;
    micro_flac::FLACEncoderResult result = micro_flac::FLAC_ENCODER_SUCCESS;
    if (samples.size() == full) {
        size_t consumed = 0;
        result = this->flac_->encode(samples.data(), samples.size(), this->encoded_.data(),
                                     this->encoded_.size(), consumed, written);
    } else {
        // A short frame is the stream's last
        result = this->flac_->finish(samples.data(), samples.size(), this->encoded_.data(),
                                     this->encoded_.size(), written);
    }
    if (result != micro_flac::FLAC_ENCODER_SUCCESS || written == 0) {
        return false;
    }
    payload.assign(this->encoded_.begin(),
                   this->encoded_.begin() + static_cast<std::ptrdiff_t>(written));
    return true;
}

bool FrameEncoder::encode_opus(std::span<const uint8_t> samples, std::vector<uint8_t>& payload) {
    payload.clear();
    const size_t full = this->opus_input_.size() * sizeof(int16_t);
    if (samples.size() > full) {
        return false;
    }
    // Little-endian 16-bit samples, then silence to a whole frame: an Opus frame has a fixed
    // duration (spec player/v1.md "Codec framing": one packet per chunk)
    std::fill(this->opus_input_.begin(), this->opus_input_.end(), int16_t{0});
    for (size_t i = 0; i * 2 < samples.size(); ++i) {
        const auto value = static_cast<uint16_t>(samples[i * 2] | (samples[(i * 2) + 1] << 8));
        this->opus_input_[i] = static_cast<int16_t>(value);
    }
    const size_t written = opus_encode_frame(this->opus_.get(), this->opus_input_.data(),
                                             this->frame_samples_, this->encoded_);
    if (written == 0) {
        return false;
    }
    payload.assign(this->encoded_.begin(),
                   this->encoded_.begin() + static_cast<std::ptrdiff_t>(written));
    return true;
}

}  // namespace sendspin::server
