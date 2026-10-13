// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file resampler.h
/// @brief Public polyphase audio resampler API
///
/// Declares MicroResampler, an abstract interface for processing audio
/// through a void* boundary, and its create() factories, which build the
/// implementation for the coefficient format chosen on ResamplerConfig.
/// Supports both fixed-ratio sample rate conversion and ASRC (asynchronous
/// sample rate conversion) for clock drift compensation

#ifndef RESAMPLER_H
#define RESAMPLER_H

#include "resampler/filter_design.h"
#include "resampler/resampler_config.h"
#include "resampler/sample_format.h"

#include <cstddef>
#include <cstdint>
#include <memory>

// Marks functions whose return value must not be ignored (the resampler
// reports errors only through boolean return codes). [[nodiscard]] needs
// C++17; ESP-IDF component builds may compile against an older standard
// depending on IDF version, so fall back to the GNU attribute there.
#if defined(__cplusplus) && __cplusplus >= 201703L
#define MICRO_RESAMPLER_NODISCARD [[nodiscard]]
#elif defined(__GNUC__)
#define MICRO_RESAMPLER_NODISCARD __attribute__((warn_unused_result))
#else
#define MICRO_RESAMPLER_NODISCARD
#endif

namespace micro_resampler {

constexpr int MAX_FILTER_LENGTH = 16384;  // Maximum supported filter length (sanity check)

///////////////////////////////////////////////////////////////////////////////
/// @brief High-quality polyphase audio resampler for embedded systems
///
/// Supports both fixed-ratio sample rate conversion and asynchronous sample
/// rate conversion (ASRC) for clock drift compensation. Construct one with
/// create(); the coefficient format set on the ResamplerConfig selects the
/// arithmetic (Q15 by default).
///
/// ## Sample Types
///
/// Methods that operate on sample data use `void*` pointers. Callers must
/// pass buffers matching the configured formats:
/// - Input type: ResamplerConfig::set_input_format(), else the coefficient format's default
/// - Output type: ResamplerConfig::set_output_format(), else the coefficient format's default
///
/// ## Thread Safety
///
/// This class is NOT thread-safe. Each instance maintains mutable internal state
/// (history buffer, phase tracking, RNG state for dithering). If multiple threads
/// need to process audio, use separate instances per thread or provide external
/// synchronization.
///
/// ## Usage Examples
///
/// Fixed-ratio resampling (44.1kHz → 48kHz):
/// @code
/// std::unique_ptr<MicroResampler> resampler =
///     MicroResampler::create(44100, 48000, 2, ResamplerQuality::MEDIUM);
/// if (!resampler || !resampler->initialize()) {
///     // invalid arguments or out of memory
/// }
///
/// int16_t input[1024], output[2048];
/// size_t consumed, generated;
/// resampler->process_block(input, 512, output, 1024, consumed, generated);
/// @endcode
///
/// With full configuration (coefficient format, I/O formats, dither, spec overrides):
/// @code
/// ResamplerConfig cfg = ResamplerConfig()
///     .set_coefficient_format(CoefficientFormat::Q31_LITE)
///     .set_quality(ResamplerQuality::HIGH)
///     .set_input_format(SampleFormat::Q23_PACKED)
///     .set_dither(true);
/// std::unique_ptr<MicroResampler> resampler = MicroResampler::create(44100, 48000, 2, cfg);
/// @endcode
///
/// ASRC mode for clock drift compensation (the asrc flag makes the derived
/// filter use interpolated phases so the ratio can drift):
/// @code
/// auto asrc = MicroResampler::create(48000, 48000, 2, ResamplerConfig().set_asrc(true));
/// asrc->adjust_ppm(-50.0);  // Sink clock 50 PPM fast: produce 50 PPM more output
/// @endcode
///
///////////////////////////////////////////////////////////////////////////////
class MicroResampler {
public:
    /// Alias for the namespace-scope precision strategy enum, kept so the
    /// MicroResampler::PrecisionStrategyType spelling remains valid
    using PrecisionStrategyType = micro_resampler::PrecisionStrategyType;

