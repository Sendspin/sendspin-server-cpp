// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file resampler_impl.h
/// @brief Private MicroResamplerImpl template behind the public MicroResampler interface
///
/// Defines MicroResamplerImpl<CoeffFormat>, the polyphase resampler for one
/// coefficient format, with explicit instantiations declared for Q15Format,
/// Q31Format, Q31LiteFormat, Float32Format, and Float64Format. Not part of the
/// public API: consumers go through MicroResampler::create()

#ifndef RESAMPLER_IMPL_H
#define RESAMPLER_IMPL_H

#include "detail/ratio_tracker.h"
#include "extrapolator/extrapolator.h"
#include "formats.h"
#include "resampler/filter_design.h"
#include "resampler/resampler.h"
#include "resampler/resampler_config.h"
#include "resampler/sample_format.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace micro_resampler {

// Forward declarations
class CoefficientGenerator;
template <typename CoeffFormat>
struct MaxTapEntry;  // Defined in sinc_table.h

namespace dotprod {
template <typename CoeffFormat>
struct DotProdConfig;
}  // namespace dotprod

///////////////////////////////////////////////////////////////////////////////
/// @brief Polyphase resampler implementation for one coefficient format
///
/// Implements the MicroResampler interface with arithmetic fixed by
/// CoeffFormat. Users obtain one through MicroResampler::create(), which
/// picks the instantiation from ResamplerConfig::set_coefficient_format();
/// the white-box tests construct it directly.
///
/// @tparam CoeffFormat   Filter coefficient format (Q15Format, Q31Format, Float64Format, etc.)
///
///////////////////////////////////////////////////////////////////////////////
template <typename CoeffFormat>
class MicroResamplerImpl final : public MicroResampler {
public:
    // Unified tracker type
    using RatioTracker = detail::UnifiedRatioTracker<CoeffFormat>;

    // Type aliases for clarity
    using accumulator_t = typename CoeffFormat::accumulator_t;
    using internal_sample_t = typename CoeffFormat::sample_t;

    // ========================================
    // Constructors
    // ========================================

