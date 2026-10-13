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

#include "visualizer_analysis.h"

#include "sched/constants.h"
#include "sched/timeline.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sendspin::server {

namespace {

/// The dB range the uint16 scale spans (spec roles/visualizer/v1.md "Visualization Data
/// (Binary)"): -60 dB is 0
constexpr float DB_FLOOR = -60.0F;
/// The smallest ratio db_unit() takes the log of
constexpr float MIN_RATIO = 1e-10F;
constexpr float U16_FULL = 65535.0F;
/// The A-weighting gain's clamp
constexpr float A_WEIGHT_MIN_DB = -50.0F;
constexpr float A_WEIGHT_MAX_DB = 6.0F;
/// A Hann-windowed full-scale sine's RMS as loudness measures it: sqrt(3 / 16)
const float LOUDNESS_REFERENCE = std::sqrt(3.0F / 16.0F);
/// The onset strength's range: the excess over ONSET_RATIO that gives 255
constexpr float ONSET_STRENGTH_RANGE = 6.0F;
/// The onset average's weight of a new value, outside and at an onset
constexpr float ONSET_AVERAGE_WEIGHT = 0.1F;
constexpr float ONSET_ATTACK_WEIGHT = 0.4F;
/// The energy the onset average starts from when the first is 0
constexpr float MIN_ENERGY = 1e-12F;

float hz_to_mel(float hz) {
    return 2595.0F * std::log10(1.0F + (hz / 700.0F));
}

float mel_to_hz(float mel) {
    return 700.0F * (std::pow(10.0F, mel / 2595.0F) - 1.0F);
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

}  // namespace

// ==============================================================================
// Scales and bins
// ==============================================================================

size_t analysis_window(uint32_t rate) {
    const uint64_t target =
        sched::ceil_div(static_cast<int64_t>(REFERENCE_WINDOW) * rate, REFERENCE_RATE);
    size_t size = RealFft::MIN_SIZE;
    while (size < target && size < RealFft::MAX_SIZE) {
        size *= 2;
    }
    return size;
}

float db_unit(float value, float reference) {
    const float ratio = std::max(value / reference, MIN_RATIO);
    const float db = 20.0F * std::log10(ratio);
    return std::clamp((db - DB_FLOOR) / -DB_FLOOR, 0.0F, 1.0F);
}

uint16_t unit_to_u16(float unit) {
    return static_cast<uint16_t>(std::clamp(unit, 0.0F, 1.0F) * U16_FULL);
}

float a_weighting_db(float hz) {
    const float f2 = std::max(hz, 1.0F) * std::max(hz, 1.0F);
    constexpr float C1 = 20.6F * 20.6F;
    constexpr float C2 = 107.7F * 107.7F;
    constexpr float C3 = 737.9F * 737.9F;
    constexpr float C4 = 12194.0F * 12194.0F;
    const float ra = (C4 * f2 * f2) / ((f2 + C1) * std::sqrt((f2 + C2) * (f2 + C3)) * (f2 + C4));
    // IEC 61672-1: +2.00 dB normalizes the curve to 0 dB at 1 kHz
    const float db = 2.0F + (20.0F * std::log10(std::max(ra, 1e-12F)));
    return std::clamp(db, A_WEIGHT_MIN_DB, A_WEIGHT_MAX_DB);
}

void spectrum_edges(const SpectrumConfig& spectrum, std::span<float> edges) {
    const uint32_t bins = spectrum.n_disp_bins;
    const auto low = static_cast<float>(spectrum.f_min);
    const auto high = static_cast<float>(spectrum.f_max);
    float from = low;
    float to = high;
    if (spectrum.scale == SpectrumScale::LOG) {
        from = std::log10(std::max(low, 1.0F));
        to = std::log10(high);
    } else if (spectrum.scale == SpectrumScale::MEL) {
        from = hz_to_mel(low);
        to = hz_to_mel(high);
    }
    for (uint32_t i = 0; i <= bins; ++i) {
        const float at = from + ((to - from) * static_cast<float>(i) / static_cast<float>(bins));
        switch (spectrum.scale) {
            case SpectrumScale::LIN:
                edges[i] = at;
                break;
            case SpectrumScale::LOG:
                edges[i] = std::pow(10.0F, at);
                break;
            case SpectrumScale::MEL:
                edges[i] = mel_to_hz(at);
                break;
        }
    }
}

int32_t spectrum_bin(float hz, std::span<const float> edges) {
    const auto above = std::upper_bound(edges.begin(), edges.end(), hz);
    const auto bin = static_cast<int32_t>(above - edges.begin()) - 1;
    return bin >= 0 && bin < static_cast<int32_t>(edges.size()) - 1 ? bin : -1;
}

std::optional<int64_t> periodic_instant_us(int64_t cell, uint32_t rate) {
    if (rate == 0 || cell < 0) {
        return std::nullopt;
    }
    // The first j with floor(j * 10^6 / rate) >= cell * F is ceil(cell * rate / 50)
    const int64_t j = sched::ceil_div(cell * rate, sched::CELLS_PER_SECOND);
    const int64_t at = sched::floor_div(j * sched::US_PER_SECOND, rate);
    if (at >= (cell + 1) * sched::CELL_US) {
        return std::nullopt;
    }
    return at;
}

// ==============================================================================
// VisualizerAnalysis
// ==============================================================================

VisualizerAnalysis::VisualizerAnalysis(const VisualizerConfig& config) : config_(config) {
    if (config.spectrum) {
        this->bins_ = config.spectrum->n_disp_bins;
        this->edges_.resize(size_t{this->bins_} + 1);
        spectrum_edges(*config.spectrum, this->edges_);
        this->bin_power_.resize(this->bins_);
        this->spectrum_smoothed_.resize(this->bins_);
        this->spectrum_out_.resize(this->bins_);
    }
}

bool VisualizerAnalysis::set_rate(uint32_t rate) {
    const size_t window = analysis_window(rate);
    // The powers live in the FFT's storage
    this->weighted_ = {};
    if (!this->fft_ || this->fft_->size() != window) {
        this->fft_ = RealFft::create(window);
        if (!this->fft_) {
            this->rate_ = 0;
            return false;
        }
    }
    this->rate_ = rate;
    this->window_ = window;
    const size_t half = window / 2;
    // Symmetric, `w[i] = w[window - 1 - i]`: the first half is the whole window
    this->hann_.resize(half);
    for (size_t i = 0; i < half; ++i) {
        const float phase = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) /
                            static_cast<float>(window - 1);
        this->hann_[i] = 0.5F - (0.5F * std::cos(phase));
    }
    this->gains_.resize(half + 1);
    this->display_bin_.assign(half + 1, -1);
    for (size_t k = 0; k <= half; ++k) {
        const float hz =
            static_cast<float>(k) * static_cast<float>(rate) / static_cast<float>(window);
        // A power gain: the A-weighting's amplitude gain squared
        this->gains_[k] = std::pow(10.0F, a_weighting_db(hz) / 10.0F);
        if (this->bins_ > 0) {
            this->display_bin_[k] = spectrum_bin(hz, this->edges_);
        }
    }
    this->history_.assign(window, 0.0F);
    this->history_at_ = 0;
    // The bin f_peak held is another frequency at another size
    this->f_peak_bin_ = -1;
    return true;
}