    // 4.28 fixed-point ratio constants (32-bit, ESP32-friendly)
    static constexpr int ASRC_FRACTIONAL_BITS = 28;
    static constexpr uint32_t UNITY_RATIO = 1U << ASRC_FRACTIONAL_BITS;  // 1.0 in 4.28 format

    // ========================================
    // Construction
    // ========================================

    /// @brief Create a resampler from a quality preset, using Q15 arithmetic and Q15 I/O
    /// The filter is derived from the preset's specification for this rate pair
    /// (see resolve_design())
    /// @param input_rate Input sample rate in Hz
    /// @param output_rate Output sample rate in Hz
    /// @param channels Number of interleaved channels
    /// @param quality Quality preset
    /// @return the resampler, or nullptr if a rate or the channel count is zero or
    /// the allocation failed. Call initialize() before processing
    static std::unique_ptr<MicroResampler> create(uint32_t input_rate, uint32_t output_rate,
                                                  int channels, ResamplerQuality quality);

    /// @brief Create a resampler from a full configuration
    /// The coefficient format, quality spec or explicit FilterDesign, I/O formats,
    /// dither, precision, and extrapolation all come from the config
    /// @param input_rate Input sample rate in Hz
    /// @param output_rate Output sample rate in Hz
    /// @param channels Number of interleaved channels
    /// @param config Construction-time configuration
    /// @return the resampler, or nullptr if a rate or the channel count is zero, an
    /// explicit FilterDesign's length is not a positive multiple of 8 (at most
    /// MAX_FILTER_LENGTH), or the allocation failed. Call initialize() before processing
    static std::unique_ptr<MicroResampler> create(uint32_t input_rate, uint32_t output_rate,
                                                  int channels, const ResamplerConfig& config);

    virtual ~MicroResampler() = default;

    // ========================================
    // Initialization and State
    // ========================================

    /// @brief Allocate the filter bank, history buffer, and other resources needed to process audio
    /// Idempotent: returns immediately with true if already initialized. If any allocation
    /// fails, releases the partially-built state and leaves the resampler uninitialized
    /// @return true on success, false if a required allocation failed
    MICRO_RESAMPLER_NODISCARD virtual bool initialize() = 0;

    /// @brief Report whether initialize() has completed successfully
    /// @return true once the resampler is initialized and ready to process audio
    virtual bool is_initialized() const = 0;

    /// @brief Clear the history buffer and phase tracking state to start a fresh stream
    /// Also clears the primed/flushed flags. If the resampler has not been initialized yet,
    /// only those flags are cleared. Format, precision, and ratio configuration are kept
    virtual void reset() = 0;

    // ========================================
    // Format Configuration (fixed at construction via ResamplerConfig)
    // ========================================

    /// @brief Get the current input sample format
    /// @return the sample format used to interpret void* input buffers
    virtual SampleFormat get_input_format() const = 0;

    /// @brief Get the current output sample format
    /// @return the sample format used to write void* output buffers
    virtual SampleFormat get_output_format() const = 0;

    // ========================================
    // Dither Configuration (fixed at construction via ResamplerConfig)
    // ========================================

    /// @brief Get the requested dither enable flag
    /// Dithering is applied at most once, at the single point where a source
    /// carrying detail below 16 bits is quantized to Q15 (see
    /// ResamplerConfig::set_dither())
    /// @return the configured flag, regardless of whether it takes effect
    virtual bool get_dither() const = 0;

    /// @brief Report whether the current configuration actually dithers
    /// @return true only if dithering is enabled and the input/processing/output
    /// combination reduces a wider-than-16-bit source to Q15 somewhere
    virtual bool dither_is_effective() const = 0;

