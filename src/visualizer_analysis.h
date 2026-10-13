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

/// @file visualizer_analysis.h
/// @brief The visualizer's features computed from the group's PCM (spec roles/visualizer/v1.md
/// "Visualization Data (Binary)"): the dB scale, A-weighting, the spectrum's display bins, the
/// analysis instants, and VisualizerAnalysis, one stream configuration's analysis state. Single
/// precision throughout; the FFT is fft.h's seam.

#pragma once

#include "fft.h"
#include "visualizer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief The analysis window, and FFT size, for input at rate: the power of two at or above
/// REFERENCE_WINDOW samples at REFERENCE_RATE scaled to rate, so the window spans about 43 ms at
/// any rate, within RealFft's sizes
size_t analysis_window(uint32_t rate);

/// @brief The window at REFERENCE_RATE: 2048 samples, a bin about 23 Hz wide
constexpr size_t REFERENCE_WINDOW = 2048;
constexpr uint32_t REFERENCE_RATE = 48000;

/// @brief The spec's amplitude scale as a fraction: `value / reference` in dB, -60 dB giving 0 and
/// 0 dB giving 1, linear in dB between and clamped outside (spec roles/visualizer/v1.md
/// "Visualization Data (Binary)")
float db_unit(float value, float reference);

/// @brief A fraction from 0 to 1 on the uint16 range, rounded down: 0 is silence, 65535 full scale
uint16_t unit_to_u16(float unit);

/// @brief The A-weighting gain at hz in dB (IEC 61672-1), clamped to -50 to +6 dB; hz below 1 is
/// taken as 1
float a_weighting_db(float hz);

/// @brief The n_disp_bins + 1 edges, in Hz, of the spectrum's display bins from f_min to f_max,
/// spaced evenly in frequency (lin), in log10 of frequency from max(f_min, 1) (log), or in HTK mel,
/// `2595 * log10(1 + f / 700)` (mel) (spec roles/visualizer/v1.md "client/state visualizer
/// object")
/// @param edges room for n_disp_bins + 1 values
void spectrum_edges(const SpectrumConfig& spectrum, std::span<float> edges);

/// @brief The display bin holding hz: the i with `edges[i] <= hz < edges[i + 1]`; -1 outside
/// [edges.front(), edges.back())
int32_t spectrum_bin(float hz, std::span<const float> edges);

/// @brief The periodic analysis instant in cell, as microseconds from the epoch's `T0`: the
/// `floor(j * 10^6 / rate)` for `j = 0, 1, ...` that falls in the cell's 20 ms, if one does; at
/// most one, rate being at most MAX_VISUALIZER_RATE
std::optional<int64_t> periodic_instant_us(int64_t cell, uint32_t rate);

/// @brief One visualizer stream configuration's analysis: a window of mono history, the FFT, the
/// tables for the input's rate and the smoothing state. Its buffers are allocated when it is built
/// and when the input's rate changes (set_rate()); push() and analyze() allocate nothing but the
/// messages they append. Not thread-safe.
///
/// At each instant, the window of the analysis_window() samples before it (Hann-windowed, DC
/// dropped) gives, A-weighted: `loudness`, the RMS against a full-scale sine's, smoothed; `f_peak`,
/// the strongest bin with parabolic interpolation, kept on its bin while another is under
/// F_PEAK_SWITCH_RATIO above it; `spectrum`, each display bin's RMS against a full-scale sine's
/// peak, smoothed; and `peak`, an onset when the energy passes ONSET_RATIO times its running
/// average, at most one per ONSET_GAP_US.
class VisualizerAnalysis {
public:
    /// Smoothing factors, the weight of the new value
    static constexpr float LOUDNESS_SMOOTHING = 0.5F;
    static constexpr float SPECTRUM_SMOOTHING = 0.4F;
    /// f_peak keeps its bin unless another is this much stronger, in magnitude
    static constexpr float F_PEAK_SWITCH_RATIO = 1.1F;
    /// An onset: energy above this multiple of its running average
    static constexpr float ONSET_RATIO = 1.6F;
    /// The least time between two `peak` messages
    static constexpr int64_t ONSET_GAP_US = 80000;

    explicit VisualizerAnalysis(const VisualizerConfig& config);

    const VisualizerConfig& config() const {
        return this->config_;
    }

    /// @brief Builds the FFT and the tables for input at rate, the history zeros; allocates, so
    /// only when the rate changes
    /// @return false when the FFT cannot be built
    bool set_rate(uint32_t rate);

    /// @brief The rate set_rate() last built for; 0 before
    uint32_t rate() const {
        return this->rate_;
    }

    /// @brief The window, and FFT size, for rate()
    size_t window() const {
        return this->window_;
    }

    /// @brief A new epoch: the history zeros, the smoothing and onset state cleared
    void reset();

    /// @brief Appends mono samples, the newest last
    void push(std::span<const float> mono);

    /// @brief Analyzes the window ending at the newest sample pushed and appends its messages to
    /// out, each `[type][ts][data]` (spec roles/visualizer/v1.md "Visualization Data (Binary)"),
    /// in type order
    /// @param periodic the configuration's periodic types are due
    /// @param onset an onset check is due, which appends a `peak` message when one fires
    void analyze(int64_t ts_us, bool periodic, bool onset, std::vector<uint8_t>& out);

private:
    /// The FFT's power of the window per bin, A-weighted, as weighted_, and its sum
    void transform();
    void append_header(uint8_t type, int64_t ts_us, std::vector<uint8_t>& out) const;
    uint16_t loudness();
    void f_peak(uint16_t& hz, uint16_t& amplitude);
    void spectrum();
    /// The onset strength, from 1 to 255, when one fires at ts_us
    std::optional<uint8_t> onset(int64_t ts_us);

    VisualizerConfig config_;
    uint32_t bins_{0};
    uint32_t rate_{0};
    size_t window_{0};
    std::unique_ptr<RealFft> fft_;
    /// The first half of the symmetric Hann window, the A-weighting power gains per FFT bin, and
    /// each FFT bin's display bin (-1 for none)
    std::vector<float> hann_;
    std::vector<float> gains_;
    std::vector<int32_t> display_bin_;
    /// The newest window_ samples, a ring whose oldest is at history_at_
    std::vector<float> history_;
    size_t history_at_{0};
    /// The A-weighted power per FFT bin, in the FFT's storage, and their sum
    std::span<float> weighted_;
    float power_{0.0F};
    std::vector<float> edges_;
    std::vector<float> bin_power_;
    std::vector<float> spectrum_smoothed_;
    std::vector<uint16_t> spectrum_out_;
    bool spectrum_started_{false};
    std::optional<float> loudness_smoothed_;
    /// The FFT bin f_peak reported last; -1 for none
    int64_t f_peak_bin_{-1};
    /// The onset detector's running energy; 0 before its first value
    float energy_average_{0.0F};
    std::optional<int64_t> last_onset_us_;
};

}  // namespace sendspin::server
