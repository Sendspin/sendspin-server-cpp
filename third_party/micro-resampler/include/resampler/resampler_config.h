// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file resampler_config.h
/// @brief Immutable-after-construction configuration for MicroResampler
///
/// Declares CoefficientFormat (processing arithmetic), ExtrapolationMode
/// (prime/flush signal extension), PrecisionStrategyType (coefficient
/// generation precision) and ResamplerConfig, the single object that carries every setting fixed at
/// construction time: the coefficient format, the quality specification (or
/// an explicit FilterDesign bypass), I/O sample formats, dithering, coefficient
/// precision, and extrapolation behavior. Runtime-mutable concerns (ASRC
/// ratio control, max block size) remain methods on the resampler itself.
///
/// See docs/configuration_redesign.md for the design rationale.

#ifndef RESAMPLER_CONFIG_H
#define RESAMPLER_CONFIG_H

#include "resampler/filter_design.h"
#include "resampler/sample_format.h"

#include <cstddef>
#include <cstdint>

namespace micro_resampler {

/// Arithmetic the resampler processes with: the filter coefficient type,
/// the internal sample type, and the accumulator width. Also selects the
/// default I/O sample format when none is set on the config
enum class CoefficientFormat : uint8_t {
    Q15,       ///< 16-bit coefficients and samples, 32-bit accumulator (embedded default)
    Q31,       ///< 32-bit coefficients and samples, 64-bit accumulator (highest fixed-point
               ///< precision)
    Q31_LITE,  ///< Q31 coefficients over a 32-bit accumulator via high-word multiplies
               ///< (Q31-class precision at lower cost on 32-bit cores)
    FLOAT32,   ///< Single-precision floating point
    FLOAT64,   ///< Double-precision floating point (reference quality)
};

/// Returns the size of one filter coefficient for a coefficient format, for
/// filter-bank memory accounting (see DesignConstraints::bytes_per_coefficient)
inline constexpr int get_bytes_per_coefficient(CoefficientFormat format) {
    switch (format) {
        case CoefficientFormat::Q15:
            return 2;
        case CoefficientFormat::Q31:
        case CoefficientFormat::Q31_LITE:
        case CoefficientFormat::FLOAT32:
            return 4;
        case CoefficientFormat::FLOAT64:
            return 8;
    }
    return 2;  // Unreachable
}

/// Signal extension used by prime() and flush() at the stream edges
enum class ExtrapolationMode : uint8_t {
    ZEROS,    ///< Zero padding (fastest, no extrapolation)
    MIRROR,   ///< FFmpeg-style signal reflection (fast, default)
    BURG_LPC  ///< Burg's method (optimal reflection coefficients)
};

/// Precision strategy types for coefficient generation
enum class PrecisionStrategyType : uint8_t {
    DEFAULT,       ///< Platform-specific default (TableFloatStrategy on ESP32,
                   ///< DoublePrecisionStrategy on desktop)
    SINGLE,        ///< Single precision (float) - faster, less accurate
    DOUBLE,        ///< Double precision (double) - good balance
    TABLE_FLOAT,   ///< Float with lookup tables - memory efficient for embedded
    TABLE_DOUBLE,  ///< Double with lookup tables - balanced embedded approach
    QUAD,          ///< Quad precision - highest accuracy (requires Boost; falls
                   ///< back to DEFAULT when quad support is not compiled in)
};

/// @brief Construction-time configuration for MicroResampler
///
/// Bundles the quality target and every processing option that must be
/// fixed before initialization. By default the filter is derived from the
/// quality preset's specification (stopband attenuation and passband
/// width) for the actual rate pair via resolve_design(); set an explicit
/// FilterDesign to bypass derivation entirely. Setters return *this for
/// fluent chaining:
///
/// @code
/// ResamplerConfig cfg = ResamplerConfig()
///     .set_quality(ResamplerQuality::HIGH)
///     .set_input_format(SampleFormat::Q23_PACKED)
///     .set_dither(true);
/// std::unique_ptr<MicroResampler> resampler = MicroResampler::create(48000, 16000, 2, cfg);
/// @endcode
class ResamplerConfig {
public:
    /// @brief Report whether the design targets ASRC use
    /// @return true if the filter must support a drifting conversion ratio
    bool is_asrc() const {
        return this->asrc_;
    }
    /// @brief Declare that the resampler will be used for ASRC
    /// A drifting ratio never lands on the GCD phase grid, so derivation
    /// selects interpolated phases with a forced count instead of exact mode
    /// @param asrc true to derive the filter for a drifting conversion ratio
    /// @return *this for chaining
    ResamplerConfig& set_asrc(bool asrc) {
        this->asrc_ = asrc;
        return *this;
    }