    // ========================================
    // Coefficient Precision (fixed at construction via ResamplerConfig)
    // ========================================

    /// @brief Get the current coefficient precision strategy
    /// @return the precision strategy that will be (or was) used to generate filter coefficients
    virtual PrecisionStrategyType get_coefficient_precision() const = 0;

    // ========================================
    // Type-Erased Processing (void* interface)
    // ========================================

    /// @brief Process a block of audio samples using void* pointers
    /// @param input Pointer to interleaved input samples (must match input format)
    /// @param input_frames Number of input frames
    /// @param output Pointer to interleaved output buffer (must match output format)
    /// @param output_capacity Maximum output frames the buffer can hold
    /// @param frames_consumed [out] Number of input frames consumed
    /// @param frames_generated [out] Number of output frames generated
    /// @return true on success, false on failure
    MICRO_RESAMPLER_NODISCARD
    virtual bool process_block_raw(const void* input, size_t input_frames, void* output,
                                   size_t output_capacity, size_t& frames_consumed,
                                   size_t& frames_generated) = 0;

    /// @brief Typed convenience wrapper around process_block_raw()
    /// The pointer types are not checked against the configured formats
    template <typename InputT, typename OutputT>
    MICRO_RESAMPLER_NODISCARD bool process_block(const InputT* input, size_t input_frames,
                                                 OutputT* output, size_t output_capacity,
                                                 size_t& frames_consumed,
                                                 size_t& frames_generated) {
        return this->process_block_raw(static_cast<const void*>(input), input_frames,
                                       static_cast<void*>(output), output_capacity, frames_consumed,
                                       frames_generated);
    }

    /// @brief Prime the history buffer using void* pointer
    /// @param initial_samples Pointer to initial samples (must match input format)
    /// @param frame_count Number of frames to use for priming
    /// @return true on success, false on failure
    MICRO_RESAMPLER_NODISCARD virtual bool prime_raw(const void* initial_samples,
                                                     size_t frame_count) = 0;

    /// @brief Typed convenience wrapper around prime_raw()
    template <typename InputT>
    MICRO_RESAMPLER_NODISCARD bool prime(const InputT* initial_samples, size_t frame_count) {
        return this->prime_raw(static_cast<const void*>(initial_samples), frame_count);
    }

    /// @brief Flush remaining samples using void* pointer
    /// @param output Pointer to output buffer (must match output format)
    /// @param output_capacity Maximum output frames the buffer can hold
    /// @param frames_generated [out] Number of output frames generated
    /// @return true on success, false on failure
    MICRO_RESAMPLER_NODISCARD
    virtual bool flush_raw(void* output, size_t output_capacity, size_t& frames_generated) = 0;

    /// @brief Typed convenience wrapper around flush_raw()
    template <typename OutputT>
    MICRO_RESAMPLER_NODISCARD bool flush(OutputT* output, size_t output_capacity,
                                         size_t& frames_generated) {
        return this->flush_raw(static_cast<void*>(output), output_capacity, frames_generated);
    }

    /// @brief Report whether flush_raw()/flush() has drained all reflected tail samples
    /// @return true once flushing has completed and no more output remains to be generated
    virtual bool is_flushed() const = 0;

    /// @brief Report whether the history buffer has been primed with initial samples
    /// @return true once prime_raw()/prime() has been called successfully
    virtual bool is_primed() const = 0;

    // ========================================
    // Configuration Getters
    // ========================================

    /// @brief Get the number of interleaved audio channels the resampler was configured with
    /// @return the channel count passed to create()
    virtual int get_channels() const = 0;

    /// @brief Get the FIR filter length in taps per polyphase branch
    /// @return the configured filter length
    virtual int get_filter_length() const = 0;

    /// @brief Get the input sample rate used for fixed-ratio conversion
    /// @return the input rate passed to create(), in Hz
    virtual uint32_t get_input_rate() const = 0;

