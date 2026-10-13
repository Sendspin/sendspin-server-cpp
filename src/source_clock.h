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

/// @file source_clock.h
/// @brief A routed source's clock against the server's (roles/source/v1.md "Source Audio Chunks
/// (Binary)": estimate the source's effective rate, absorb its deviation by resampling).
/// SourceClock measures how the corrected audio runs against the chunks' capture times and steers
/// a correction in ppm; SampleSlip applies it by dropping or repeating single frames, SourceAsrc
/// with micro-resampler's ASRC.

#pragma once

#include "memory.h"
#include "sendspin/server/config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace micro_resampler {
class MicroResampler;
}

namespace sendspin::server {

/// @brief The audio SourceClock averages the phase error over before each step of its loop
constexpr int64_t SOURCE_CLOCK_WINDOW_US = 1000000;
/// @brief A phase error that moves this far from the last one taken is a step in the source's time
/// mapping, not drift: absorbed once the next observation confirms it, else ignored
constexpr int64_t SOURCE_CLOCK_STEP_US = 5000;
/// @brief The correction starts once a window's average phase error reaches this, so a source
/// whose clock keeps the server's passes through unaltered
constexpr int64_t SOURCE_CLOCK_ENGAGE_US = 2000;
/// @brief The largest correction, in ppm; well within the spec's 0.5% (player/v1.md "Correction
/// Quality")
constexpr double SOURCE_CLOCK_MAX_PPM = 500.0;
/// @brief The loop's gains: ppm per microsecond of averaged phase error, and per microsecond-second
/// of its integral
constexpr double SOURCE_CLOCK_KP = 0.05;
constexpr double SOURCE_CLOCK_KI = 0.0015;

/// @brief The phase loop of one source's stream. A chunk's phase error is its first frame's capture
/// time less the time its corrected position plays at the stream's nominal rate, both measured from
/// a reference that absorbs steps and the audio dropped before correction. Pure: no clock reads.
class SourceClock {
public:
    /// @brief One chunk, before its frames are corrected
    /// @param capture_us its first frame's capture time
    /// @param position its first frame's position in the corrected audio, in frames at rate
    /// @param frames its frames
    /// @return the correction from this chunk on, in ppm: positive drops audio, negative adds
    double observe(int64_t capture_us, double position, uint32_t rate, size_t frames);

    /// @brief The source's audio of duration_us was dropped before correction: its capture times
    /// move on without its frames
    void skip(int64_t duration_us);

    /// @brief The corrected positions restart: the next chunk sets a new reference. The loop's
    /// estimate of the source's rate stays.
    void restart();

    /// @brief Whether the correction has started (SOURCE_CLOCK_ENGAGE_US)
    bool engaged() const {
        return this->engaged_;
    }

    double ppm() const {
        return this->ppm_;
    }

private:
    void clear_window();

    /// Capture time less corrected time at the reference; nullopt before the first chunk
    std::optional<double> offset_us_;
    /// The first window after a reference only centers it, so the reference chunk's own jitter
    /// does not bias every later error
    bool centered_{false};
    /// The last phase error taken into the window
    double last_us_{0.0};
    /// An unconfirmed step
    std::optional<double> step_us_;
    double window_sum_{0.0};
    int64_t window_frames_{0};
    double integral_{0.0};
    double ppm_{0.0};
    bool engaged_{false};
};

/// @brief Drops or repeats single frames at a rate in ppm: one each 10^6 / |ppm| frames, dropping
/// for a positive one, as the spec suggests to players (player/v1.md "Sample deletion and
/// insertion"). Any sample format: frames are copied whole.
struct SampleSlip {
    /// Frames passed since the last slip, times ppm; a slip at +-10^6
    int64_t phase{0};

    /// @brief The most frames slip() writes for frames in
    static size_t max_output(size_t frames);

    /// @brief Copies frames frames of frame_bytes each from in to out, slipping as due
    /// @param out room for max_output(frames) frames
    /// @return the frames written
    size_t slip(const uint8_t* in, size_t frames, size_t frame_bytes, int32_t ppm, uint8_t* out);
};

/// @brief micro-resampler's ASRC at one rate, floats in and out, its ratio set from a ppm
/// correction before each block; its first output frame lines up with its first input frame
/// (primed). Tracks where the next input frame lands in its output.
class SourceAsrc {
public:
    /// @param max_block the most input frames micro-resampler is fed at once; its history buffer is
    /// sized for it, so process() never reallocates
    /// @return nullptr when micro-resampler refuses or an allocation fails
    static std::unique_ptr<SourceAsrc> create(uint32_t rate, uint32_t channels,
                                              ResamplerQuality quality, size_t max_block);

    ~SourceAsrc();
    SourceAsrc(const SourceAsrc&) = delete;
    SourceAsrc& operator=(const SourceAsrc&) = delete;

    /// @brief The most frames process() writes for frames in
    static size_t max_output(size_t frames);

    /// @brief Resamples frames frames at a ratio of 1 + ppm * 10^-6 input frames per output frame
    /// @param out room for max_output(frames) frames
    /// @return the frames written, or nullopt when micro-resampler refused
    std::optional<size_t> process(const float* in, size_t frames, double ppm, float* out);

    /// @brief Starts a new stream: empty history, primed again by the next block, positions from 0
    void reset();

    /// @brief The output position the next input frame lands at: the frames written so far, plus
    /// what the filter holds at the current ratio
    double next_position() const;

    /// @brief The output frames written since create()
    uint64_t produced() const {
        return this->produced_;
    }

private:
    SourceAsrc(std::unique_ptr<micro_resampler::MicroResampler> resampler, uint32_t channels,
               size_t max_block);

    std::unique_ptr<micro_resampler::MicroResampler> resampler_;
    uint32_t channels_;
    size_t max_block_;
    bool primed_{false};
    /// The ratio in micro-resampler's 4.28 fixed point
    uint32_t ratio_;
    uint64_t produced_{0};
    /// Input taken less the output read positions advanced, in 4.28 fixed point: the input the
    /// filter holds past the next output frame
    int64_t held_{0};
};

}  // namespace sendspin::server