void VisualizerAnalysis::reset() {
    std::fill(this->history_.begin(), this->history_.end(), 0.0F);
    this->history_at_ = 0;
    this->spectrum_started_ = false;
    this->loudness_smoothed_.reset();
    this->f_peak_bin_ = -1;
    this->energy_average_ = 0.0F;
    this->last_onset_us_.reset();
}

void VisualizerAnalysis::push(std::span<const float> mono) {
    const size_t window = this->window_;
    // Only the newest window counts
    if (mono.size() > window) {
        mono = mono.last(window);
    }
    while (!mono.empty()) {
        const size_t take = std::min(mono.size(), window - this->history_at_);
        std::copy_n(mono.begin(), take,
                    this->history_.begin() + static_cast<std::ptrdiff_t>(this->history_at_));
        this->history_at_ = (this->history_at_ + take) % window;
        mono = mono.subspan(take);
    }
}

void VisualizerAnalysis::analyze(int64_t ts_us, bool periodic, bool onset,
                                 std::vector<uint8_t>& out) {
    const uint8_t types = this->config_.types;
    const bool periodic_due = periodic && (types & visualizer_type::PERIODIC) != 0;
    const bool onset_due = onset && (types & visualizer_type::PEAK) != 0;
    if (!periodic_due && !onset_due) {
        return;
    }
    this->transform();
    if (periodic_due && (types & visualizer_type::LOUDNESS) != 0) {
        const uint16_t value = this->loudness();
        this->append_header(visualizer_type::LOUDNESS, ts_us, out);
        put_u16(out, value);
    }
    if (periodic_due && (types & visualizer_type::F_PEAK) != 0) {
        uint16_t hz = 0;
        uint16_t amplitude = 0;
        this->f_peak(hz, amplitude);
        this->append_header(visualizer_type::F_PEAK, ts_us, out);
        put_u16(out, hz);
        put_u16(out, amplitude);
    }
    if (periodic_due && (types & visualizer_type::SPECTRUM) != 0) {
        this->spectrum();
        this->append_header(visualizer_type::SPECTRUM, ts_us, out);
        for (const uint16_t value : this->spectrum_out_) {
            put_u16(out, value);
        }
    }
    if (onset_due) {
        if (const std::optional<uint8_t> strength = this->onset(ts_us)) {
            this->append_header(visualizer_type::PEAK, ts_us, out);
            out.push_back(*strength);
        }
    }
}

