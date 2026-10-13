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

/// @file opus_reader.h
/// @brief For tests of Opus streams: libopus's decoder over a stream's packets as a player gets
/// them, the test tones, and how close decoded audio is to a reference, since a lossy codec is
/// never bit exact

#pragma once

#include <opus.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server::test {

/// @brief Decodes one Opus stream's packets, in order, to 16-bit interleaved samples
class OpusReader {
public:
    OpusReader(uint32_t rate, uint32_t channels) : channels_(channels) {
        int error = OPUS_OK;
        this->decoder_.reset(
            opus_decoder_create(static_cast<opus_int32>(rate), static_cast<int>(channels), &error));
        if (error != OPUS_OK) {
            this->decoder_.reset();
        }
    }

    /// @return the packet's samples, nullopt when it does not decode
    std::optional<std::vector<int16_t>> decode(std::span<const uint8_t> packet) {
        if (!this->decoder_) {
            return std::nullopt;
        }
        // The longest Opus packet: 120 ms at 48 kHz
        std::vector<int16_t> out(size_t{5760} * this->channels_);
        const int samples =
            opus_decode(this->decoder_.get(), packet.data(), static_cast<opus_int32>(packet.size()),
                        out.data(), 5760, 0);
        if (samples <= 0) {
            return std::nullopt;
        }
        out.resize(static_cast<size_t>(samples) * this->channels_);
        return out;
    }

private:
    struct Deleter {
        void operator()(OpusDecoder* decoder) const {
            opus_decoder_destroy(decoder);
        }
    };

    uint32_t channels_;
    std::unique_ptr<OpusDecoder, Deleter> decoder_;
};

/// @brief The test tones: 440 Hz on the left, 1,250 Hz on the right, at a third of full scale,
/// as 16-bit samples of frame position at rate
inline int16_t tone(uint64_t position, uint32_t channel, uint32_t rate) {
    const double t = static_cast<double>(position) / rate;
    const double hz = channel == 0 ? 440.0 : 1250.0;
    return static_cast<int16_t>(std::lround(10000.0 * std::sin(2 * std::numbers::pi * hz * t)));
}

/// @brief Stereo 16-bit little-endian frames of the tones from position first
inline std::vector<uint8_t> tone_frames(uint64_t first, size_t count, uint32_t rate) {
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < count; ++i) {
        for (uint32_t channel = 0; channel < 2; ++channel) {
            const auto value = static_cast<uint16_t>(tone(first + i, channel, rate));
            out[(i * 4) + (channel * 2)] = static_cast<uint8_t>(value);
            out[(i * 4) + (channel * 2) + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    return out;
}

/// @brief The signal-to-noise ratio in dB of decoded against reference, sample for sample, over
/// their common length; decoded is shifted by lag samples (positive: later)
inline double snr_db(std::span<const int16_t> reference, std::span<const int16_t> decoded,
                     int64_t lag = 0) {
    double signal = 0;
    double noise = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        const int64_t j = static_cast<int64_t>(i) + lag;
        if (j < 0 || static_cast<size_t>(j) >= decoded.size()) {
            continue;
        }
        const double r = reference[i];
        const double d = decoded[static_cast<size_t>(j)];
        signal += r * r;
        noise += (r - d) * (r - d);
    }
    return noise == 0 ? 200.0 : 10.0 * std::log10(signal / noise);
}

/// @brief The lag in [-reach, reach] interleaved samples, a multiple of channels, at which decoded
/// is closest to reference
inline int64_t best_lag(std::span<const int16_t> reference, std::span<const int16_t> decoded,
                        int64_t reach, uint32_t channels) {
    int64_t best = 0;
    double best_snr = -1e9;
    for (int64_t lag = -reach; lag <= reach; lag += channels) {
        const double snr = snr_db(reference, decoded, lag);
        if (snr > best_snr) {
            best_snr = snr;
            best = lag;
        }
    }
    return best;
}

}  // namespace sendspin::server::test
