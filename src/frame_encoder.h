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

/// @file frame_encoder.h
/// @brief A pipeline instance's last stages: the quantize stage, then the PCM, FLAC or Opus
/// encoder, one frame per call; and what format selection and stream/start need of an output
/// format, its shape and its `codec_header`.

#pragma once

#include "sched/chunk.h"
#include "sched/types.h"
#include "sendspin/server/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace micro_flac {
class FLACEncoder;
}

struct OpusEncoder;

namespace sendspin::server {

/// @brief The largest Opus packet of one 20 ms frame: a code 0 packet's TOC byte and a frame of at
/// most 1,275 bytes (RFC 6716 section 3.2.1)
constexpr int64_t OPUS_MAX_PACKET_BYTES = 1276;

/// @brief An Opus stream's bit depth: the encoder's 16-bit input, which a player decodes to and
/// stream/start announces
constexpr uint8_t OPUS_BIT_DEPTH = 16;

/// @brief An output format's shape: PCM at 16, 24 or 32 bits; FLAC at 16 or 24 bits, its `B_f` the
/// encoder's get_max_output_bytes(); Opus at 8, 12, 16, 24 or 48 kHz, mono or stereo, any
/// `bit_depth` (spec player/v1.md: ignored for `opus`), its `B_f` OPUS_MAX_PACKET_BYTES. No other
/// codec, depth or Opus shape has an encoder.
sched::FormatShape output_shape(const sched::AudioFormat& format);

/// @brief The depth the quantize stage writes for format: its `bit_depth`, or 16 for Opus, whose
/// encoder takes 16-bit samples
uint32_t quantized_depth(const sched::AudioFormat& format);

/// @brief The encoder's delay in output samples: the Opus encoder's lookahead, `r / 400` (2.5 ms,
/// CELT alone, the restricted low-delay application); 0 for PCM and FLAC. A pipeline feeds the
/// encoder its output this far ahead, so a packet decodes to its own frame's samples.
uint32_t encoder_delay_samples(const sched::AudioFormat& format);

/// @brief encoder_delay_samples() in microseconds, rounded up
int64_t encoder_delay_us(const sched::AudioFormat& format);

/// @brief The stream/start `codec_header`: for FLAC, the `fLaC` marker and the STREAMINFO block in
/// standard Base64 (spec player/v1.md "Codec framing"); nullopt for any other codec
std::optional<std::string> codec_header(const sched::AudioFormat& format);

/// @brief The quantize stage and the encoder of one pipeline instance: the host's samples at
/// quantized_depth() (convert_pcm()), then as PCM, as one FLAC frame of the format's frame length,
/// or as one Opus packet of one 20 ms frame (spec player/v1.md "Codec framing")
class FrameEncoder {
public:
    /// @return nullptr when output_shape() has no encoder for format
    static std::unique_ptr<FrameEncoder> create(const sched::AudioFormat& format);

    ~FrameEncoder();
    FrameEncoder(const FrameEncoder&) = delete;
    FrameEncoder& operator=(const FrameEncoder&) = delete;

    /// @brief Starts a new stream: FLAC frame numbers restart at 0, the Opus encoder's state is
    /// cleared. At every epoch start.
    void reset();

    /// @brief The Opus encoder's lookahead as libopus reports it; 0 for PCM and FLAC
    uint32_t delay_samples() const;

    /// @brief Encodes one frame
    /// @param input whole samples in in_format at the format's channel count: one frame's, or
    /// fewer for a drained epoch's last frame, which ends a FLAC stream and which Opus pads with
    /// silence to a whole frame
    /// @return false when the encoder refused the call; payload is then empty
    bool encode(std::span<const uint8_t> input, SampleFormat in_format,
                std::vector<uint8_t>& payload);

    /// @brief encode() for samples the quantize stage (convert_pcm()) already wrote at
    /// quantized_depth()
    bool encode_quantized(std::span<const uint8_t> samples, std::vector<uint8_t>& payload);

private:
    struct OpusDeleter {
        void operator()(OpusEncoder* encoder) const;
    };

    FrameEncoder(const sched::AudioFormat& format, uint32_t frame_samples,
                 std::unique_ptr<micro_flac::FLACEncoder> flac,
                 std::unique_ptr<OpusEncoder, OpusDeleter> opus);

    bool encode_opus(std::span<const uint8_t> samples, std::vector<uint8_t>& payload);

    sched::AudioFormat format_;
    uint32_t frame_samples_;
    /// FLAC only
    std::unique_ptr<micro_flac::FLACEncoder> flac_;
    /// Opus only
    std::unique_ptr<OpusEncoder, OpusDeleter> opus_;
    /// FLAC and Opus: the quantize stage's output, one full frame's, and the encoder's, at its
    /// largest
    std::vector<uint8_t> quantized_;
    std::vector<uint8_t> encoded_;
    /// Opus only: one frame of 16-bit samples, aligned for the encoder
    std::vector<int16_t> opus_input_;
};

}  // namespace sendspin::server