void VisualizerAnalysis::transform() {
    const size_t window = this->window_;
    const size_t half = window / 2;
    const std::span<float> input = this->fft_->input();
    // The ring from its oldest sample into the FFT's input, then windowed, each half by the
    // symmetric window's half
    const auto oldest = this->history_.begin() + static_cast<std::ptrdiff_t>(this->history_at_);
    const auto copied = std::copy(oldest, this->history_.end(), input.begin());
    std::copy(this->history_.begin(), oldest, copied);
    for (size_t i = 0; i < half; ++i) {
        input[i] *= this->hann_[i];
        input[window - 1 - i] *= this->hann_[i];
    }
    this->weighted_ = this->fft_->power();
    // DC carries no frequency to show
    this->weighted_[0] = 0.0F;
    float power = 0.0F;
    for (size_t k = 1; k < this->weighted_.size(); ++k) {
        this->weighted_[k] *= this->gains_[k];
        power += this->weighted_[k];
    }
    this->power_ = power;
}

void VisualizerAnalysis::append_header(uint8_t type, int64_t ts_us,
                                       std::vector<uint8_t>& out) const {
    uint8_t id = VISUALIZER_FIRST_MESSAGE_ID;
    for (uint8_t bit = type; bit > 1; bit >>= 1) {
        ++id;
    }
    out.push_back(id);
    const auto ts = static_cast<uint64_t>(ts_us);
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<uint8_t>(ts >> shift));
    }
}

uint16_t VisualizerAnalysis::loudness() {
    // Parseval over the one-sided spectrum: the window's RMS
    const float rms = std::sqrt(2.0F * this->power_) / static_cast<float>(this->window_);
    const float unit = db_unit(rms, LOUDNESS_REFERENCE);
    this->loudness_smoothed_ = this->loudness_smoothed_
                                   ? (LOUDNESS_SMOOTHING * unit) +
                                         ((1.0F - LOUDNESS_SMOOTHING) * *this->loudness_smoothed_)
                                   : unit;
    return unit_to_u16(*this->loudness_smoothed_);
}