    /// @brief Get the basic filter latency in input samples
    /// Equal to filter_length / 2. For sub-sample precision use get_latency_precise()
    /// @return the filter latency in input samples
    virtual int get_latency() const = 0;

    /// @brief Get the precise filter group delay in input samples
    /// For a symmetric FIR filter of length N, the group delay is exactly (N-1)/2 samples
    /// @return the precise filter latency in input samples
    virtual double get_latency_precise() const = 0;

    /// @brief Get the number of polyphase filter bank phases in use
    /// @return the phase count, between 1 and the design's configured maximum
    virtual int get_num_phases() const = 0;

    /// @brief Get the output sample rate used for fixed-ratio conversion
    /// @return the output rate passed to create(), in Hz
    virtual uint32_t get_output_rate() const = 0;

    // ========================================
    // ASRC Rate Control
    // ========================================

    /// @brief Report whether the ratio tracker has transitioned into ASRC mode
    /// ASRC mode is entered automatically and permanently the first time set_ratio(),
    /// set_ratio_fixed(), set_target_ratio(), set_target_ratio_fixed(), or adjust_ppm() is called
    /// @return true if in ASRC mode, false while still in the initial fixed-ratio mode
    virtual bool is_asrc_mode() const = 0;

    /// @brief Check whether ASRC rate control is available for this conversion ratio
    /// @return false if the simplified input:output ratio is 16:1 or larger, exceeding the
    /// 4.28 fixed-point format used to represent ASRC ratios; true otherwise
    virtual bool is_asrc_supported() const = 0;

    /// @brief Get the current input:output conversion ratio
    /// Returns the base ratio while in fixed mode, or the (possibly slewing) ASRC ratio
    /// once ASRC mode has been enabled
    /// @return the current ratio as a double (1.0 = unity)
    virtual double get_current_ratio() const = 0;

    /// @brief Get the current input:output conversion ratio in 4.28 fixed-point format
    /// @return the current ratio encoded as Q4.28 (1.0 = 1 << 28)
    virtual uint32_t get_current_ratio_fixed() const = 0;

    /// @brief Report whether the ratio tracker is still in the initial fixed-ratio mode
    /// @return true if no ASRC ratio adjustment has been applied yet, false once in ASRC mode
    virtual bool is_fixed_mode() const = 0;

    /// @brief Get the nominal input sample rate used as the ASRC ratio baseline
    /// @return the original (pre-GCD-simplified) input rate passed at construction, in Hz
    virtual uint32_t get_nominal_rate() const = 0;

    /// @brief Get the current fractional input position within the polyphase cycle
    /// Intended for debugging and monitoring
    /// @return the fractional phase position in the range [0, 1)
    virtual double get_phase_position() const = 0;

    /// @brief Set the conversion ratio directly, enabling ASRC mode
    /// The new ratio sets the advance after the next output frame (whose position is already
    /// fixed). The read position stays continuous, so an instant change steps the pitch without
    /// a click; at drift-correction sizes (up to a few hundred ppm) the step is inaudible. Slew
    /// larger deliberate changes if the pitch step matters
    /// @param ratio New input:output ratio (1.0 = unity, less than 1.0 speeds up output,
    /// greater than 1.0 slows it down)
    virtual void set_ratio(double ratio) = 0;

    /// @brief Set the conversion ratio directly using 4.28 fixed-point format, enabling ASRC mode
    /// @param ratio_4_28 New input:output ratio encoded as Q4.28 (1.0 = 1 << 28)
    virtual void set_ratio_fixed(uint32_t ratio_4_28) = 0;

    /// @brief Report whether the ratio is still ramping toward a target
    /// The target is set by set_target_ratio(), set_target_ratio_fixed(), or adjust_ppm()
    /// @return true while a slew is in progress, false otherwise (including in fixed mode)
    virtual bool is_slewing() const = 0;