    /// @brief Get the arithmetic MicroResampler::create() builds the resampler with
    /// @return the configured coefficient format
    CoefficientFormat get_coefficient_format() const {
        return this->coefficient_format_;
    }
    /// @brief Set the arithmetic MicroResampler::create() builds the resampler with
    /// Unset input/output formats default to the matching sample format
    /// (Q15 for Q15, Q31 for Q31 and Q31_LITE, FLOAT32, FLOAT64)
    /// @param format Coefficient format to process with
    /// @return *this for chaining
    ResamplerConfig& set_coefficient_format(CoefficientFormat format) {
        this->coefficient_format_ = format;
        return *this;
    }

    /// @brief Report whether TPDF dithering of the reduction to 16-bit is requested
    /// @return the requested dither flag
    bool get_dither() const {
        return this->dither_;
    }
    /// @brief Enable TPDF dithering of the reduction to 16-bit (Q15)
    /// Takes effect only when a source carrying detail below 16 bits is
    /// quantized to Q15 somewhere in the pipeline; see
    /// MicroResampler::dither_is_effective()
    /// @param dither true to dither the reduction to 16-bit
    /// @return *this for chaining
    ResamplerConfig& set_dither(bool dither) {
        this->dither_ = dither;
        return *this;
    }

    /// @brief Get the extrapolation algorithm used by prime()/flush()
    /// @return the configured extrapolation mode
    ExtrapolationMode get_extrapolation_mode() const {
        return this->extrapolation_mode_;
    }
    /// @brief Set the extrapolation algorithm used by prime()/flush()
    /// @param mode Extrapolation algorithm to use
    /// @return *this for chaining
    ResamplerConfig& set_extrapolation_mode(ExtrapolationMode mode) {
        this->extrapolation_mode_ = mode;
        return *this;
    }

    /// @brief Get the LPC prediction order for the BURG_LPC extrapolation mode
    /// @return the configured order, or 0 meaning the per-format default
    int get_extrapolator_lpc_order() const {
        return this->extrapolator_lpc_order_;
    }
    /// @brief Set the LPC prediction order for the BURG_LPC extrapolation mode
    /// @param order Desired order, clamped to [1, 8] at initialization
    /// (0 = per-format default: 4 for float formats, 2 for fixed-point)
    /// @return *this for chaining
    ResamplerConfig& set_extrapolator_lpc_order(int order) {
        this->extrapolator_lpc_order_ = order;
        return *this;
    }

    /// @brief Get the explicit filter design, bypassing spec-driven derivation
    /// Only meaningful when has_filter_design() is true
    /// @return the explicit design set with set_filter_design()
    const FilterDesign& get_filter_design() const {
        return this->filter_design_;
    }
    /// @brief Report whether an explicit filter design was set
    /// @return true if set_filter_design() was called
    bool has_filter_design() const {
        return this->has_filter_design_;
    }
    /// @brief Supply an explicit FilterDesign, bypassing spec-driven derivation
    /// The design is used verbatim: filter length, Kaiser beta, cutoffs, and
    /// phase configuration all come from it instead of resolve_design()
    /// @param design Complete filter design to use
    /// @return *this for chaining
    ResamplerConfig& set_filter_design(const FilterDesign& design) {
        this->filter_design_ = design;
        this->has_filter_design_ = true;
        return *this;
    }

    /// @brief Get the input sample format for type-erased processing
    /// Only meaningful when has_input_format() is true; otherwise the
    /// resampler uses the coefficient format's natural sample format
    /// @return the format set with set_input_format()
    SampleFormat get_input_format() const {
        return this->input_format_;
    }
    /// @brief Report whether an explicit input format was set
    /// @return true if set_input_format() was called
    bool has_input_format() const {
        return this->has_input_format_;
    }
    /// @brief Set the input sample format for type-erased processing
    /// @param format Sample format of the void* input buffers passed to
    /// process_block_raw() / prime_raw()
    /// @return *this for chaining
    ResamplerConfig& set_input_format(SampleFormat format) {
        this->input_format_ = format;
        this->has_input_format_ = true;
        return *this;
    }

    /// @brief Get the largest input block initialize() sizes the history buffer for
    /// @return the block size in frames, or 0 meaning the filter length
    size_t get_max_block_size() const {
        return this->max_block_size_;
    }
    /// @brief Size the history buffer for the largest input block at initialize()
    /// By default the buffer holds one filter length of input per call (104 frames
    /// at MEDIUM for 44.1 kHz to 48 kHz); a larger block reallocates on first use.
    /// Setting this keeps processing allocation-free for blocks up to this size
    /// @param frames Largest input frames per process_block() call (0 = filter length)
    /// @return *this for chaining
    ResamplerConfig& set_max_block_size(size_t frames) {
        this->max_block_size_ = frames;
        return *this;
    }