void VisualizerAnalysis::f_peak(uint16_t& hz, uint16_t& amplitude) {
    const std::span<const float> weighted = this->weighted_;
    auto bin =
        static_cast<int64_t>(std::max_element(weighted.begin(), weighted.end()) - weighted.begin());
    const int64_t held = this->f_peak_bin_;
    // The ratio of magnitudes, squared for the powers
    constexpr float SWITCH_POWER_RATIO = F_PEAK_SWITCH_RATIO * F_PEAK_SWITCH_RATIO;
    if (held >= 0 && bin != held && weighted[bin] < SWITCH_POWER_RATIO * weighted[held]) {
        bin = held;
    }
    this->f_peak_bin_ = bin;
    hz = 0;
    amplitude = 0;
    if (weighted[bin] <= 0.0F) {
        return;
    }
    const float bin_hz = static_cast<float>(this->rate_) / static_cast<float>(this->window_);
    float frequency = static_cast<float>(bin) * bin_hz;
    // The interpolation runs on magnitudes: the square roots of these three bins only
    float magnitude = std::sqrt(weighted[bin]);
    const auto size = static_cast<int64_t>(weighted.size());
    if (bin > 0 && bin < size - 1) {
        const float a = std::sqrt(weighted[bin - 1]);
        const float b = magnitude;
        const float c = std::sqrt(weighted[bin + 1]);
        const float curvature = a - (2.0F * b) + c;
        if (std::fabs(curvature) > 1e-12F) {
            // The vertex of the parabola through the three bins, within a bin of the peak
            const float delta = std::clamp(0.5F * (a - c) / curvature, -1.0F, 1.0F);
            frequency += delta * bin_hz;
            magnitude = b - (0.25F * (a - c) * delta);
        }
    }
    hz = static_cast<uint16_t>(std::clamp(std::round(frequency), 0.0F, U16_FULL));
    // A full-scale sine's peak through the Hann window is a quarter of the window
    const float reference = static_cast<float>(this->window_) / 4.0F;
    // spec roles/visualizer/v1.md "f_peak": no peak detected carries an amplitude of 0
    amplitude = hz == 0 ? 0 : unit_to_u16(db_unit(magnitude, reference));
}

void VisualizerAnalysis::spectrum() {
    std::fill(this->bin_power_.begin(), this->bin_power_.end(), 0.0F);
    for (size_t k = 0; k < this->weighted_.size(); ++k) {
        const int32_t bin = this->display_bin_[k];
        if (bin >= 0) {
            this->bin_power_[static_cast<size_t>(bin)] += this->weighted_[k];
        }
    }
    const float reference = static_cast<float>(this->window_) / 4.0F;
    for (size_t i = 0; i < this->bins_; ++i) {
        // Quantized before smoothing, so the smoothing starts from what was shown
        const float unit =
            static_cast<float>(unit_to_u16(db_unit(std::sqrt(this->bin_power_[i]), reference))) /
            U16_FULL;
        float& smoothed = this->spectrum_smoothed_[i];
        smoothed = this->spectrum_started_
                       ? (SPECTRUM_SMOOTHING * unit) + ((1.0F - SPECTRUM_SMOOTHING) * smoothed)
                       : unit;
        this->spectrum_out_[i] = unit_to_u16(smoothed);
    }
    this->spectrum_started_ = true;
}

std::optional<uint8_t> VisualizerAnalysis::onset(int64_t ts_us) {
    const float energy = this->power_;
    const float average = this->energy_average_;
    if (average <= 0.0F) {
        this->energy_average_ = std::max(energy, MIN_ENERGY);
        return std::nullopt;
    }
    const bool too_soon = this->last_onset_us_ && ts_us - *this->last_onset_us_ < ONSET_GAP_US;
    if (too_soon || energy <= ONSET_RATIO * average) {
        this->energy_average_ =
            ((1.0F - ONSET_AVERAGE_WEIGHT) * average) + (ONSET_AVERAGE_WEIGHT * energy);
        return std::nullopt;
    }
    // The excess over the threshold, ONSET_STRENGTH_RANGE times the average giving 255
    const float excess = (energy / average) - ONSET_RATIO;
    const auto strength =
        static_cast<uint8_t>(std::clamp(excess / ONSET_STRENGTH_RANGE, 0.0F, 1.0F) * 255.0F);
    // A fast attack, so a sustained rise fires once
    this->energy_average_ =
        ((1.0F - ONSET_ATTACK_WEIGHT) * average) + (ONSET_ATTACK_WEIGHT * energy);
    this->last_onset_us_ = ts_us;
    return std::max<uint8_t>(strength, 1);
}

}  // namespace sendspin::server