    // Constructor with quality preset: the filter is derived from the
    // preset's specification for this rate pair (see resolve_design()),
    // with default formats matching the CoeffFormat
    MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate, int channels,
                       ResamplerQuality quality);

    // Constructor with full configuration: quality spec or explicit
    // FilterDesign, I/O formats, dither, precision, and extrapolation all
    // come from the config (see ResamplerConfig). The config's coefficient
    // format is ignored: CoeffFormat decides the arithmetic
    MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate, int channels,
                       const ResamplerConfig& config);

    ~MicroResamplerImpl() override;

    // Initialize the resampler (allocates memory for filter bank and buffers)
    // Returns true on success, false on allocation failure
    // Safe to call multiple times (idempotent)
    MICRO_RESAMPLER_NODISCARD bool initialize() override;

    // Check if the resampler has been initialized
    bool is_initialized() const override {
        return this->initialized_;
    }

    // ========================================
    // Format Configuration
    // ========================================

    SampleFormat get_input_format() const override {
        return this->input_format_;
    }

    SampleFormat get_output_format() const override {
        return this->output_format_;
    }

    bool get_dither() const override {
        return this->dither_;
    }

    bool dither_is_effective() const override;

    // Get the current coefficient precision strategy
    PrecisionStrategyType get_coefficient_precision() const override {
        return this->precision_strategy_;
    }

    // Type-erased interface implementation
    MICRO_RESAMPLER_NODISCARD
    bool process_block_raw(const void* input, size_t input_frames, void* output,
                           size_t output_capacity, size_t& frames_consumed,
                           size_t& frames_generated) override;

    void reset() override;

    // Prime the history buffer with reflected samples for smooth startup.
    // Call once before the first processBlock() with the first samples of input.
    // Uses FFmpeg-style reflection: mirrors (filter_length + 1) / 2 frames
    // around the first sample, so the filter sees realistic signal history.
    // If not called, history starts as zeros (existing behavior).
    // Returns true on success, false if initialization fails
    MICRO_RESAMPLER_NODISCARD
    bool prime_raw(const void* initial_samples, size_t frame_count) override;

    // Check if the resampler has been primed with initial samples
    bool is_primed() const override {
        return this->primed_;
    }

    // Flush remaining samples using FFmpeg-style signal reflection.
    // Instead of zero-padding, mirrors (filter_length + 1) / 2 frames of signal
    // around the last sample, producing smoother end-of-stream output.
    // Call repeatedly until isFlushed() returns true or frames_generated == 0.
    // Returns true on success, false if initialization fails
    MICRO_RESAMPLER_NODISCARD
    bool flush_raw(void* output, size_t output_capacity, size_t& frames_generated) override;

    // Check if the resampler has completed flushing
    bool is_flushed() const override {
        return this->flushed_;
    }

    // Returns the basic filter latency in input samples (filter_length/2).
    // For more precise phase-aligned latency, use getLatencyPrecise().
    int get_latency() const override {
        return this->filter_length_ / 2;
    }

    // Returns the precise filter latency in input samples.
    // For a symmetric FIR filter of length N, the group delay is exactly (N-1)/2 samples.
    double get_latency_precise() const override {
        return (this->filter_length_ - 1) / 2.0;
    }
    int get_filter_length() const override {
        return this->filter_length_;
    }
    int get_channels() const override {
        return this->num_channels_;
    }

    // get_input_rate() - available in fixed-ratio mode
    uint32_t get_input_rate() const override {
        return this->input_rate_;
    }

    // get_output_rate() - available in fixed-ratio mode
    uint32_t get_output_rate() const override {
        return this->output_rate_;
    }

    int get_num_phases() const override {
        return this->num_phases_;
    }

    // ========================================
    // ASRC-Specific Rate Control API
    // ========================================
    //
    // ASRC (Asynchronous Sample Rate Conversion) mode enables dynamic ratio
    // adjustment for clock drift compensation. The ratio is stored in 4.28
    // fixed-point format (4 integer bits, 28 fractional bits).
    //
    // LIMITATION: ASRC mode only supports conversion ratios < 16:1.
    // For extreme downsampling ratios >= 16:1 (e.g., 192kHz → 8kHz = 24:1),
    // the ASRC functions (adjust_ppm, set_ratio, etc.) will silently have no
    // effect, and the resampler continues using fixed integer arithmetic.
    // Use is_asrc_supported() to check if ASRC is available for a given ratio.

    // Check if ASRC mode is supported for this conversion ratio
    // Returns false if the ratio >= 16:1 (exceeds 4.28 format limit)
    bool is_asrc_supported() const override {
        return this->ratio_tracker_.is_asrc_supported();
    }

    // Set ratio directly (1.0 = unity, <1.0 = speed up output, >1.0 = slow down). The read
    // position stays continuous, so an instant change only steps the pitch (see resampler.h)
    void set_ratio(double ratio) override {
        this->ratio_tracker_.set_ratio(ratio);
    }

    void set_ratio_fixed(uint32_t ratio_4_28) override {
        this->ratio_tracker_.set_ratio_fixed(ratio_4_28);
    }

    // Smoothly transition to new ratio over slew_samples
    void set_target_ratio(double ratio, uint32_t slew_samples) override {
        this->ratio_tracker_.set_target_ratio(ratio, slew_samples);
    }

    void set_target_ratio_fixed(uint32_t ratio_4_28, uint32_t slew_samples) override {
        this->ratio_tracker_.set_target_ratio_fixed(ratio_4_28, slew_samples);
    }

    // Adjust ratio by PPM offset from unity (supports fractional PPM)
    void adjust_ppm(double ppm_offset, uint32_t slew_samples = 0) override {
        this->ratio_tracker_.adjust_ppm(ppm_offset, slew_samples);
    }

    // Get current ratio state
    double get_current_ratio() const override {
        return this->ratio_tracker_.get_current_ratio();
    }

    uint32_t get_current_ratio_fixed() const override {
        return this->ratio_tracker_.get_current_ratio_fixed();
    }

    double get_target_ratio() const override {
        return this->ratio_tracker_.get_target_ratio();
    }

    uint32_t get_target_ratio_fixed() const override {
        return this->ratio_tracker_.get_target_ratio_fixed();
    }

    bool is_slewing() const override {
        return this->ratio_tracker_.is_slewing();
    }

    // Get the current fractional input position (for debugging/monitoring)
    double get_phase_position() const override {
        return this->ratio_tracker_.get_phase_position();
    }

    // Get subsample offset from nominal 1:1 ratio (in samples, range approximately -1 to +1)
    double get_subsample_offset() const override {
        return this->ratio_tracker_.get_subsample_offset();
    }

    // Get nominal rate (for ASRC compatibility - returns input rate)
    uint32_t get_nominal_rate() const override {
        return this->ratio_tracker_.get_nominal_rate();
    }

    // Mode query methods
    bool is_fixed_mode() const override {
        return this->ratio_tracker_.is_fixed_mode();
    }

    bool is_asrc_mode() const override {
        return this->ratio_tracker_.is_asrc_mode();
    }

    // Extrapolation mode used by prime/flush (fixed at construction)
    ExtrapolationMode get_extrapolation_mode() const override {
        return this->extrapolation_mode_;
    }

    // LPC order used by LPC-based extrapolation modes (fixed at construction)
    int get_extrapolator_lpc_order() const override;

    // Block size configuration for input conversion buffer
    // When input format differs from coefficient format, a conversion buffer is used
    // Call this to pre-allocate the buffer for larger block sizes
    void set_max_block_size(size_t max_frames) override;
    size_t get_max_block_size() const override;

