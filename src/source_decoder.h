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

/// @file source_decoder.h
/// @brief SourceDecoder: a routed source's job on the group audio threads, which decodes its
/// input stream's PCM, FLAC (micro-flac) or Opus (libopus, from micro-opus) chunks, checks their
/// framing and duration, and hands the PCM to its SourcePcmSink

#pragma once

#include "memory.h"
#include "messages.h"
#include "sendspin/server/types.h"
#include "source_feed.h"
#include "source_sink.h"
#include "transport.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace micro_flac {
class FLACDecoder;
}

struct OpusDecoder;

namespace sendspin::server {

/// @brief Whether this build decodes Opus (SENDSPIN_SERVER_ENABLE_OPUS)
bool source_opus_available();

/// @brief The PCM a source's stream decodes to, which its group's input takes: PCM and FLAC at
/// 16 and 24 bits as S16 and S24, bit for bit; any other FLAC depth, and 32-bit PCM, as F32; Opus
/// as S16. nullopt for a stream the server does not take: PCM at another depth, FLAC past 32
/// bits, Opus at a rate libopus does not decode (8, 12, 16, 24 or 48 kHz) or with more than two
/// channels, or Opus without source_opus_available(), or a format a GroupInput does not take
/// (InputFormat).
std::optional<InputFormat> source_input_format(const AudioFormat& format);

/// @brief Runs one item of its feed per call, on whichever group audio thread runs its task.
/// Reports what it cannot take to core through the feed and faults, a Wakeup on core that a
/// signal after it is gone leaves alone.
class SourceDecoder {
public:
    SourceDecoder(std::shared_ptr<SourceFeed> feed, std::unique_ptr<SourcePcmSink> sink,
                  Wakeup::Handle faults);
    ~SourceDecoder();
    SourceDecoder(const SourceDecoder&) = delete;
    SourceDecoder& operator=(const SourceDecoder&) = delete;

    /// @brief Takes one item of the feed
    /// @return whether another waits
    bool run();

private:
    struct OpusDeleter {
        void operator()(OpusDecoder* decoder) const;
    };

    /// What decoding a chunk came to
    enum class Decoded : uint8_t {
        /// Its audio went to the sink
        TAKEN,
        /// It breaks the stream's framing or duration: a protocol error
        BROKEN,
        /// The heap could not give the decoder what it needed: the stream is refused
        REFUSED,
    };

    /// @return false when the stream cannot be taken
    bool start(const SourceItem& item, std::span<const uint8_t> header);
    /// Reads a FLAC stream's header, its block sizes capped at max_frames
    /// @return the largest block the decoder takes; 0 for a header it cannot take
    size_t start_flac(std::span<const uint8_t> header, size_t max_frames);
    /// Makes buffer_ at least bytes long
    /// @return false when the heap cannot give it
    bool reserve(size_t bytes);
    Decoded decode(const SourceItem& item, std::span<const uint8_t> audio);
    Decoded decode_pcm(const SourceItem& item, std::span<const uint8_t> audio);
    Decoded decode_flac(const SourceItem& item, std::span<const uint8_t> audio);
    Decoded decode_opus(const SourceItem& item, std::span<const uint8_t> audio);
    /// Hands a chunk's frames to the sink in one write; BROKEN past the longest chunk
    Decoded emit(const SourceItem& item, const uint8_t* pcm, size_t frames);
    /// Turns buffer_'s first samples, left-justified 32-bit integers, into floats in place
    void widen(size_t samples);
    void report(SourceFault::Kind kind, const SourceItem& item);

    std::shared_ptr<SourceFeed> feed_;
    std::unique_ptr<SourcePcmSink> sink_;
    Wakeup::Handle faults_;
    /// The stream being decoded and its format: nullopt with none, or after it failed
    std::optional<AudioFormat> stream_;
    InputFormat output_;
    uint64_t connection_{0};
    uint64_t stream_number_{0};
    std::unique_ptr<micro_flac::FLACDecoder> flac_;
    std::unique_ptr<OpusDecoder, OpusDeleter> opus_;
    /// A decoded chunk in output_'s format, FLAC's 32-bit samples before they become floats, or
    /// 32-bit PCM's floats; uninitialized, each chunk writing what it reads; in PSRAM where there
    /// is one
    PlacedArray<uint8_t> buffer_;
    size_t buffer_bytes_{0};
    /// The frames buffer_ holds for a FLAC stream: a chunk's longest and one largest block
    size_t flac_frames_{0};
};

}  // namespace sendspin::server
