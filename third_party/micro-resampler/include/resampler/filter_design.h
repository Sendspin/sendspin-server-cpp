// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file filter_design.h
/// @brief Filter design configuration for the polyphase resampler
///
/// Declares ResamplerQuality (the VERY_LOW through LUDICROUS presets) and the
/// FilterDesign class, which centralizes FIR filter length, Kaiser beta,
/// downsampling/upsampling cutoff percentages, polyphase count, extrapolator
/// LPC order, and max-tap extraction settings used to build a resampler

#ifndef FILTER_DESIGN_H
#define FILTER_DESIGN_H

#include <cstddef>
#include <cstdint>

namespace micro_resampler {

enum class ResamplerQuality : uint8_t { VERY_LOW, LOW, MEDIUM, HIGH, VERY_HIGH, LUDICROUS };

// Centralized filter design parameters
// All filter-related configuration in one place
//
// FREQUENCY TERMINOLOGY CLARIFICATION:
// ------------------------------------
// This library uses normalized frequency representation where:
// - 0.0 = DC (0 Hz)
// - 0.5 = Nyquist frequency (sample_rate / 2)
// - 1.0 = Full sample rate
//
// Examples:
// - A normalized frequency of 0.45 means 45% of the sample rate, which is
//   90% of the Nyquist frequency
// - For 48kHz sample rate: 0.45 * 48000 = 21.6kHz
//
// WHICH SAMPLE RATE?
// ------------------
// The FIR filter cutoff is always limited to the lower Nyquist frequency:
// - For BOTH upsampling and downsampling: scaled by min_rate/max_rate
// - This preserves input content up to input Nyquist and prevents aliasing
//
class FilterDesign {
public:
    /// @brief Build a FilterDesign from one of the built-in quality presets
    /// Fills in filter length, Kaiser beta, cutoff percentages, and default phase count
    /// from the table of preset values (VERY_LOW through LUDICROUS)
    /// @param quality Quality preset to use
    /// @return a fully-populated FilterDesign for the requested preset
    static FilterDesign from_quality(ResamplerQuality quality);

    // Copy constructor and assignment for creating custom configs
    FilterDesign(const FilterDesign&) = default;
    FilterDesign& operator=(const FilterDesign&) = default;

    /// @brief Convert a frequency in Hz to this library's normalized frequency scale
    /// @param freq_hz Frequency in Hz
    /// @param sample_rate Sample rate in Hz that freq_hz is relative to
    /// @return the normalized frequency (0.0 = DC, 0.5 = Nyquist, 1.0 = sample_rate)
    static double hz_to_normalized(double freq_hz, double sample_rate) {
        return freq_hz / sample_rate;
    }

    /// @brief Convert a normalized frequency back to Hz
    /// @param normalized_freq Normalized frequency (0.0 = DC, 0.5 = Nyquist, 1.0 = sample_rate)
    /// @param sample_rate Sample rate in Hz that the result should be relative to
    /// @return the frequency in Hz
    static double normalized_to_hz(double normalized_freq, double sample_rate) {
        return normalized_freq * sample_rate;
    }

    /// @brief Get the recommended number of polyphase filter bank phases
    /// @return the default phase count; the resampler uses min(this value, the
    /// GCD-simplified output rate) unless is_force_phase_count() is true
    int get_default_phase_count() const {
        return this->default_phase_count_;
    }
    /// @brief Set the recommended number of polyphase filter bank phases
    /// @param count Default phase count to use (subject to GCD simplification and the
    /// 1-1024 phase range unless force phase count is enabled)
    void set_default_phase_count(int count) {
        this->default_phase_count_ = count;
    }

    // Cutoff frequency percentages of Nyquist for FIR filter
    // For downsampling: percentage of output Nyquist to use as cutoff
    // For upsampling: percentage of input Nyquist to use as cutoff
    /// @brief Get the FIR cutoff for downsampling conversions
    /// @return the cutoff as a fraction of output Nyquist (0-1.0)
    double get_downsampling_cutoff_percent() const {
        return this->downsampling_cutoff_percent_;
    }
    /// @brief Set the FIR cutoff for downsampling conversions
    /// @param percent Cutoff as a fraction of output Nyquist (0-1.0); clamped to that range (NaN
    /// becomes 0), which keeps every coefficient within the bound the max-tap path needs
    void set_downsampling_cutoff_percent(double percent) {
        this->downsampling_cutoff_percent_ = percent >= 0.0 ? (percent > 1.0 ? 1.0 : percent) : 0.0;
    }

