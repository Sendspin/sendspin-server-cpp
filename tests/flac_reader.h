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

/// @file flac_reader.h
/// @brief A test's view of a FLAC stream as a player gets it: the stream/start codec_header, then
/// chunks of whole frames, decoded with micro-flac's decoder

#pragma once

#include "crypto/base64url.h"
#include <micro_flac/flac_decoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sendspin::server::test {

/// @brief The frame number a fixed-blocksize FLAC frame's header carries (RFC 9639 section 9.1.5,
/// coded like UTF-8); nullopt for a header that is not one
inline std::optional<uint64_t> flac_frame_number(std::span<const uint8_t> frame) {
    constexpr size_t NUMBER_AT = 4;
    // The sync code, with the fixed-blocksize strategy bit clear
    if (frame.size() <= NUMBER_AT || frame[0] != 0xFF || frame[1] != 0xF8) {
        return std::nullopt;
    }
    const uint8_t lead = frame[NUMBER_AT];
    size_t length = 0;
    while (length < 8 && (lead & (0x80U >> length)) != 0) {
        ++length;
    }
    if (length == 0) {
        return lead;
    }
    if (length == 1 || length > 7 || frame.size() < NUMBER_AT + length) {
        return std::nullopt;
    }
    uint64_t number = lead & (0x7FU >> length);
    for (size_t i = 1; i < length; ++i) {
        number = (number << 6) | (frame[NUMBER_AT + i] & 0x3FU);
    }
    return number;
}

/// @brief One frame of a chunk: its number and its samples, packed as the decoder writes them
struct DecodedFlacFrame {
    uint64_t number{0};
    std::vector<uint8_t> pcm;
};

/// @brief A decoder primed with a stream/start codec_header, which decodes chunk payloads
class FlacReader {
public:
    /// @return false when codec_header is not standard Base64 of `fLaC` and a STREAMINFO block
    bool open(std::string_view codec_header) {
        std::string url(codec_header);
        std::replace(url.begin(), url.end(), '+', '-');
        std::replace(url.begin(), url.end(), '/', '_');
        const std::optional<std::vector<uint8_t>> header = b64url_decode(url);
        // The standard alphabet, padding included
        if (!header || codec_header.size() % 4 != 0 ||
            codec_header.find_first_of("-_") != std::string_view::npos) {
            return false;
        }
        this->header = *header;
        this->decoder_ = std::make_unique<micro_flac::FLACDecoder>();
        size_t used = 0;
        size_t samples = 0;
        const micro_flac::FLACDecoderResult result = this->decoder_->decode(
            header->data(), header->size(), static_cast<uint8_t*>(nullptr), 0, used, samples);
        return result == micro_flac::FLAC_DECODER_HEADER_READY && used == header->size();
    }

    const micro_flac::FLACStreamInfo& info() const {
        return this->decoder_->get_stream_info();
    }

    /// @return every frame of payload in order; nullopt when one fails to decode or the payload
    /// does not end on a frame boundary
    std::optional<std::vector<DecodedFlacFrame>> decode(std::span<const uint8_t> payload) {
        std::vector<DecodedFlacFrame> frames;
        const micro_flac::FLACStreamInfo& stream = this->info();
        std::vector<uint8_t> out(size_t{stream.max_block_size()} * stream.num_channels() *
                                 stream.bytes_per_sample());
        size_t offset = 0;
        while (offset < payload.size()) {
            DecodedFlacFrame frame;
            const std::optional<uint64_t> number = flac_frame_number(payload.subspan(offset));
            if (!number) {
                return std::nullopt;
            }
            frame.number = *number;
            size_t used = 0;
            size_t samples = 0;
            const micro_flac::FLACDecoderResult result =
                this->decoder_->decode(payload.data() + offset, payload.size() - offset, out.data(),
                                       out.size(), used, samples);
            if (result != micro_flac::FLAC_DECODER_SUCCESS || used == 0) {
                return std::nullopt;
            }
            frame.pcm.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(
                                                            samples * stream.bytes_per_sample()));
            frames.push_back(std::move(frame));
            offset += used;
        }
        return frames;
    }

    std::vector<uint8_t> header;

private:
    std::unique_ptr<micro_flac::FLACDecoder> decoder_;
};

}  // namespace sendspin::server::test