private:
    // Ratio tracker handles all phase state and calculation
    RatioTracker ratio_tracker_;

    // Prime/flush state tracking
    bool primed_;                    // True after prime() called
    bool flushed_;                   // True after flush() completed
    size_t flush_remaining_frames_;  // Reflected frames still to process

    // Extrapolation settings
    ExtrapolationMode extrapolation_mode_ = ExtrapolationMode::MIRROR;

    // Polymorphic extrapolator handles all extrapolation modes
    std::unique_ptr<Extrapolator<CoeffFormat>> extrapolator_;

    internal_sample_t* history_buffer_;  // Size: >= 2 * filter_length_ * num_channels_
                                         // samples (double-sized for zero-copy)
    size_t history_buffer_capacity_;     // Current capacity in frames (for reallocation tracking)

    typename CoeffFormat::coefficient_t*
        filter_bank_;                    // Pointer to dynamically allocated filter bank
    uint8_t* coefficient_block_shifts_;  // Per-block shift values (one per 8 coefficients)

    // Max tap extraction (only allocated if feature is enabled)
    // Array sized as num_phases * max_taps_per_phase_, storing largest coefficients per phase
    MaxTapEntry<CoeffFormat>*
        max_tap_entries_;     // Interleaved coefficients and indices for cache efficiency
    int max_taps_per_phase_;  // Max taps extracted per phase (1-FilterDesign::MAX_TAPS_PER_PHASE)

    // Aligned-mode row permutation: when the fixed aligned path is active, the
    // filter bank rows (and the per-row shift/max-tap arrays) are stored in
    // phase visit order so consecutive outputs read sequential memory. This
    // maps a logical phase index to its physical row; nullptr when the bank
    // is stored in natural phase order
    uint16_t* phase_to_row_ = nullptr;

    int num_channels_;

    // Rate storage - stores the original (non-GCD-simplified) rates
    uint32_t input_rate_;   ///< Input sample rate
    uint32_t output_rate_;  ///< Output sample rate

    int filter_length_;
    uint32_t num_phases_;  // Number of polyphase filter banks (1-1024)

    // Workspace buffers for interpolation (avoids VLAs)
    std::vector<accumulator_t> temp_output_;
    std::vector<accumulator_t> temp_output2_;

    // ========================================
    // Type-Erased Format Configuration
    // ========================================

    SampleFormat input_format_;   ///< Runtime input sample format
    SampleFormat output_format_;  ///< Runtime output sample format
    bool dither_ = false;         ///< Requested dither enable (see set_dither())

    // RNG state storage for output dithering
    std::vector<uint32_t> output_rng_states_;
    std::vector<uint32_t*> output_rng_ptrs_;

    // RNG state storage for input dithering
    std::vector<uint32_t> input_rng_states_;
    std::vector<uint32_t*> input_rng_ptrs_;

    // Lazy initialization state
    bool initialized_ = false;

    // Coefficient precision strategy
    PrecisionStrategyType precision_strategy_ = PrecisionStrategyType::DEFAULT;

    // Polymorphic coefficient generator for runtime strategy selection
    // (CoefficientGenerator defined in detail/coefficient_generator.h)
    std::unique_ptr<micro_resampler::CoefficientGenerator> coefficient_generator_;

    // Configuration storage for deferred initialization
    struct Config {
        uint32_t input_rate = 0;
        uint32_t output_rate = 0;
        int channels = 0;
        FilterDesign design = FilterDesign::from_quality(ResamplerQuality::MEDIUM);
    };
    Config config_;

    // LPC order requested via ResamplerConfig (0 = extrapolator default),
    // applied to the extrapolator created during initialize()
    int extrapolator_lpc_order_ = 0;

    // Input block size requested via ResamplerConfig (0 = filter length),
    // used to size the history buffer during initialize()
    size_t initial_max_block_size_ = 0;

    // Delegation target for the public constructors: takes a fully resolved
    // FilterDesign and concrete formats
    MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate, int channels,
                       const FilterDesign& design, SampleFormat input_format,
                       SampleFormat output_format);

    // Resolve the FilterDesign for a config: the explicit design if one was
    // set, otherwise spec-driven derivation for this rate pair and CoeffFormat
    static FilterDesign design_from_config(const ResamplerConfig& config, uint32_t input_rate,
                                           uint32_t output_rate);

    // Initialize the per-block dot product config with the fields that are
    // invariant across output samples (length, channels, feature pointers)
    void init_dotprod_config(dotprod::DotProdConfig<CoeffFormat>& config) const;

    // Helper for computing dot product with appropriate shift handling; updates
    // only the per-output pointer fields of the pre-built config
    void compute_dotprod(dotprod::DotProdConfig<CoeffFormat>& config,
                         const internal_sample_t* samples, uint16_t phase_index,
                         accumulator_t* output);

    // Templated interpolation with concrete output type (inlineable, no virtual dispatch)
    template <typename OutputSampleType>
    void interpolate_frame_impl(dotprod::DotProdConfig<CoeffFormat>& config,
                                const internal_sample_t* read_ptr, uint16_t phase_index,
                                int32_t blend_factor, void* output);

    // Helper to free all allocated resources (used in destructor and constructor error paths)
    void cleanup_resources();

    // Reorder filter bank rows (plus block shifts and max tap entries) into the
    // aligned fixed-mode visit order and build phase_to_row_; no-op when the
    // aligned path is not active or the reorder buffers cannot be allocated
    void permute_filter_bank_rows();

    // Factory method to create coefficient generator based on precision strategy
    std::unique_ptr<micro_resampler::CoefficientGenerator> create_coefficient_generator(
        PrecisionStrategyType type);

    // Where (if anywhere) the single reduction to 16-bit resolution occurs, and
    // thus where dither is applied when enabled. Dither only helps when the source
    // (input) carries detail below 16 bits; the processing format (CoeffFormat)
    // decides whether that reduction lands at the input conversion (Q15 processing)
    // or the output conversion (wider processing writing Q15 output).
    enum class DitherStage { None, Input, Output };
    DitherStage dither_stage() const;

    // Initialize RNG storage for dithering
    void init_rng_storage();

    // Get default input format for this coefficient format
    static SampleFormat get_default_input_format();

    // Get default output format for this coefficient format
    static SampleFormat get_default_output_format();

    // Result of phase wraparound calculation for interpolation
    struct NextPhaseResult {
        uint16_t phase;        // Next phase index (wraps to 0 if needed)
        size_t sample_offset;  // Sample offset for wraparound (0 or 1)
    };

    // Compute next phase with wraparound handling for interpolation
    NextPhaseResult compute_next_phase(uint16_t phase_index) const {
        uint16_t next = phase_index + 1;
        if (next == this->num_phases_) {
            return {0, 1};  // Wrap to phase 0, advance by 1 sample
        }
        return {next, 0};  // No wrap, no offset
    }

    // Helper functions extracted from processBlock variants to reduce duplication
    // Returns true if we should break (not enough samples for wraparound interpolation)
    bool check_phase_wrap_around(const PhaseInfo& phase_info, size_t consumed,
                                 size_t input_frames) const;

    // Returns pointer to read location in history buffer
    const internal_sample_t* get_read_ptr(size_t consumed) const;

    // Updates history buffer after processing
    void update_history_buffer(const void* interleaved_input, size_t consumed);

    // Prepares the history buffer to receive input frames (grows it if needed)
    // and converts the entire block into it up front
    // Returns false if the buffer could not be grown (allocation failure)
    bool copy_input_to_history(const void* interleaved_input, size_t input_frames);

    // Convert a contiguous run of input frames into dst using the runtime input format
    // (switch once per call, then a templated inner loop)
    void convert_input(const void* src, internal_sample_t* dst, size_t frame_count);

    // Templated input conversion loop (inlineable, no virtual dispatch)
    template <typename InputSampleType>
    void convert_samples(const uint8_t* src, internal_sample_t* dst, size_t sample_count);

    // History buffer frames needed for filter_length history plus one input block of
    // block_frames, or 0 if the byte size would overflow size_t
    size_t history_frames_for_block(size_t block_frames) const;

    // Ensure history buffer has room for one input block of block_frames
    // Returns false on size overflow or allocation failure (existing buffer is left intact)
    bool ensure_history_buffer_capacity(size_t block_frames);

    // Internal result structure for process_block_impl and flush_impl
    struct ProcessResult {
        size_t input_frames_consumed = 0;
        size_t output_frames_generated = 0;
        bool ok = true;  // False if processing aborted (e.g., allocation failure)
    };

    // Unified process_block implementation (dispatches to output-format-templated inner loop)
    ProcessResult process_block_impl(const void* interleaved_input, size_t input_frames,
                                     void* interleaved_output, size_t output_capacity);

    // Templated inner loop with concrete output type (switch once per block, not per frame)
    template <typename OutputSampleType>
    ProcessResult process_block_inner(const void* interleaved_input, size_t input_frames,
                                      void* interleaved_output, size_t output_capacity);

    // Internal flush implementation (dispatches to output-format-templated inner loop)
    ProcessResult flush_impl(void* interleaved_output, size_t output_capacity);

    // Templated flush inner loop with concrete output type
    template <typename OutputSampleType>
    ProcessResult flush_inner(void* interleaved_output, size_t output_capacity);
};

// Explicit template instantiation declarations (one per CoeffFormat)
extern template class MicroResamplerImpl<Q15Format>;
extern template class MicroResamplerImpl<Q31Format>;
extern template class MicroResamplerImpl<Q31LiteFormat>;
extern template class MicroResamplerImpl<Float32Format>;
extern template class MicroResamplerImpl<Float64Format>;
}  // namespace micro_resampler

#endif  // RESAMPLER_IMPL_H