    /// @brief Get the filter bank memory budget in bytes
    /// @return the budget, or 0 meaning unlimited
    size_t get_memory_budget_bytes() const {
        return this->memory_budget_bytes_;
    }
    /// @brief Limit the filter bank memory used by spec-driven derivation
    /// When the CPU-first exact-phase bank exceeds the budget, derivation
    /// falls back to interpolated phases, then to a shorter filter with a
    /// reduced passband. Stopband attenuation is never sacrificed. Ignored
    /// when an explicit FilterDesign is set
    /// @param bytes Budget in bytes (0 = unlimited)
    /// @return *this for chaining
    ResamplerConfig& set_memory_budget_bytes(size_t bytes) {
        this->memory_budget_bytes_ = bytes;
        return *this;
    }

    /// @brief Get the output sample format for type-erased processing
    /// Only meaningful when has_output_format() is true; otherwise the
    /// resampler uses the coefficient format's natural sample format
    /// @return the format set with set_output_format()
    SampleFormat get_output_format() const {
        return this->output_format_;
    }
    /// @brief Report whether an explicit output format was set
    /// @return true if set_output_format() was called
    bool has_output_format() const {
        return this->has_output_format_;
    }
    /// @brief Set the output sample format for type-erased processing
    /// @param format Sample format of the void* output buffers written by
    /// process_block_raw() / flush_raw()
    /// @return *this for chaining
    ResamplerConfig& set_output_format(SampleFormat format) {
        this->output_format_ = format;
        this->has_output_format_ = true;
        return *this;
    }

    /// @brief Get the passband edge override for spec-driven derivation
    /// @return the fraction of the lower Nyquist, or 0 meaning the quality
    /// preset's value
    double get_passband_fraction() const {
        return this->passband_fraction_;
    }
    /// @brief Override the passband edge used by spec-driven derivation
    /// @param fraction Passband edge as a fraction (0-1) of the lower of the
    /// two Nyquist frequencies (0 = use the quality preset's value)
    /// @return *this for chaining
    ResamplerConfig& set_passband_fraction(double fraction) {
        this->passband_fraction_ = fraction;
        return *this;
    }

    /// @brief Get the precision strategy for coefficient generation
    /// @return the configured precision strategy
    PrecisionStrategyType get_precision() const {
        return this->precision_;
    }
    /// @brief Set the precision strategy for coefficient generation
    /// @param precision Strategy used to compute filter coefficients
    /// @return *this for chaining
    ResamplerConfig& set_precision(PrecisionStrategyType precision) {
        this->precision_ = precision;
        return *this;
    }

    /// @brief Get the quality preset supplying the default specification
    /// @return the configured quality preset
    ResamplerQuality get_quality() const {
        return this->quality_;
    }
    /// @brief Set the quality preset supplying the default specification
    /// The preset maps to a (stopband dB, passband fraction) pair via
    /// quality_spec_from_preset(); either half can be overridden with
    /// set_stopband_db() / set_passband_fraction()
    /// @param quality Quality preset to use
    /// @return *this for chaining
    ResamplerConfig& set_quality(ResamplerQuality quality) {
        this->quality_ = quality;
        return *this;
    }

    /// @brief Get the stopband attenuation override for spec-driven derivation
    /// @return the attenuation in dB, or 0 meaning the quality preset's value
    double get_stopband_db() const {
        return this->stopband_db_;
    }
    /// @brief Override the stopband attenuation used by spec-driven derivation
    /// @param stopband_db Minimum alias/image rejection in dB (0 = use the
    /// quality preset's value)
    /// @return *this for chaining
    ResamplerConfig& set_stopband_db(double stopband_db) {
        this->stopband_db_ = stopband_db;
        return *this;
    }

private:
    // Struct fields
    FilterDesign filter_design_ = FilterDesign::from_quality(ResamplerQuality::MEDIUM);

    // 64-bit fields
    double passband_fraction_ = 0.0;  // 0 = use the quality preset's value
    double stopband_db_ = 0.0;        // 0 = use the quality preset's value

    // size_t fields
    size_t max_block_size_ = 0;       // 0 = filter length
    size_t memory_budget_bytes_ = 0;  // 0 = unlimited

    // 32-bit fields
    int extrapolator_lpc_order_ = 0;  // 0 = per-format default

    // 8-bit fields
    bool asrc_ = false;
    CoefficientFormat coefficient_format_ = CoefficientFormat::Q15;
    bool dither_ = false;
    ExtrapolationMode extrapolation_mode_ = ExtrapolationMode::MIRROR;
    bool has_filter_design_ = false;
    bool has_input_format_ = false;
    bool has_output_format_ = false;
    SampleFormat input_format_ = SampleFormat::Q15;
    SampleFormat output_format_ = SampleFormat::Q15;
    PrecisionStrategyType precision_ = PrecisionStrategyType::DEFAULT;
    ResamplerQuality quality_ = ResamplerQuality::MEDIUM;
};

}  // namespace micro_resampler

#endif  // RESAMPLER_CONFIG_H