    /// @brief Get the cumulative timing drift from the base ratio
    /// Always 0.0 in fixed mode; in ASRC mode, tracks how far output timing has drifted
    /// from the base ratio, wrapped to stay within +/-1 sample
    /// @return the subsample offset in samples, approximately in the range [-1, 1]
    virtual double get_subsample_offset() const = 0;

    /// @brief Smoothly transition to a new conversion ratio, enabling ASRC mode
    /// @param ratio Target input:output ratio (1.0 = unity)
    /// @param slew_samples Number of output frames over which to ramp linearly to the target
    /// ratio; the ratio reaches the target exactly after that many frames (0 = change instantly)
    virtual void set_target_ratio(double ratio, uint32_t slew_samples) = 0;

    /// @brief Get the conversion ratio being slewed toward
    /// Equal to get_current_ratio() when no slew is in progress
    /// @return the target ratio as a double (1.0 = unity)
    virtual double get_target_ratio() const = 0;

    /// @brief Smoothly transition to a new conversion ratio using 4.28 fixed-point format
    /// @param ratio_4_28 Target input:output ratio encoded as Q4.28 (1.0 = 1 << 28)
    /// @param slew_samples Number of output frames over which to ramp linearly to the target
    /// ratio; the ratio reaches the target exactly after that many frames (0 = change instantly)
    virtual void set_target_ratio_fixed(uint32_t ratio_4_28, uint32_t slew_samples) = 0;

    /// @brief Get the conversion ratio being slewed toward, in 4.28 fixed-point format
    /// @return the target ratio encoded as Q4.28 (1.0 = 1 << 28)
    virtual uint32_t get_target_ratio_fixed() const = 0;

    /// @brief Adjust the conversion ratio by a PPM offset from the base ratio, enabling ASRC mode
    /// The offset is relative to the base conversion ratio, not to unity
    /// @param ppm_offset Parts-per-million adjustment to the input:output ratio. Positive
    /// consumes more input per output frame, producing fewer output frames (use when the
    /// sink clock is slow); negative produces more output frames (sink clock is fast)
    /// @param slew_samples Number of output frames over which to ramp linearly to the new
    /// ratio; the ratio reaches it exactly after that many frames (0 = change instantly, which
    /// is inaudible at drift-correction sizes; see set_ratio())
    virtual void adjust_ppm(double ppm_offset, uint32_t slew_samples = 0) = 0;

    // ========================================
    // Extrapolation Configuration (fixed at construction via ResamplerConfig)
    // ========================================

    /// @brief Get the configured extrapolation mode used by prime()/flush()
    /// @return the extrapolation mode from the construction-time configuration
    virtual ExtrapolationMode get_extrapolation_mode() const = 0;

    /// @brief Get the LPC order currently used by the active extrapolator
    /// @return the LPC order in use, or 0 if the resampler is not yet initialized or the
    /// active extrapolation mode does not use LPC
    virtual int get_extrapolator_lpc_order() const = 0;

    /// @brief Pre-allocate the history buffer to accommodate a given input block size
    /// Triggers lazy initialize() if it has not run yet. Growing the buffer up front avoids
    /// a reallocation the first time a large block is processed. Prefer
    /// ResamplerConfig::set_max_block_size(), which sizes the buffer during initialize()
    /// @param max_frames Largest number of input frames expected in a single
    /// process_block_raw()/process_block() call
    virtual void set_max_block_size(size_t max_frames) = 0;

    /// @brief Get the largest input block size the history buffer currently supports
    /// without reallocating
    /// Unlike ResamplerConfig::get_max_block_size() (the requested size, 0 = default), this
    /// reports what the allocated buffer actually holds
    /// @return the buffer headroom in frames (history buffer capacity minus filter length),
    /// or 0 if the resampler has not been initialized yet
    virtual size_t get_max_block_size() const = 0;
};

}  // namespace micro_resampler

#endif  // RESAMPLER_H