    /// @brief Get the FIR filter length in taps per polyphase branch
    /// @return the configured filter length
    int get_filter_length() const {
        return this->filter_length_;
    }
    /// @brief Set the FIR filter length in taps per polyphase branch
    /// @param length Filter length; must be a positive multiple of 8 (the SIMD dot-product
    /// kernels work in 8-tap blocks), or MicroResampler::create() returns nullptr
    void set_filter_length(int length) {
        this->filter_length_ = length;
    }

    // Force phase count configuration
    // When true, uses exact default_phase_count instead of GCD-optimized count
    /// @brief Report whether the exact default phase count is forced
    /// @return true if the resampler must use get_default_phase_count() exactly instead of
    /// the GCD-optimized phase count
    bool is_force_phase_count() const {
        return this->force_phase_count_;
    }
    /// @brief Set whether the exact default phase count is forced
    /// @param force When true, bypass GCD-based phase optimization and use
    /// get_default_phase_count() exactly
    void set_force_phase_count(bool force) {
        this->force_phase_count_ = force;
    }

    /// @brief Get the Kaiser window beta parameter used to design the FIR filter
    /// @return the configured Kaiser beta
    double get_kaiser_beta() const {
        return this->kaiser_beta_;
    }
    /// @brief Set the Kaiser window beta parameter used to design the FIR filter
    /// @param beta Kaiser beta; higher values widen the transition band and improve
    /// stopband attenuation
    void set_kaiser_beta(double beta) {
        this->kaiser_beta_ = beta;
    }

    // Max tap extraction configuration
    // When enabled (with block adaptive Q), extracts the largest tap coefficients
    // per phase and handles them separately. This allows better quantization of
    // the remaining smaller coefficients in each SIMD block.
    /// @brief Report whether per-phase max tap extraction is enabled
    /// @return true if the largest coefficient(s) per phase are extracted and handled
    /// separately (requires block adaptive Q)
    bool is_max_tap_extraction_enabled() const {
        return this->extract_max_tap_per_phase_;
    }
    /// @brief Enable or disable per-phase max tap extraction
    /// @param enabled When true, extract the largest coefficient(s) per phase so the
    /// remaining coefficients in each SIMD block can be quantized more precisely
    void set_max_tap_extraction_enabled(bool enabled) {
        this->extract_max_tap_per_phase_ = enabled;
    }

    // Number of max taps to extract per phase (1-MAX_TAPS_PER_PHASE)
    // Higher values provide better precision for largest coefficients but use more memory.
    // Capped at 6 so the extracted taps' sum always fits the 32-bit Q30 accumulator: the 6
    // largest coefficients of a windowed-sinc phase sum to at most 1.96, but 7 can pass 2.0
    static constexpr int MAX_TAPS_PER_PHASE = 6;
    /// @brief Get the number of largest-magnitude taps extracted per phase
    /// @return the configured max-taps-per-phase count
    int get_max_taps_per_phase() const {
        return this->max_taps_per_phase_;
    }
    /// @brief Set the number of largest-magnitude taps extracted per phase
    /// @param count Desired count; clamped to the valid range [1, MAX_TAPS_PER_PHASE] before
    /// storing
    void set_max_taps_per_phase(int count) {
        if (count < 1) {
            count = 1;
        }
        if (count > MAX_TAPS_PER_PHASE) {
            count = MAX_TAPS_PER_PHASE;
        }
        this->max_taps_per_phase_ = count;
    }

    /// @brief Get the FIR cutoff for upsampling conversions
    /// @return the cutoff as a fraction of input Nyquist (0-1.0)
    double get_upsampling_cutoff_percent() const {
        return this->upsampling_cutoff_percent_;
    }
    /// @brief Set the FIR cutoff for upsampling conversions
    /// @param percent Cutoff as a fraction of input Nyquist (0-1.0); clamped to that range (NaN
    /// becomes 0), which keeps every coefficient within the bound the max-tap path needs
    void set_upsampling_cutoff_percent(double percent) {
        this->upsampling_cutoff_percent_ = percent >= 0.0 ? (percent > 1.0 ? 1.0 : percent) : 0.0;
    }

private:
    // Private constructor - use factory method
    FilterDesign() = default;

    // 64-bit fields
    double downsampling_cutoff_percent_{};  // Cutoff as percentage of output Nyquist (0-1.0)
    double kaiser_beta_{};
    double upsampling_cutoff_percent_{};  // Cutoff as percentage of input Nyquist (0-1.0)

    // 32-bit fields
    int default_phase_count_{};  // Recommended number of polyphase banks
    int filter_length_{};
    int max_taps_per_phase_ = 4;  // Default: 4 taps (experimentally validated)

    // 8-bit fields
    bool extract_max_tap_per_phase_ = true;  // Requires block adaptive Q
    bool force_phase_count_ = false;         // When true, bypass GCD optimization
};

}  // namespace micro_resampler

#endif  // FILTER_DESIGN_H
