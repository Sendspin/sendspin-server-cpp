// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#include "resampler/resampler.h"

// detail/coefficient_generator_impl.h defines CoefficientGeneratorImpl (used
// below via detail/coefficient_generator.h's CoefficientGenerator base and
// detail/precision_strategy.h's strategy types).
#include "detail/accumulator_blender.h"
#include "detail/coefficient_generator.h"
#include "detail/coefficient_generator_impl.h"  // IWYU pragma: keep
#include "detail/cycle_profile.h"
#include "detail/precision_strategy.h"
#include "detail/rng_utils.h"
#include "dotprod/dotprod.h"
#include "dotprod/dotprod_dispatch.h"  // For dotprod::compute()
#include "extrapolator/extrapolator.h"
#include "memory_utils.h"
#include "resampler/design_resolver.h"
#include "resampler_impl.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <numeric>
#include <type_traits>

namespace micro_resampler {

#if MR_PROFILE_ENABLED
namespace profile {
CycleCounters g_cycles;
}  // namespace profile
#endif

// Helper to free a pointer and set it to nullptr
template <typename T>
static void free_and_null(T*& ptr) {
    if (ptr) {
        MemoryUtils::free(ptr);
        ptr = nullptr;
    }
}

// Allocation strategy for the filter bank: banks up to the configured
// threshold stay in internal RAM (a bank larger than the data cache pays a
// PSRAM miss on nearly every row read), larger banks follow the PSRAM policy
static AllocStrategy bank_alloc_strategy(size_t bank_bytes) {
#if defined(CONFIG_RESAMPLER_INTERNAL_BANK_MAX_KB) && CONFIG_RESAMPLER_INTERNAL_BANK_MAX_KB > 0
    if (bank_bytes <= static_cast<size_t>(CONFIG_RESAMPLER_INTERNAL_BANK_MAX_KB) * 1024) {
        return AllocStrategy::PREFER_INTERNAL;
    }
#else
    (void)bank_bytes;
#endif
    return AllocStrategy::PREFER_PSRAM;
}

// ============================================================================
// Default Format Helpers
// ============================================================================

template <typename CoeffFormat>
SampleFormat MicroResamplerImpl<CoeffFormat>::get_default_input_format() {
    if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                  std::is_same_v<CoeffFormat, Q31LiteFormat>) {
        return SampleFormat::Q31;
    } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
        return SampleFormat::FLOAT32;
    } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
        return SampleFormat::FLOAT64;
    } else {
        return SampleFormat::Q15;  // Q15Format and default fallback
    }
}

template <typename CoeffFormat>
SampleFormat MicroResamplerImpl<CoeffFormat>::get_default_output_format() {
    if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                  std::is_same_v<CoeffFormat, Q31LiteFormat>) {
        return SampleFormat::Q31;
    } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
        return SampleFormat::FLOAT32;
    } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
        return SampleFormat::FLOAT64;
    } else {
        return SampleFormat::Q15;  // Q15Format and default fallback
    }
}

// ============================================================================
// Constructor with quality preset
// ============================================================================
template <typename CoeffFormat>
MicroResamplerImpl<CoeffFormat>::MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate,
                                                    int channels, ResamplerQuality quality)
    : MicroResamplerImpl(input_rate, output_rate, channels,
                         ResamplerConfig().set_quality(quality)) {}

// ============================================================================
// Design Resolution for ResamplerConfig
// ============================================================================
template <typename CoeffFormat>
FilterDesign MicroResamplerImpl<CoeffFormat>::design_from_config(const ResamplerConfig& config,
                                                                 uint32_t input_rate,
                                                                 uint32_t output_rate) {
    if (config.has_filter_design()) {
        return config.get_filter_design();
    }

    QualitySpec spec = quality_spec_from_preset(config.get_quality());
    if (config.get_stopband_db() > 0.0) {
        spec.stopband_db = config.get_stopband_db();
    }
    if (config.get_passband_fraction() > 0.0) {
        spec.passband_fraction = config.get_passband_fraction();
    }

    DesignConstraints constraints;
    constraints.memory_budget_bytes = config.get_memory_budget_bytes();
    constraints.bytes_per_coefficient = sizeof(typename CoeffFormat::coefficient_t);
    constraints.asrc = config.is_asrc();
    constraints.table_beta_grid = precision_uses_beta_table(config.get_precision());

    return resolve_design(spec, input_rate, output_rate, constraints).design;
}

// ============================================================================
// Constructor with full configuration
// ============================================================================
template <typename CoeffFormat>
MicroResamplerImpl<CoeffFormat>::MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate,
                                                    int channels, const ResamplerConfig& config)
    : MicroResamplerImpl(
          input_rate, output_rate, channels, design_from_config(config, input_rate, output_rate),
          config.has_input_format() ? config.get_input_format() : get_default_input_format(),
          config.has_output_format() ? config.get_output_format() : get_default_output_format()) {
    this->dither_ = config.get_dither();
    this->precision_strategy_ = config.get_precision();
    this->extrapolation_mode_ = config.get_extrapolation_mode();
    this->extrapolator_lpc_order_ = config.get_extrapolator_lpc_order();
    this->initial_max_block_size_ = config.get_max_block_size();
}

// ============================================================================
// Delegation target: fully resolved FilterDesign and concrete formats
// ============================================================================
template <typename CoeffFormat>
MicroResamplerImpl<CoeffFormat>::MicroResamplerImpl(uint32_t input_rate, uint32_t output_rate,
                                                    int channels, const FilterDesign& design,
                                                    SampleFormat input_format,
                                                    SampleFormat output_format)
    : primed_(false),
      flushed_(false),
      flush_remaining_frames_(0),
      history_buffer_(nullptr),
      history_buffer_capacity_(0),
      filter_bank_(nullptr),
      coefficient_block_shifts_(nullptr),
      max_tap_entries_(nullptr),
      max_taps_per_phase_(design.get_max_taps_per_phase()),
      num_channels_(channels),
      input_rate_(input_rate),
      output_rate_(output_rate),
      input_format_(input_format),
      output_format_(output_format) {
    assert(channels > 0);

    const uint32_t input_rate_integer = input_rate;
    const uint32_t output_rate_integer = output_rate;

    const uint32_t rate_gcd = std::gcd(input_rate_integer, output_rate_integer);

    uint32_t simplified_output_rate = output_rate_integer / rate_gcd;

    // Calculate ideal phase count based on simplified ratio
    // For integer-ratio downsampling, we only need 1 phase since the filter
    // is always aligned the same way (e.g., 3:1 always starts at 0, 3, 6, 9...)
    // For upsampling, we need simplified_output_rate_ phases to avoid interpolation
    uint32_t ideal_phases = simplified_output_rate;

    // Use the minimum of ideal phases and design default, but respect min/max limits
    // Unless force_phase_count is set, then use exact design_phases
    uint32_t design_phases = static_cast<uint32_t>(design.get_default_phase_count());
    if (design.is_force_phase_count()) {
        this->num_phases_ = design_phases;
    } else {
        this->num_phases_ = std::min(ideal_phases, design_phases);
    }

    if (this->num_phases_ < 1) {
        this->num_phases_ = 1;
    }

    this->filter_length_ = design.get_filter_length();

    assert(this->filter_length_ <= MAX_FILTER_LENGTH);

    // Store configuration for deferred initialization
    this->config_.input_rate = input_rate;
    this->config_.output_rate = output_rate;
    this->config_.channels = channels;
    this->config_.design = design;

    // Initialize ratio tracker early (no memory allocation) so query methods work before
    // initialize()
    this->ratio_tracker_.init(input_rate, output_rate, this->filter_length_, this->num_phases_);
}

template <typename CoeffFormat>
MicroResamplerImpl<CoeffFormat>::~MicroResamplerImpl() {
    this->cleanup_resources();
}

template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::cleanup_resources() {
    free_and_null(this->history_buffer_);
    free_and_null(this->filter_bank_);
    free_and_null(this->coefficient_block_shifts_);
    free_and_null(this->max_tap_entries_);
    free_and_null(this->phase_to_row_);
}

// ============================================================================
// Format Configuration
// ============================================================================
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::dither_is_effective() const {
    return this->dither_stage() != DitherStage::None;
}

// ============================================================================
// Factory Method for Coefficient Generator
// ============================================================================
template <typename CoeffFormat>
std::unique_ptr<CoefficientGenerator> MicroResamplerImpl<CoeffFormat>::create_coefficient_generator(
    PrecisionStrategyType type) {
    switch (type) {
        case PrecisionStrategyType::SINGLE:
            return std::make_unique<
                CoefficientGeneratorImpl<CoeffFormat, SinglePrecisionStrategy>>();

        case PrecisionStrategyType::DOUBLE:
            return std::make_unique<
                CoefficientGeneratorImpl<CoeffFormat, DoublePrecisionStrategy>>();

        case PrecisionStrategyType::TABLE_FLOAT:
            return std::make_unique<CoefficientGeneratorImpl<CoeffFormat, TableFloatStrategy>>();

        case PrecisionStrategyType::TABLE_DOUBLE:
            return std::make_unique<CoefficientGeneratorImpl<CoeffFormat, TableDoubleStrategy>>();

        case PrecisionStrategyType::QUAD:
#ifdef RESAMPLER_ENABLE_QUAD_PRECISION
            return std::make_unique<CoefficientGeneratorImpl<CoeffFormat, QuadPrecisionStrategy>>();
#else
            // Without quad support compiled in, QUAD degrades to the platform
            // default rather than failing
            [[fallthrough]];
#endif
        case PrecisionStrategyType::DEFAULT:
        default:
            return std::make_unique<
                CoefficientGeneratorImpl<CoeffFormat, DefaultPrecisionStrategy>>();
    }
}

// ============================================================================
// Dither Stage Resolution
// ============================================================================
// Dither is applied at most once, at the sole point where a source carrying
// detail below 16 bits is quantized to Q15. If the input is already Q15 (no
// sub-16-bit detail) or dithering is disabled, there is nothing to dither.
// Otherwise the processing format decides where that reduction happens: Q15
// processing crushes the wide source to 16 bits at the input conversion; a wider
// processing format preserves it and the crush occurs at the output conversion,
// but only when the output is Q15.
template <typename CoeffFormat>
typename MicroResamplerImpl<CoeffFormat>::DitherStage
MicroResamplerImpl<CoeffFormat>::dither_stage() const {
    if (!this->dither_ || this->input_format_ == SampleFormat::Q15) {
        return DitherStage::None;
    }
    if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
        return DitherStage::Input;
    } else {
        return this->output_format_ == SampleFormat::Q15 ? DitherStage::Output : DitherStage::None;
    }
}

// ============================================================================
// Initialize RNG Storage for Dithering
// ============================================================================
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::init_rng_storage() {
    // Seed only the RNG bank for the single stage that actually dithers.
    switch (this->dither_stage()) {
        case DitherStage::Output:
            this->output_rng_states_.resize(this->num_channels_);
            this->output_rng_ptrs_.resize(this->num_channels_);
            for (int ch = 0; ch < this->num_channels_; ++ch) {
                this->output_rng_states_[ch] = initialize_rng_state(static_cast<uint32_t>(ch));
                this->output_rng_ptrs_[ch] = &this->output_rng_states_[ch];
            }
            break;
        case DitherStage::Input:
            this->input_rng_states_.resize(this->num_channels_);
            this->input_rng_ptrs_.resize(this->num_channels_);
            for (int ch = 0; ch < this->num_channels_; ++ch) {
                this->input_rng_states_[ch] = initialize_input_rng_state(static_cast<uint32_t>(ch));
                this->input_rng_ptrs_[ch] = &this->input_rng_states_[ch];
            }
            break;
        case DitherStage::None:
            break;
    }
}

// ============================================================================
// Initialize - allocates all resources
// ============================================================================
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::initialize() {
    // Already initialized - return success
    if (this->initialized_) {
        return true;
    }

    // Allocate filter bank dynamically based on actual filter length and phases
    size_t filter_bank_count = static_cast<size_t>(this->num_phases_) * this->filter_length_;

    this->filter_bank_ = MemoryUtils::allocate_aligned<typename CoeffFormat::coefficient_t>(
        filter_bank_count, 16,
        bank_alloc_strategy(filter_bank_count * sizeof(typename CoeffFormat::coefficient_t)));

    if (!this->filter_bank_) {
        this->cleanup_resources();
        return false;
    }

    memset(this->filter_bank_, 0, filter_bank_count * sizeof(typename CoeffFormat::coefficient_t));

    // Q15 always uses block adaptive Q for better precision
    // Q31/Float formats don't use adaptive Q (they have full precision already)
    constexpr int Q31_PRECISION_BITS = 31;  // Full-precision Q31 has no adaptive shift
    bool use_block_adaptive_q =
        !CoeffFormat::USE_FLOAT && (CoeffFormat::PRECISION_BITS != Q31_PRECISION_BITS);

    // Allocate coefficient_block_shifts_ only if needed for block mode.
    // Internal RAM preferred: this array is read on every output sample and is
    // small relative to the filter bank
    if (use_block_adaptive_q) {
        size_t coefficient_block_shifts_count = filter_bank_count / 8;
        // No alignment needed - these are accessed sequentially, not with SIMD
        this->coefficient_block_shifts_ = MemoryUtils::allocate<uint8_t>(
            coefficient_block_shifts_count, AllocStrategy::PREFER_INTERNAL);

        if (!this->coefficient_block_shifts_) {
            this->cleanup_resources();
            return false;
        }

        memset(this->coefficient_block_shifts_, CoeffFormat::PRECISION_BITS + 1,
               coefficient_block_shifts_count);  // Initialize to standard shift for format
    }

    // Allocate max tap extraction arrays if feature is enabled
    bool needs_max_tap_extraction =
        use_block_adaptive_q && this->config_.design.is_max_tap_extraction_enabled();
    if (needs_max_tap_extraction) {
        // Allocate interleaved struct array for max tap per phase
        // 8-byte alignment for efficient access of interleaved data.
        // Internal RAM preferred: one entry line is loaded per kernel call
        size_t max_tap_count = static_cast<size_t>(this->num_phases_) * this->max_taps_per_phase_;

        this->max_tap_entries_ = MemoryUtils::allocate_aligned<MaxTapEntry<CoeffFormat>>(
            max_tap_count, 8, AllocStrategy::PREFER_INTERNAL);

        if (!this->max_tap_entries_) {
            this->cleanup_resources();
            return false;
        }

        // Initialize to safe defaults
        std::uninitialized_value_construct_n(this->max_tap_entries_, max_tap_count);
    }

    // Allocate history buffer: filter_length frames of history plus room for one input
    // block (filter_length frames, or the configured max block size if larger)
    // This allows zero-copy reads when consumed >= filter_length
    // Blocks larger than the room allocated here reallocate the buffer on first use
    size_t history_buffer_frames = this->history_frames_for_block(
        std::max(static_cast<size_t>(this->filter_length_), this->initial_max_block_size_));
    if (history_buffer_frames == 0) {
        this->cleanup_resources();
        return false;  // Configured max block size is too large to address
    }
    size_t history_buffer_size = history_buffer_frames * this->num_channels_;
    this->history_buffer_ = MemoryUtils::allocate_aligned<internal_sample_t>(
        history_buffer_size, 16, AllocStrategy::INTERNAL_ONLY);

    if (!this->history_buffer_) {
        this->cleanup_resources();
        return false;
    }

    this->history_buffer_capacity_ = history_buffer_frames;

    // Initialize entire history buffer to zero
    memset(this->history_buffer_, 0, history_buffer_size * sizeof(internal_sample_t));

    // Generate coefficients directly into the allocated buffer
    // Use factory pattern to create appropriate precision strategy generator
    if (!this->coefficient_generator_) {
        this->coefficient_generator_ =
            this->create_coefficient_generator(this->precision_strategy_);
    }

    // Generate filter bank using the polymorphic interface
    this->coefficient_generator_->generate_filter_bank(
        this->filter_bank_, this->num_phases_, this->config_.design, this->config_.input_rate,
        this->config_.output_rate, nullptr, this->coefficient_block_shifts_, this->max_tap_entries_,
        this->max_taps_per_phase_);

    // Initialize workspace buffers for interpolation (avoids VLAs)
    this->temp_output_.resize(this->num_channels_);
    this->temp_output2_.resize(this->num_channels_);

    // Reorder bank rows into the aligned fixed-mode visit order so consecutive
    // outputs read sequential memory (matters when the bank lives in PSRAM)
    this->permute_filter_bank_rows();

    // Initialize RNG storage for dithering
    this->init_rng_storage();

    // Create the configured extrapolator, applying any explicit LPC order, and allocate its
    // scratch now so prime/flush never allocate while streaming
    this->extrapolator_ = create_extrapolator<CoeffFormat>(this->extrapolation_mode_);
    if (this->extrapolator_lpc_order_ > 0) {
        this->extrapolator_->set_lpc_order(this->extrapolator_lpc_order_);
    }
    if (!this->extrapolator_->allocate(this->filter_length_)) {
        this->extrapolator_.reset();
        this->cleanup_resources();
        return false;
    }

    // Note: ratio_tracker was already initialized in constructor to allow
    // ASRC methods (set_ratio, adjust_ppm, etc.) to work before initialize().
    // Do NOT re-initialize here as that would overwrite user's ASRC settings.

    // Reset prime/flush state only (ratio tracker state preserved)
    this->primed_ = false;
    this->flushed_ = false;
    this->flush_remaining_frames_ = 0;

    this->initialized_ = true;
    return true;
}

template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::reset() {
    // Reset prime/flush state (always safe)
    this->primed_ = false;
    this->flushed_ = false;
    this->flush_remaining_frames_ = 0;

    // If not initialized, nothing else to reset
    if (!this->initialized_) {
        return;
    }

    // Clear the entire history buffer (may be larger than 2*filter_length when input conversion is
    // used)
    if (this->history_buffer_) {
        memset(this->history_buffer_, 0,
               this->history_buffer_capacity_ * this->num_channels_ * sizeof(internal_sample_t));
    }

    // Reset the ratio tracker (handles all phase state initialization)
    this->ratio_tracker_.reset();
}

// Prime the history buffer with reflected samples for smooth startup
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::prime_raw(const void* initial_samples, size_t frame_count) {
    // Lazy initialization
    if (!this->initialized_) {
        if (!this->initialize()) {
            return false;
        }
    }

    // Extrapolators only look at the first filter_length frames
    size_t prime_frames = std::min(frame_count, static_cast<size_t>(this->filter_length_));

    // Stage the converted frames in the history buffer's input section [filter_length,
    // 2 * filter_length), which holds nothing between process calls. Priming writes only the
    // history section [0, filter_length), so the two never overlap.
    internal_sample_t* staged =
        &this->history_buffer_[static_cast<size_t>(this->filter_length_) * this->num_channels_];
    this->convert_input(initial_samples, staged, prime_frames);

    this->extrapolator_->prime_buffer(this->history_buffer_, this->num_channels_, staged,
                                      static_cast<int>(prime_frames), this->filter_length_);

    this->primed_ = true;
    return true;
}

// Get LPC order from current extrapolator
template <typename CoeffFormat>
int MicroResamplerImpl<CoeffFormat>::get_extrapolator_lpc_order() const {
    if (this->initialized_ && this->extrapolator_) {
        return this->extrapolator_->get_lpc_order();
    }
    return 0;  // Not initialized, return default
}

template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::process_block_raw(const void* input, size_t input_frames,
                                                        void* output, size_t output_capacity,
                                                        size_t& frames_consumed,
                                                        size_t& frames_generated) {
    // Lazy initialization
    if (!this->initialized_) {
        if (!this->initialize()) {
            frames_consumed = 0;
            frames_generated = 0;
            return false;
        }
    }

    ProcessResult result = this->process_block_impl(input, input_frames, output, output_capacity);
    frames_consumed = result.input_frames_consumed;
    frames_generated = result.output_frames_generated;
    return result.ok;
}

// Flush remaining samples using FFmpeg-style signal reflection
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::flush_raw(void* output, size_t output_capacity,
                                                size_t& frames_generated) {
    frames_generated = 0;

    // Lazy initialization
    if (!this->initialized_) {
        if (!this->initialize()) {
            return false;
        }
    }

    // Already flushed - return success with zero frames
    if (this->flushed_) {
        return true;
    }

    // First call: set up extrapolated samples in history buffer
    if (this->flush_remaining_frames_ == 0) {
        this->extrapolator_->prepare_flush(this->history_buffer_, this->num_channels_,
                                           this->filter_length_);
        // Add 1 extra frame to ensure we generate enough output samples when upsampling,
        // since each input frame produces more than one output frame on average. Must match
        // the filter_length / 2 + 1 frames prepare_flush() extrapolates.
        this->flush_remaining_frames_ = this->filter_length_ / 2 + 1;
    }

    // Use internal flush implementation that processes directly from history buffer
    ProcessResult result = this->flush_impl(output, output_capacity);

    // Update remaining frames
    this->flush_remaining_frames_ -= result.input_frames_consumed;

    // Mark as flushed when all reflected samples consumed
    if (this->flush_remaining_frames_ == 0) {
        this->flushed_ = true;
    }

    frames_generated = result.output_frames_generated;
    return true;
}

// Helper: Check if we have enough samples for phase wraparound interpolation
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::check_phase_wrap_around(const PhaseInfo& phase_info,
                                                              size_t consumed,
                                                              size_t input_frames) const {
    if (phase_info.blend_factor > 0) {
        uint16_t next_phase = phase_info.phase_index + 1;
        if (next_phase == this->num_phases_) {
            // Phase will wrap - need one extra frame for interpolation
            if (consumed >= static_cast<size_t>(this->filter_length_)) {
                // Zero-copy mode: ensure we have enough samples in input buffer
                if (consumed + 1 > input_frames) {
                    return true;  // Should break - not enough samples
                }
            }
            // History buffer mode always has enough (capacity >= 2*filter_length)
        }
    }
    return false;  // Continue processing
}

// Helper: Get read pointer based on consumed count
template <typename CoeffFormat>
const typename MicroResamplerImpl<CoeffFormat>::internal_sample_t*
MicroResamplerImpl<CoeffFormat>::get_read_ptr(size_t consumed) const {
    // All data is in history buffer (including converted input)
    return &this->history_buffer_[consumed * this->num_channels_];
}

// Helper: Update history buffer after processing
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::update_history_buffer(const void* /*interleaved_input*/,
                                                            size_t consumed) {
    // All data is in history buffer, just shift forward
    if (consumed > 0) {
        std::memmove(this->history_buffer_, &this->history_buffer_[consumed * this->num_channels_],
                     static_cast<size_t>(this->filter_length_) * this->num_channels_ *
                         sizeof(internal_sample_t));
    }
}

// Helper: Prepare history buffer to receive input frames
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::copy_input_to_history(const void* interleaved_input,
                                                            size_t input_frames) {
    // Convert the whole block up front: the end-of-block history shift needs
    // the full input section converted regardless of how many frames the
    // processing loop consumes, so eager conversion adds no work and the loop
    // needs no per-output conversion checks
    if (!this->ensure_history_buffer_capacity(input_frames)) {
        return false;
    }
    this->convert_input(
        interleaved_input,
        &this->history_buffer_[static_cast<size_t>(this->filter_length_) * this->num_channels_],
        input_frames);
    return true;
}

// Reorder filter bank rows into the aligned fixed-mode visit order.
// The aligned path advances the phase by (P mod Q) per output (P/Q = the
// GCD-simplified input/output rates, Q == num_phases), so it walks the bank
// with a fixed stride. Storing row (p * inv(P)) mod Q at physical position p's
// visit slot makes consecutive outputs read consecutive rows, turning strided
// PSRAM traffic into sequential traffic. All per-row companions
// (coefficient_block_shifts_, max_tap_entries_) are permuted identically, and
// phase_to_row_ maps logical phase to physical row for every other consumer
// (general/ASRC paths after a mode transition, flush).
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::permute_filter_bank_rows() {
    if (!this->ratio_tracker_.is_fixed_aligned() || this->num_phases_ <= 1) {
        return;
    }

    const uint32_t num_rows = this->num_phases_;
    const uint32_t stride = this->ratio_tracker_.get_input_rate() % num_rows;
    if (stride <= 1) {
        return;  // Visit order is already sequential
    }

    // Modular inverse of the stride (exists because gcd(P, Q) == 1 after rate
    // simplification); num_phases is small, so a linear search is fine at init
    uint32_t stride_inverse = 0;
    for (uint32_t candidate = 1; candidate < num_rows; ++candidate) {
        if ((candidate * stride) % num_rows == 1) {
            stride_inverse = candidate;
            break;
        }
    }
    if (stride_inverse == 0) {
        return;  // Defensive: no inverse found, keep natural order
    }

    // Allocate every reorder buffer up front; skip the whole permutation if
    // any allocation fails so the three arrays always stay consistent
    const size_t row_coeffs = this->filter_length_;
    const size_t bank_count = static_cast<size_t>(num_rows) * row_coeffs;
    auto* new_bank = MemoryUtils::allocate_aligned<typename CoeffFormat::coefficient_t>(
        bank_count, 16,
        bank_alloc_strategy(bank_count * sizeof(typename CoeffFormat::coefficient_t)));
    uint16_t* map = MemoryUtils::allocate<uint16_t>(num_rows, AllocStrategy::PREFER_INTERNAL);

    uint8_t* new_shifts = nullptr;
    const size_t row_shift_bytes = row_coeffs / 8;
    if (this->coefficient_block_shifts_ != nullptr) {
        new_shifts = MemoryUtils::allocate<uint8_t>(static_cast<size_t>(num_rows) * row_shift_bytes,
                                                    AllocStrategy::PREFER_INTERNAL);
    }

    MaxTapEntry<CoeffFormat>* new_entries = nullptr;
    const size_t row_entries = this->max_taps_per_phase_;
    if (this->max_tap_entries_ != nullptr) {
        new_entries = MemoryUtils::allocate_aligned<MaxTapEntry<CoeffFormat>>(
            static_cast<size_t>(num_rows) * row_entries, 8, AllocStrategy::PREFER_INTERNAL);
    }

    const bool shifts_ok = (this->coefficient_block_shifts_ == nullptr) || (new_shifts != nullptr);
    const bool entries_ok = (this->max_tap_entries_ == nullptr) || (new_entries != nullptr);
    if (new_bank == nullptr || map == nullptr || !shifts_ok || !entries_ok) {
        MemoryUtils::free(new_bank);
        MemoryUtils::free(map);
        MemoryUtils::free(new_shifts);
        MemoryUtils::free(new_entries);
        return;  // Keep natural order
    }

    for (uint32_t phase = 0; phase < num_rows; ++phase) {
        const uint32_t row = (phase * stride_inverse) % num_rows;
        map[phase] = static_cast<uint16_t>(row);
        memcpy(&new_bank[row * row_coeffs], &this->filter_bank_[phase * row_coeffs],
               row_coeffs * sizeof(typename CoeffFormat::coefficient_t));
        if (new_shifts != nullptr) {
            memcpy(&new_shifts[row * row_shift_bytes],
                   &this->coefficient_block_shifts_[phase * row_shift_bytes], row_shift_bytes);
        }
        if (new_entries != nullptr) {
            memcpy(&new_entries[row * row_entries], &this->max_tap_entries_[phase * row_entries],
                   row_entries * sizeof(MaxTapEntry<CoeffFormat>));
        }
    }

    MemoryUtils::free(this->filter_bank_);
    this->filter_bank_ = new_bank;
    if (new_shifts != nullptr) {
        MemoryUtils::free(this->coefficient_block_shifts_);
        this->coefficient_block_shifts_ = new_shifts;
    }
    if (new_entries != nullptr) {
        MemoryUtils::free(this->max_tap_entries_);
        this->max_tap_entries_ = new_entries;
    }
    this->phase_to_row_ = map;
}

// Fill in the DotProdConfig fields that hold for every output sample of a
// block; compute_dotprod() then only swaps the per-phase pointers
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::init_dotprod_config(
    dotprod::DotProdConfig<CoeffFormat>& config) const {
    config.samples = nullptr;
    config.coeffs = nullptr;
    config.length = this->filter_length_;
    config.num_channels = this->num_channels_;
    config.block_shifts = nullptr;
    config.max_tap_entries = nullptr;
    config.max_taps_count = (this->max_tap_entries_ != nullptr) ? this->max_taps_per_phase_ : 0;
}

template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::compute_dotprod(dotprod::DotProdConfig<CoeffFormat>& config,
                                                      const internal_sample_t* samples,
                                                      uint16_t phase_index, accumulator_t* output) {
    // Map the logical phase to its physical bank row (identity when the bank
    // is in natural order; visit order when the aligned permutation is active)
    const size_t row =
        (this->phase_to_row_ != nullptr) ? this->phase_to_row_[phase_index] : phase_index;

    // Update only the per-output fields of the pre-built configuration
    config.samples = samples;
    config.coeffs = &this->filter_bank_[row * this->filter_length_];

    // Set up block shifts (PATH 1)
    if (this->coefficient_block_shifts_ != nullptr) {
        config.block_shifts = &this->coefficient_block_shifts_[(row * this->filter_length_) / 8];
    }

    // Set up max tap extraction
    if (this->max_tap_entries_ != nullptr) {
        config.max_tap_entries = &this->max_tap_entries_[row * this->max_taps_per_phase_];
    }

    // Dispatch to centralized implementation
    MR_PROF(uint32_t prof_kernel = profile::ccount();)
    dotprod::compute(config, output);
    MR_PROF(profile::mark(prof_kernel, profile::g_cycles.kernel); ++profile::g_cycles.kernel_calls;)
}

// Helper to write a single output sample using a concrete SampleType (no virtual dispatch)
template <typename SampleType, typename AccT>
static inline void write_output_sample(void* dst, AccT acc, uint32_t* rng) {
    if constexpr (SampleType::IS_PACKED) {
        SampleType::from_accumulator_packed(static_cast<uint8_t*>(dst), acc, rng);
    } else {
        *static_cast<typename SampleType::sample_t*>(dst) = SampleType::from_accumulator(acc, rng);
    }
}

template <typename CoeffFormat>
template <typename OutputSampleType>
void MicroResamplerImpl<CoeffFormat>::interpolate_frame_impl(
    dotprod::DotProdConfig<CoeffFormat>& config, const internal_sample_t* read_ptr,
    uint16_t phase_index, int32_t blend_factor, void* output) {
    accumulator_t* temp_output = this->temp_output_.data();

    // Compute first phase using helper method
    this->compute_dotprod(config, read_ptr, phase_index, temp_output);

    // Compile-time output byte size
    constexpr size_t OUTPUT_BYTES_PER_SAMPLE = []() {
        if constexpr (OutputSampleType::IS_PACKED) {
            return OutputSampleType::BYTES_PER_SAMPLE;
        } else {
            return sizeof(typename OutputSampleType::sample_t);
        }
    }();

    // Compile-time RNG resolution: IS_DITHERED is constexpr on the SampleConverter type,
    // so the dead branch is eliminated entirely (no per-sample empty() check)
    auto get_output_rng = [this]([[maybe_unused]] int ch) -> uint32_t* {
        if constexpr (OutputSampleType::IS_DITHERED) {
            return this->output_rng_ptrs_[ch];
        } else {
            // The `this` capture is unused on this branch, which -Wunused-lambda-capture
            // can't see past `if constexpr` (it isn't dead per-TU, just per-instantiation).
            (void)this;
            return nullptr;
        }
    };

    // Optimization: Skip second phase computation when blend is zero
    if (blend_factor == 0) {
        // Convert each channel
        if (this->num_channels_ == 1) {
            write_output_sample<OutputSampleType>(output, temp_output[0], get_output_rng(0));
            return;
        }
        if (this->num_channels_ == 2) {
            write_output_sample<OutputSampleType>(output, temp_output[0], get_output_rng(0));
            write_output_sample<OutputSampleType>(
                static_cast<uint8_t*>(output) + OUTPUT_BYTES_PER_SAMPLE, temp_output[1],
                get_output_rng(1));
            return;
        }

        for (int ch = 0; ch < this->num_channels_; ++ch) {
            write_output_sample<OutputSampleType>(
                static_cast<uint8_t*>(output) + ch * OUTPUT_BYTES_PER_SAMPLE, temp_output[ch],
                get_output_rng(ch));
        }
        return;
    }

    // blend_factor != 0
    // Compute second phase for interpolation (handles wraparound)
    auto [next_phase, offset] = this->compute_next_phase(phase_index);

    // Compute second phase using same helper method
    // NOTE: Always use full filter_length, even when offset=1 (wraparound)
    // The caller has already verified we have enough samples buffered
    accumulator_t* temp_output2 = this->temp_output2_.data();
    this->compute_dotprod(config, read_ptr + offset * this->num_channels_, next_phase,
                          temp_output2);

    // Blend the two phases at full accumulator precision, then convert to output
    for (int ch = 0; ch < this->num_channels_; ++ch) {
        accumulator_t blended = AccumulatorBlender<CoeffFormat>::blend(
            temp_output[ch], temp_output2[ch], blend_factor, BLEND_FACTOR_BITS);
        write_output_sample<OutputSampleType>(
            static_cast<uint8_t*>(output) + ch * OUTPUT_BYTES_PER_SAMPLE, blended,
            get_output_rng(ch));
    }
}

// Dispatch wrapper: switches on output_format_ once per block. Only Q15 output
// can dither, and only when processing in a wider format reduces a wider-than-
// 16-bit source to 16 bits here (dither_stage() == Output). Q15 processing and
// all >=24-bit outputs are written verbatim.
template <typename CoeffFormat>
typename MicroResamplerImpl<CoeffFormat>::ProcessResult
MicroResamplerImpl<CoeffFormat>::process_block_impl(const void* interleaved_input,
                                                    size_t input_frames, void* interleaved_output,
                                                    size_t output_capacity) {
    switch (this->output_format_) {
        case SampleFormat::Q15:
            if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
                // Q15 processing never dithers on output (16-bit source floor).
                return process_block_inner<Q15SampleConverter<CoeffFormat, false>>(
                    interleaved_input, input_frames, interleaved_output, output_capacity);
            } else {
                if (this->dither_stage() == DitherStage::Output) {
                    return process_block_inner<Q15SampleConverter<CoeffFormat, true>>(
                        interleaved_input, input_frames, interleaved_output, output_capacity);
                }
                return process_block_inner<Q15SampleConverter<CoeffFormat, false>>(
                    interleaved_input, input_frames, interleaved_output, output_capacity);
            }
        case SampleFormat::Q23:
            return process_block_inner<Q23SampleConverter<CoeffFormat, false>>(
                interleaved_input, input_frames, interleaved_output, output_capacity);
        case SampleFormat::Q23_PACKED:
            return process_block_inner<Q23PackedSampleConverter<CoeffFormat, false>>(
                interleaved_input, input_frames, interleaved_output, output_capacity);
        case SampleFormat::Q31:
            return process_block_inner<Q31SampleConverter<CoeffFormat, false>>(
                interleaved_input, input_frames, interleaved_output, output_capacity);
        case SampleFormat::FLOAT32:
            return process_block_inner<Float32SampleConverter<CoeffFormat, false>>(
                interleaved_input, input_frames, interleaved_output, output_capacity);
        case SampleFormat::FLOAT64:
            return process_block_inner<Float64SampleConverter<CoeffFormat, false>>(
                interleaved_input, input_frames, interleaved_output, output_capacity);
    }
    return {0, 0};
}

// Templated inner loop - output format resolved at compile time
template <typename CoeffFormat>
template <typename OutputSampleType>
typename MicroResamplerImpl<CoeffFormat>::ProcessResult
MicroResamplerImpl<CoeffFormat>::process_block_inner(const void* interleaved_input,
                                                     size_t input_frames, void* interleaved_output,
                                                     size_t output_capacity) {
    ProcessResult result = {0, 0};

    if (input_frames == 0 || output_capacity == 0) {
        return result;
    }

    MR_PROF(uint32_t prof_block = profile::ccount(); ++profile::g_cycles.blocks;)

    // STEP 1: Prepare history buffer and convert the incoming input frames
    // Abort before any conversion writes if the buffer cannot be grown
    if (!this->copy_input_to_history(interleaved_input, input_frames)) {
        result.ok = false;
        return result;
    }

    size_t consumed = 0;
    size_t generated = 0;

    // Handle initial skip for latency compensation (tracker handles mode internally)
    size_t skip_count = this->ratio_tracker_.get_initial_skip_count();
    if (skip_count > 0) {
        size_t samples_to_skip = this->ratio_tracker_.consume_initial_skip(input_frames);
        consumed += samples_to_skip;

        if (this->ratio_tracker_.get_initial_skip_count() > 0) {
            // Not enough input to complete initial skip
            this->update_history_buffer(interleaved_input, consumed);
            result.input_frames_consumed = consumed;
            return result;
        }
    }

    // Build the dot product configuration once per block; the processing loop
    // only swaps its per-phase pointers
    dotprod::DotProdConfig<CoeffFormat> dotprod_config{};
    this->init_dotprod_config(dotprod_config);

    // Compile-time output stride
    constexpr size_t OUTPUT_BYTES_PER_SAMPLE = []() {
        if constexpr (OutputSampleType::IS_PACKED) {
            return OutputSampleType::BYTES_PER_SAMPLE;
        } else {
            return sizeof(typename OutputSampleType::sample_t);
        }
    }();
    const size_t output_frame_stride = OUTPUT_BYTES_PER_SAMPLE * this->num_channels_;

    // STEP 2: Process outputs
    if (this->ratio_tracker_.is_fixed_aligned()) {
        // FAST PATH: aligned fixed mode. The accumulator encodes
        // samples_to_consume * Q + phase_index (Q = simplified output rate ==
        // num_phases), so the div/mod of calculate_phase() reduces to an
        // incremental DDA update: add the simplified input rate per output,
        // consume one input frame per conditional subtract of Q. Consumption
        // folds into the emit pass instead of taking its own loop iteration,
        // and the aligned path never blends, so no wraparound check is needed.
        const uint32_t step = this->ratio_tracker_.get_input_rate();
        const uint32_t modulus = this->ratio_tracker_.get_output_rate();
        uint32_t acc = this->ratio_tracker_.get_aligned_accumulator();

        // Emit state hoisted out of the loop: per-phase pointer bases, the
        // accumulator scratch buffer, and the compile-time RNG resolution
        // (see interpolate_frame_impl / compute_dotprod)
        const typename CoeffFormat::coefficient_t* const bank_base = this->filter_bank_;
        const uint16_t* const phase_to_row = this->phase_to_row_;
        const uint8_t* const shift_base = this->coefficient_block_shifts_;
        const MaxTapEntry<CoeffFormat>* const entries_base = this->max_tap_entries_;
        const size_t row_length = this->filter_length_;
        const size_t row_shift_stride = row_length / 8;
        const size_t row_tap_stride = this->max_taps_per_phase_;
        accumulator_t* const temp_output = this->temp_output_.data();
        auto get_output_rng = [this]([[maybe_unused]] int ch) -> uint32_t* {
            if constexpr (OutputSampleType::IS_DITHERED) {
                return this->output_rng_ptrs_[ch];
            } else {
                // The `this` capture is unused on this branch (see the note in
                // interpolate_frame_impl)
                (void)this;
                return nullptr;
            }
        };

        // Batched sub-path: Q15 mono/stereo with block shifts and a sequential
        // row visit order (permuted bank, natural stride-1 order, or a
        // single-phase bank) can compute up to four outputs per kernel call,
        // amortizing the call, prologue, and dispatch overhead across the
        // batch. Falls through to the single-output loop otherwise.
        bool used_batched_kernel = false;
#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_RISCV_P4)
        if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            const bool fixed_row = (modulus == 1);
            const bool rows_sequential =
                fixed_row || (step % modulus <= 1) || (phase_to_row != nullptr);

            if (rows_sequential && shift_base != nullptr && this->num_channels_ <= 2) {
                used_batched_kernel = true;

                constexpr size_t MAX_BATCH = 4;
                const internal_sample_t* sample_ptrs[MAX_BATCH];
                accumulator_t batch_acc[MAX_BATCH * 2];
                const internal_sample_t* const history_base = this->history_buffer_;
                const size_t num_channels = this->num_channels_;
                const int tap_count =
                    (entries_base != nullptr) ? static_cast<int>(row_tap_stride) : 0;
                bool stop = false;

                while (!stop && generated < output_capacity && consumed < input_frames) {
                    MR_PROF(uint32_t prof_t = profile::ccount();)

                    // Run the DDA ahead of the kernel call, mirroring the
                    // single-output loop's consume/emit decisions per output
                    size_t max_n = std::min(MAX_BATCH, output_capacity - generated);
                    size_t row0 = 0;
                    size_t n = 0;
                    while (n < max_n) {
                        MR_PROF(++profile::g_cycles.iterations;)
                        if (acc >= modulus) {
                            do {
                                acc -= modulus;
                                ++consumed;
                            } while (acc >= modulus && consumed < input_frames);
                            MR_PROF(++profile::g_cycles.consume_iters;)
                            if (acc >= modulus || consumed == input_frames) {
                                stop = true;
                                break;
                            }
                        }
                        if (n == 0) {
                            row0 = (phase_to_row != nullptr) ? phase_to_row[acc] : acc;
                            if (!fixed_row) {
                                // The sequential row walk wraps at the end of
                                // the bank; cap the batch at the boundary
                                max_n = std::min(max_n, static_cast<size_t>(modulus) - row0);
                            }
                        }
                        sample_ptrs[n] = &history_base[consumed * num_channels];
                        ++n;
                        acc += step;
                    }
                    if (n == 0) {
                        break;
                    }
                    MR_PROF(profile::mark(prof_t, profile::g_cycles.prep);)

                    MR_PROF(uint32_t prof_kernel = profile::ccount();)
#if defined(DOTPROD_PLATFORM_XTENSA_S3)
                    dotprod::xtensa_s3::compute_block_shift_batch(
#else
                    dotprod::riscv_p4::compute_block_shift_batch(
#endif
                        sample_ptrs, &bank_base[row0 * row_length],
                        &shift_base[row0 * row_shift_stride],
                        (entries_base != nullptr) ? &entries_base[row0 * row_tap_stride] : nullptr,
                        static_cast<int>(row_length), tap_count, static_cast<int>(n), fixed_row,
                        static_cast<int>(num_channels), batch_acc);
                    MR_PROF(profile::mark(prof_kernel, profile::g_cycles.kernel);
                            ++profile::g_cycles.kernel_calls;)

                    uint8_t* output_ptr =
                        static_cast<uint8_t*>(interleaved_output) + generated * output_frame_stride;
                    if (num_channels == 1) {
                        for (size_t i = 0; i < n; ++i) {
                            write_output_sample<OutputSampleType>(
                                output_ptr + i * output_frame_stride, batch_acc[i],
                                get_output_rng(0));
                        }
                    } else {
                        for (size_t i = 0; i < n; ++i) {
                            uint8_t* dst = output_ptr + i * output_frame_stride;
                            write_output_sample<OutputSampleType>(dst, batch_acc[2 * i],
                                                                  get_output_rng(0));
                            write_output_sample<OutputSampleType>(dst + OUTPUT_BYTES_PER_SAMPLE,
                                                                  batch_acc[(2 * i) + 1],
                                                                  get_output_rng(1));
                        }
                    }
                    generated += n;
                    MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)
                }
            }
        }
#endif  // DOTPROD_PLATFORM_XTENSA_S3 || DOTPROD_PLATFORM_RISCV_P4

        while (!used_batched_kernel && generated < output_capacity && consumed < input_frames) {
            MR_PROF(uint32_t prof_t = profile::ccount(); ++profile::g_cycles.iterations;)

            if (acc >= modulus) {
                do {
                    acc -= modulus;
                    ++consumed;
                } while (acc >= modulus && consumed < input_frames);

                MR_PROF(profile::mark(prof_t, profile::g_cycles.consume);
                        ++profile::g_cycles.consume_iters;)

                // Stop when input ran out mid-consume or nothing is left to
                // read (mirrors the generic loop's per-pass entry condition)
                if (acc >= modulus || consumed == input_frames) {
                    break;
                }
            }

            const internal_sample_t* read_ptr = this->get_read_ptr(consumed);
            void* output_ptr =
                static_cast<uint8_t*>(interleaved_output) + generated * output_frame_stride;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.prep);)

            // Emit inline: the aligned path never blends, so this is the
            // blend-free body of interpolate_frame_impl and compute_dotprod
            // without their call layers, using the hoisted pointer bases
            const size_t row = (phase_to_row != nullptr) ? phase_to_row[acc] : acc;
            dotprod_config.samples = read_ptr;
            dotprod_config.coeffs = &bank_base[row * row_length];
            if (shift_base != nullptr) {
                dotprod_config.block_shifts = &shift_base[row * row_shift_stride];
            }
            if (entries_base != nullptr) {
                dotprod_config.max_tap_entries = &entries_base[row * row_tap_stride];
            }

            MR_PROF(uint32_t prof_kernel = profile::ccount();)
            dotprod::compute(dotprod_config, temp_output);
            MR_PROF(profile::mark(prof_kernel, profile::g_cycles.kernel);
                    ++profile::g_cycles.kernel_calls;)

            if (this->num_channels_ == 1) {
                write_output_sample<OutputSampleType>(output_ptr, temp_output[0],
                                                      get_output_rng(0));
            } else if (this->num_channels_ == 2) {
                write_output_sample<OutputSampleType>(output_ptr, temp_output[0],
                                                      get_output_rng(0));
                write_output_sample<OutputSampleType>(
                    static_cast<uint8_t*>(output_ptr) + OUTPUT_BYTES_PER_SAMPLE, temp_output[1],
                    get_output_rng(1));
            } else {
                for (int ch = 0; ch < this->num_channels_; ++ch) {
                    write_output_sample<OutputSampleType>(
                        static_cast<uint8_t*>(output_ptr) + ch * OUTPUT_BYTES_PER_SAMPLE,
                        temp_output[ch], get_output_rng(ch));
                }
            }

            MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)

            acc += step;
            generated++;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.post);)
        }

        this->ratio_tracker_.set_aligned_accumulator(acc);
    } else if (this->ratio_tracker_.is_asrc_mode()) {
        // ASRC FAST PATH. The generic loop spends two tracker round trips per
        // output (a consume pass and an emit pass, each calling
        // calculate_phase()); here the 4.28 accumulator lives in a register,
        // consume and emit fuse into one pass per output, and the emit inlines
        // the blend body of interpolate_frame_impl with hoisted pointer bases,
        // mirroring the aligned fast path above. Arithmetic is bit-identical:
        // phase decode via decode_asrc_phase(), same dot products, same
        // AccumulatorBlender<CoeffFormat>::blend().
        //
        // The ratio is a register too. While a slew is active, each output
        // steps it through the tracker's own step_slew() (the same ramp as
        // update_after_output()), called out of line; a steady ratio pays one
        // predictable branch.
        // For the subsample offset, slewed_ratio_sum collects the ratios of
        // the slewed outputs, which all precede the steady ones in a block;
        // every later output advanced by the final ratio.
        uint32_t ratio = this->ratio_tracker_.get_current_ratio_fixed();
        bool slewing = this->ratio_tracker_.is_slewing();
        uint32_t slewed_ratio_sum = 0;
        uint32_t slewed_outputs = 0;
        const uint32_t num_phases = this->ratio_tracker_.get_num_phases();
        uint32_t acc = this->ratio_tracker_.get_asrc_accumulator();

        // Emit state hoisted out of the loop (see the aligned fast path)
        const typename CoeffFormat::coefficient_t* const bank_base = this->filter_bank_;
        const uint16_t* const phase_to_row = this->phase_to_row_;
        const uint8_t* const shift_base = this->coefficient_block_shifts_;
        const MaxTapEntry<CoeffFormat>* const entries_base = this->max_tap_entries_;
        const size_t row_length = this->filter_length_;
        const size_t row_shift_stride = row_length / 8;
        const size_t row_tap_stride = this->max_taps_per_phase_;
        const internal_sample_t* const history_base = this->history_buffer_;
        const size_t num_channels = this->num_channels_;
        accumulator_t* const temp_output = this->temp_output_.data();
        accumulator_t* const temp_output2 = this->temp_output2_.data();
        auto get_output_rng = [this]([[maybe_unused]] int ch) -> uint32_t* {
            if constexpr (OutputSampleType::IS_DITHERED) {
                return this->output_rng_ptrs_[ch];
            } else {
                // The `this` capture is unused on this branch (see the note in
                // interpolate_frame_impl)
                (void)this;
                return nullptr;
            }
        };

        // Per-row config swap shared by both blend rows of an output
        auto point_config_at_row = [&](uint32_t phase) {
            const size_t row = (phase_to_row != nullptr) ? phase_to_row[phase] : phase;
            dotprod_config.coeffs = &bank_base[row * row_length];
            if (shift_base != nullptr) {
                dotprod_config.block_shifts = &shift_base[row * row_shift_stride];
            }
            if (entries_base != nullptr) {
                dotprod_config.max_tap_entries = &entries_base[row * row_tap_stride];
            }
        };

        // Dual-row kernel eligibility, hoisted per block: one call computes
        // both blend rows' dot products, loading each sample vector once.
        // Q15 needs the block-shift path on the S3 or P4; Q31Lite's scalar
        // mulsh/mulh kernels run on every Xtensa target and the P4; Float32
        // uses the S3 or P4 FPU dual kernels. Outputs whose blend pair wraps
        // to phase 0 (a one-frame sample advance for row B) fall back to the
        // generic emit below.
#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_RISCV_P4)
        constexpr bool KERNEL_DUAL_Q15 = std::is_same_v<CoeffFormat, Q15Format>;
        constexpr bool KERNEL_DUAL_F32 = std::is_same_v<CoeffFormat, Float32Format>;
#else
        constexpr bool KERNEL_DUAL_Q15 = false;
        constexpr bool KERNEL_DUAL_F32 = false;
#endif
#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_XTENSA_MAC16) || \
    defined(DOTPROD_PLATFORM_RISCV_P4)
        constexpr bool KERNEL_DUAL_Q31LITE = std::is_same_v<CoeffFormat, Q31LiteFormat>;
#else
        constexpr bool KERNEL_DUAL_Q31LITE = false;
#endif
        [[maybe_unused]] const bool dual_kernel_ok = ((KERNEL_DUAL_Q15 && shift_base != nullptr) ||
                                                      KERNEL_DUAL_Q31LITE || KERNEL_DUAL_F32) &&
                                                     num_channels <= 2;
        [[maybe_unused]] const int dual_tap_count =
            (entries_base != nullptr) ? static_cast<int>(row_tap_stride) : 0;

        // Two-call emit: the blend body of interpolate_frame_impl without its
        // call layers, using the hoisted pointer bases. Used by the scalar
        // loop for every output and by the batched loop for outputs whose
        // blend pair wraps to phase 0 (a one-frame sample advance for row B).
        auto emit_two_call = [&](const PhaseInfo& phase_info, const internal_sample_t* read_ptr,
                                 void* output_ptr) {
            dotprod_config.samples = read_ptr;
            point_config_at_row(phase_info.phase_index);

            MR_PROF(uint32_t prof_kernel = profile::ccount();)
            dotprod::compute(dotprod_config, temp_output);
            MR_PROF(profile::mark(prof_kernel, profile::g_cycles.kernel);
                    ++profile::g_cycles.kernel_calls;)

            if (phase_info.blend_factor == 0) {
                for (size_t ch = 0; ch < num_channels; ++ch) {
                    write_output_sample<OutputSampleType>(
                        static_cast<uint8_t*>(output_ptr) + ch * OUTPUT_BYTES_PER_SAMPLE,
                        temp_output[ch], get_output_rng(static_cast<int>(ch)));
                }
                return;
            }

            // Second blend row, with wraparound to phase 0 / +1 frame
            uint32_t next_phase = phase_info.phase_index + 1U;
            size_t offset = 0;
            if (next_phase == num_phases) {
                next_phase = 0;
                offset = 1;
            }
            dotprod_config.samples = read_ptr + offset * num_channels;
            point_config_at_row(next_phase);

            MR_PROF(uint32_t prof_kernel2 = profile::ccount();)
            dotprod::compute(dotprod_config, temp_output2);
            MR_PROF(profile::mark(prof_kernel2, profile::g_cycles.kernel);
                    ++profile::g_cycles.kernel_calls;)

            for (size_t ch = 0; ch < num_channels; ++ch) {
                accumulator_t blended = AccumulatorBlender<CoeffFormat>::blend(
                    temp_output[ch], temp_output2[ch], phase_info.blend_factor, BLEND_FACTOR_BITS);
                write_output_sample<OutputSampleType>(
                    static_cast<uint8_t*>(output_ptr) + ch * OUTPUT_BYTES_PER_SAMPLE, blended,
                    get_output_rng(static_cast<int>(ch)));
            }
        };

        bool used_dual_loop = false;
#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_XTENSA_MAC16) || \
    defined(DOTPROD_PLATFORM_RISCV_P4)
        if constexpr (KERNEL_DUAL_Q15 || KERNEL_DUAL_Q31LITE || KERNEL_DUAL_F32) {
            if (dual_kernel_ok) {
                used_dual_loop = true;

                // Batched sub-path: at near-unity ratios the blend pair
                // (k, k+1) is stable for many consecutive outputs, so up to
                // MAX_DUAL_BATCH outputs share one dual-row kernel call with
                // per-output sample pointers and blend factors. The DDA runs
                // ahead of the call; a pair change rolls its consume back so
                // the next batch re-enters at the exact scalar-loop state,
                // and a wrapping pair (only checked at n == 0) emits alone
                // through the two-call path.
                constexpr size_t MAX_DUAL_BATCH = 4;
                const internal_sample_t* sample_ptrs[MAX_DUAL_BATCH];
                int32_t blend_factors[MAX_DUAL_BATCH];
                accumulator_t batch_acc[4 * MAX_DUAL_BATCH];
                const size_t acc_stride = 2 * num_channels;

                while (generated < output_capacity && consumed < input_frames) {
                    MR_PROF(uint32_t prof_t = profile::ccount();)

                    const size_t max_n = std::min(MAX_DUAL_BATCH, output_capacity - generated);
                    size_t n = 0;
                    uint32_t pair_phase = 0;
                    bool input_exhausted = false;
                    bool wrap_emit = false;
                    PhaseInfo phase_info{};

                    while (n < max_n) {
                        MR_PROF(++profile::g_cycles.iterations;)

                        // Consume the integer part of the accumulator (the
                        // generic loop's separate consume pass, fused in)
                        const uint32_t to_consume = acc >> RatioTracker::FRACTIONAL_BITS;
                        size_t consume_now = 0;
                        if (to_consume > 0) {
                            const size_t available = input_frames - consumed;
                            consume_now = std::min(static_cast<size_t>(to_consume), available);
                            consumed += consume_now;
                            acc -= static_cast<uint32_t>(consume_now)
                                   << RatioTracker::FRACTIONAL_BITS;
                            MR_PROF(++profile::g_cycles.consume_iters;)
                            if (consume_now < static_cast<size_t>(to_consume) ||
                                consumed == input_frames) {
                                input_exhausted = true;
                                break;
                            }
                        }

                        phase_info = RatioTracker::decode_asrc_phase(acc, num_phases);

                        if (phase_info.phase_index + 1U >= num_phases) {
                            if (n == 0) {
                                wrap_emit = true;
                            } else {
                                // Roll the consume back; the next batch
                                // re-enters here at n == 0
                                consumed -= consume_now;
                                acc += static_cast<uint32_t>(consume_now)
                                       << RatioTracker::FRACTIONAL_BITS;
                            }
                            break;
                        }
                        if (n == 0) {
                            pair_phase = phase_info.phase_index;
                        } else if (phase_info.phase_index != pair_phase) {
                            // Pair changed: roll back and start the next batch
                            consumed -= consume_now;
                            acc += static_cast<uint32_t>(consume_now)
                                   << RatioTracker::FRACTIONAL_BITS;
                            break;
                        }

                        sample_ptrs[n] = &history_base[consumed * num_channels];
                        blend_factors[n] = phase_info.blend_factor;
                        ++n;
                        acc += ratio;

                        if (slewing) {
                            slewed_ratio_sum += ratio;
                            ++slewed_outputs;
                            ratio = this->ratio_tracker_.step_slew_out_of_line();
                            slewing = this->ratio_tracker_.is_slewing();
                        }
                    }
                    MR_PROF(profile::mark(prof_t, profile::g_cycles.prep);)

                    if (n > 0) {
                        const size_t row_a =
                            (phase_to_row != nullptr) ? phase_to_row[pair_phase] : pair_phase;
                        const size_t row_b = (phase_to_row != nullptr)
                                                 ? phase_to_row[pair_phase + 1U]
                                                 : pair_phase + 1U;

                        MR_PROF(uint32_t prof_kernel = profile::ccount();)
#if defined(DOTPROD_PLATFORM_XTENSA_S3)
                        if constexpr (KERNEL_DUAL_Q15) {
                            dotprod::xtensa_s3::compute_block_shift_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &shift_base[row_a * row_shift_stride],
                                (entries_base != nullptr) ? &entries_base[row_a * row_tap_stride]
                                                          : nullptr,
                                &bank_base[row_b * row_length],
                                &shift_base[row_b * row_shift_stride],
                                (entries_base != nullptr) ? &entries_base[row_b * row_tap_stride]
                                                          : nullptr,
                                static_cast<int>(row_length), dual_tap_count, static_cast<int>(n),
                                static_cast<int>(num_channels), batch_acc);
                        }
                        if constexpr (KERNEL_DUAL_F32) {
                            dotprod::xtensa_s3::compute_fixed_float32_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &bank_base[row_b * row_length], static_cast<int>(row_length),
                                static_cast<int>(n), static_cast<int>(num_channels), batch_acc);
                        }
#elif defined(DOTPROD_PLATFORM_RISCV_P4)
                        if constexpr (KERNEL_DUAL_Q15) {
                            dotprod::riscv_p4::compute_block_shift_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &shift_base[row_a * row_shift_stride],
                                (entries_base != nullptr) ? &entries_base[row_a * row_tap_stride]
                                                          : nullptr,
                                &bank_base[row_b * row_length],
                                &shift_base[row_b * row_shift_stride],
                                (entries_base != nullptr) ? &entries_base[row_b * row_tap_stride]
                                                          : nullptr,
                                static_cast<int>(row_length), dual_tap_count, static_cast<int>(n),
                                static_cast<int>(num_channels), batch_acc);
                        }
                        if constexpr (KERNEL_DUAL_F32) {
                            dotprod::riscv_p4::compute_fixed_float32_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &bank_base[row_b * row_length], static_cast<int>(row_length),
                                static_cast<int>(n), static_cast<int>(num_channels), batch_acc);
                        }
                        if constexpr (KERNEL_DUAL_Q31LITE) {
                            dotprod::riscv_p4::compute_fixed_q31lite_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &bank_base[row_b * row_length], static_cast<int>(row_length),
                                static_cast<int>(n), static_cast<int>(num_channels), batch_acc);
                        }
#endif  // DOTPROD_PLATFORM_XTENSA_S3 / DOTPROD_PLATFORM_RISCV_P4
#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_XTENSA_MAC16)
                        if constexpr (KERNEL_DUAL_Q31LITE) {
                            dotprod::xtensa_q31lite::compute_fixed_dual(
                                sample_ptrs, &bank_base[row_a * row_length],
                                &bank_base[row_b * row_length], static_cast<int>(row_length),
                                static_cast<int>(n), static_cast<int>(num_channels), batch_acc);
                        }
#endif  // DOTPROD_PLATFORM_XTENSA_S3 || DOTPROD_PLATFORM_XTENSA_MAC16
                        MR_PROF(profile::mark(prof_kernel, profile::g_cycles.kernel);
                                ++profile::g_cycles.kernel_calls;)

                        // batch_acc holds [row A, row B] (mono) or [row A L,
                        // row A R, row B L, row B R] (stereo) per output;
                        // blend here (bit-exact: same per-row arithmetic,
                        // same AccumulatorBlender::blend, and blend(a, b, 0) == a)
                        uint8_t* out_base = static_cast<uint8_t*>(interleaved_output) +
                                            generated * output_frame_stride;
                        for (size_t i = 0; i < n; ++i) {
                            uint8_t* dst = out_base + i * output_frame_stride;
                            for (size_t ch = 0; ch < num_channels; ++ch) {
                                const accumulator_t acc_a = batch_acc[acc_stride * i + ch];
                                const accumulator_t acc_b =
                                    batch_acc[acc_stride * i + num_channels + ch];
                                const accumulator_t blended =
                                    (blend_factors[i] == 0)
                                        ? acc_a
                                        : AccumulatorBlender<CoeffFormat>::blend(
                                              acc_a, acc_b, blend_factors[i], BLEND_FACTOR_BITS);
                                write_output_sample<OutputSampleType>(
                                    dst + ch * OUTPUT_BYTES_PER_SAMPLE, blended,
                                    get_output_rng(static_cast<int>(ch)));
                            }
                        }
                        generated += n;
                        MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)
                    }

                    if (wrap_emit) {
                        // n == 0, so the outer loop condition still
                        // guarantees output capacity; the consume already ran
                        emit_two_call(phase_info, &history_base[consumed * num_channels],
                                      static_cast<uint8_t*>(interleaved_output) +
                                          generated * output_frame_stride);
                        acc += ratio;

                        if (slewing) {
                            slewed_ratio_sum += ratio;
                            ++slewed_outputs;
                            ratio = this->ratio_tracker_.step_slew_out_of_line();
                            slewing = this->ratio_tracker_.is_slewing();
                        }
                        ++generated;
                        MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)
                    }

                    if (input_exhausted) {
                        break;
                    }
                    MR_PROF(profile::mark(prof_t, profile::g_cycles.post);)
                }
            }
        }
#endif  // DOTPROD_PLATFORM_XTENSA_S3 || DOTPROD_PLATFORM_XTENSA_MAC16 ||
        // DOTPROD_PLATFORM_RISCV_P4

        while (!used_dual_loop && generated < output_capacity && consumed < input_frames) {
            MR_PROF(uint32_t prof_t = profile::ccount(); ++profile::g_cycles.iterations;)

            // Consume the integer part of the accumulator (the generic loop's
            // separate consume pass, fused in)
            uint32_t to_consume = acc >> RatioTracker::FRACTIONAL_BITS;
            if (to_consume > 0) {
                size_t available = input_frames - consumed;
                size_t consume_now = std::min(static_cast<size_t>(to_consume), available);
                consumed += consume_now;
                acc -= static_cast<uint32_t>(consume_now) << RatioTracker::FRACTIONAL_BITS;

                MR_PROF(profile::mark(prof_t, profile::g_cycles.consume);
                        ++profile::g_cycles.consume_iters;)

                // Stop when input ran out mid-consume or nothing is left to
                // read (mirrors the generic loop's per-pass entry condition)
                if (consume_now < static_cast<size_t>(to_consume) || consumed == input_frames) {
                    break;
                }
            }

            const PhaseInfo phase_info = RatioTracker::decode_asrc_phase(acc, num_phases);

            MR_PROF(profile::mark(prof_t, profile::g_cycles.phase_calc);)

            // check_phase_wrap_around() cannot break here: history-buffer mode
            // always holds filter_length + input_frames converted frames, and
            // the loop condition guarantees consumed + 1 <= input_frames, so
            // the wraparound row's one-frame advance is always covered

            const internal_sample_t* read_ptr = &history_base[consumed * num_channels];
            void* output_ptr =
                static_cast<uint8_t*>(interleaved_output) + generated * output_frame_stride;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.prep);)

            emit_two_call(phase_info, read_ptr, output_ptr);

            MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)

            acc += ratio;
            if (slewing) {
                slewed_ratio_sum += ratio;
                ++slewed_outputs;
                ratio = this->ratio_tracker_.step_slew_out_of_line();
                slewing = this->ratio_tracker_.is_slewing();
            }
            generated++;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.post);)
        }

        this->ratio_tracker_.set_asrc_accumulator(acc);
        const uint32_t steady_outputs = static_cast<uint32_t>(generated) - slewed_outputs;
        this->ratio_tracker_.advance_subsample_offset(slewed_ratio_sum + ratio * steady_outputs,
                                                      static_cast<uint32_t>(generated));
    } else {
        while (generated < output_capacity && consumed < input_frames) {
            MR_PROF(uint32_t prof_t = profile::ccount(); ++profile::g_cycles.iterations;)

            // Calculate phase info using the tracker
            PhaseInfo phase_info = this->ratio_tracker_.calculate_phase();

            MR_PROF(profile::mark(prof_t, profile::g_cycles.phase_calc);)

            // Check if we need to consume more input samples before generating output
            if (phase_info.samples_to_consume > 0) {
                size_t samples_available = input_frames - consumed;
                size_t samples_to_consume_now =
                    std::min(static_cast<size_t>(phase_info.samples_to_consume), samples_available);

                consumed += samples_to_consume_now;
                this->ratio_tracker_.update_after_consume(samples_to_consume_now);

                MR_PROF(profile::mark(prof_t, profile::g_cycles.consume);
                        ++profile::g_cycles.consume_iters;)

                if (samples_to_consume_now < static_cast<size_t>(phase_info.samples_to_consume)) {
                    break;
                }

                continue;
            }

            // Check phase wraparound
            if (this->check_phase_wrap_around(phase_info, consumed, input_frames)) {
                break;
            }

            // Get read pointer from history buffer
            const internal_sample_t* read_ptr = this->get_read_ptr(consumed);

            // Interpolate and generate output (direct call, no switch per frame)
            void* output_ptr =
                static_cast<uint8_t*>(interleaved_output) + generated * output_frame_stride;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.prep);)

            this->template interpolate_frame_impl<OutputSampleType>(
                dotprod_config, read_ptr, phase_info.phase_index, phase_info.blend_factor,
                output_ptr);

            MR_PROF(profile::mark(prof_t, profile::g_cycles.frame);)

            this->ratio_tracker_.update_after_output();
            generated++;

            MR_PROF(profile::mark(prof_t, profile::g_cycles.post);)
        }
    }

    MR_PROF(uint32_t prof_fixup = profile::ccount();)

    // STEP 3: Update history buffer (all input was converted up front in STEP 1)
    this->update_history_buffer(interleaved_input, consumed);

    // Finalize phase tracking
    this->ratio_tracker_.finalize(consumed);

    MR_PROF(profile::mark(prof_fixup, profile::g_cycles.block_fixup);
            profile::g_cycles.outputs += static_cast<uint32_t>(generated);
            profile::g_cycles.total += profile::ccount() - prof_block;)

    result.input_frames_consumed = consumed;
    result.output_frames_generated = generated;
    return result;
}

// Flush dispatch wrapper (same dither policy as process_block_impl: only Q15
// output under a wider processing format dithers, when dither_stage() == Output)
template <typename CoeffFormat>
typename MicroResamplerImpl<CoeffFormat>::ProcessResult MicroResamplerImpl<CoeffFormat>::flush_impl(
    void* interleaved_output, size_t output_capacity) {
    switch (this->output_format_) {
        case SampleFormat::Q15:
            if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
                return flush_inner<Q15SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                           output_capacity);
            } else {
                if (this->dither_stage() == DitherStage::Output) {
                    return flush_inner<Q15SampleConverter<CoeffFormat, true>>(interleaved_output,
                                                                              output_capacity);
                }
                return flush_inner<Q15SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                           output_capacity);
            }
        case SampleFormat::Q23:
            return flush_inner<Q23SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                       output_capacity);
        case SampleFormat::Q23_PACKED:
            return flush_inner<Q23PackedSampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                             output_capacity);
        case SampleFormat::Q31:
            return flush_inner<Q31SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                       output_capacity);
        case SampleFormat::FLOAT32:
            return flush_inner<Float32SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                           output_capacity);
        case SampleFormat::FLOAT64:
            return flush_inner<Float64SampleConverter<CoeffFormat, false>>(interleaved_output,
                                                                           output_capacity);
    }
    return {0, 0};
}

// Templated flush inner loop - output format resolved at compile time
template <typename CoeffFormat>
template <typename OutputSampleType>
typename MicroResamplerImpl<CoeffFormat>::ProcessResult
MicroResamplerImpl<CoeffFormat>::flush_inner(void* interleaved_output, size_t output_capacity) {
    ProcessResult result = {0, 0};

    if (output_capacity == 0 || this->flush_remaining_frames_ == 0) {
        return result;
    }

    // Input frames is the number of remaining flush frames
    size_t input_frames = this->flush_remaining_frames_;

    size_t consumed = 0;
    size_t generated = 0;

    // Compile-time output stride
    constexpr size_t OUTPUT_BYTES_PER_SAMPLE = []() {
        if constexpr (OutputSampleType::IS_PACKED) {
            return OutputSampleType::BYTES_PER_SAMPLE;
        } else {
            return sizeof(typename OutputSampleType::sample_t);
        }
    }();
    const size_t output_frame_stride = OUTPUT_BYTES_PER_SAMPLE * this->num_channels_;

    // Per-block dot product configuration (see process_block_inner)
    dotprod::DotProdConfig<CoeffFormat> dotprod_config{};
    this->init_dotprod_config(dotprod_config);

    // Process outputs - reads from history buffer (data placed by prepare_flush)
    while (generated < output_capacity && consumed < input_frames) {
        PhaseInfo phase_info = this->ratio_tracker_.calculate_phase();

        if (phase_info.samples_to_consume > 0) {
            size_t samples_available = input_frames - consumed;
            size_t samples_to_consume_now =
                std::min(static_cast<size_t>(phase_info.samples_to_consume), samples_available);

            consumed += samples_to_consume_now;
            this->ratio_tracker_.update_after_consume(samples_to_consume_now);

            if (samples_to_consume_now < static_cast<size_t>(phase_info.samples_to_consume)) {
                break;
            }

            continue;
        }

        // Check phase wraparound
        if (phase_info.blend_factor > 0) {
            uint16_t next_phase = phase_info.phase_index + 1;
            if (next_phase == this->num_phases_) {
                if (consumed + 1 > input_frames) {
                    break;
                }
            }
        }

        const internal_sample_t* read_ptr = &this->history_buffer_[consumed * this->num_channels_];

        void* output_ptr =
            static_cast<uint8_t*>(interleaved_output) + generated * output_frame_stride;
        this->template interpolate_frame_impl<OutputSampleType>(
            dotprod_config, read_ptr, phase_info.phase_index, phase_info.blend_factor, output_ptr);

        this->ratio_tracker_.update_after_output();
        generated++;
    }

    // Update history buffer - shift data forward after consuming. Keep the history window
    // plus every extrapolated frame not yet consumed, not just filter_length frames, or a
    // later flush call (after a partial one) would read stale extrapolation.
    if (consumed > 0) {
        size_t frames_to_keep = static_cast<size_t>(this->filter_length_) + input_frames - consumed;
        std::memmove(this->history_buffer_, &this->history_buffer_[consumed * this->num_channels_],
                     frames_to_keep * this->num_channels_ * sizeof(internal_sample_t));
    }

    this->ratio_tracker_.finalize(consumed);

    result.input_frames_consumed = consumed;
    result.output_frames_generated = generated;
    return result;
}

// Set maximum block size (pre-allocates conversion buffer when input conversion is needed)
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::set_max_block_size(size_t max_frames) {
    // Trigger initialization if needed - user wants to control allocation timing
    if (!this->initialized_) {
        if (!this->initialize()) {
            return;  // Initialization failed
        }
    }
    this->ensure_history_buffer_capacity(max_frames);
}

// Get the largest input block size the history buffer currently supports without reallocating
// (buffer headroom in frames: capacity minus filter length, or 0 if not yet initialized)
template <typename CoeffFormat>
size_t MicroResamplerImpl<CoeffFormat>::get_max_block_size() const {
    if (!this->initialized_) {
        return 0;  // Not initialized yet - no buffer allocated
    }
    return this->history_buffer_capacity_ > static_cast<size_t>(this->filter_length_)
               ? this->history_buffer_capacity_ - this->filter_length_
               : 0;
}

// History buffer frames for filter_length history plus one block of block_frames,
// or 0 if frames * channels * sizeof(sample) (plus alignment padding) overflows size_t
template <typename CoeffFormat>
size_t MicroResamplerImpl<CoeffFormat>::history_frames_for_block(size_t block_frames) const {
    constexpr size_t ALIGNMENT_PADDING = 16;  // allocate_aligned() rounds up to 16 bytes
    const size_t max_frames = (SIZE_MAX - ALIGNMENT_PADDING) / sizeof(internal_sample_t) /
                              static_cast<size_t>(this->num_channels_);
    const size_t filter_length = static_cast<size_t>(this->filter_length_);
    if (max_frames < filter_length || block_frames > max_frames - filter_length) {
        return 0;
    }
    return filter_length + block_frames;
}

// Ensure history buffer has room for one input block of block_frames
template <typename CoeffFormat>
bool MicroResamplerImpl<CoeffFormat>::ensure_history_buffer_capacity(size_t block_frames) {
    const size_t frames_needed = this->history_frames_for_block(block_frames);
    if (frames_needed == 0) {
        return false;  // Block too large to address
    }
    if (frames_needed <= this->history_buffer_capacity_) {
        return true;  // Already have enough
    }

    // Allocate new larger buffer
    size_t new_size = frames_needed * this->num_channels_;
    auto new_buffer = MemoryUtils::allocate_aligned<internal_sample_t>(
        new_size, 16, AllocStrategy::INTERNAL_ONLY);

    if (!new_buffer) {
        // Allocation failed - keep the existing (smaller) buffer intact so the
        // caller can refuse the oversized block instead of writing past the end
        return false;
    }

    // Copy existing history (first filter_length frames)
    if (this->history_buffer_) {
        std::memcpy(new_buffer, this->history_buffer_,
                    static_cast<size_t>(this->filter_length_) * this->num_channels_ *
                        sizeof(internal_sample_t));
        MemoryUtils::free(this->history_buffer_);
    }

    // Zero the rest
    std::memset(
        &new_buffer[this->filter_length_ * this->num_channels_], 0,
        (frames_needed - this->filter_length_) * this->num_channels_ * sizeof(internal_sample_t));

    this->history_buffer_ = new_buffer;
    this->history_buffer_capacity_ = frames_needed;
    return true;
}

// Dispatch to concrete converter type (switch once per batch, not per sample).
// Input dithering only applies when Q15 processing crushes a wider-than-16-bit
// source to 16 bits at the input conversion (dither_stage() == Input). Wider
// processing formats widen the source losslessly, so their input conversion is
// always verbatim; that dithered switch is compiled only for Q15Format.
template <typename CoeffFormat>
void MicroResamplerImpl<CoeffFormat>::convert_input(const void* src, internal_sample_t* dst,
                                                    size_t frame_count) {
    const size_t sample_count = frame_count * this->num_channels_;
    const uint8_t* src_bytes = static_cast<const uint8_t*>(src);

    if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
        if (this->dither_stage() == DitherStage::Input) {
            switch (this->input_format_) {
                case SampleFormat::Q23:
                    convert_samples<Q23SampleConverter<CoeffFormat, true>>(src_bytes, dst,
                                                                           sample_count);
                    return;
                case SampleFormat::Q23_PACKED:
                    convert_samples<Q23PackedSampleConverter<CoeffFormat, true>>(src_bytes, dst,
                                                                                 sample_count);
                    return;
                case SampleFormat::Q31:
                    convert_samples<Q31SampleConverter<CoeffFormat, true>>(src_bytes, dst,
                                                                           sample_count);
                    return;
                case SampleFormat::FLOAT32:
                    convert_samples<Float32SampleConverter<CoeffFormat, true>>(src_bytes, dst,
                                                                               sample_count);
                    return;
                case SampleFormat::FLOAT64:
                    convert_samples<Float64SampleConverter<CoeffFormat, true>>(src_bytes, dst,
                                                                               sample_count);
                    return;
                case SampleFormat::Q15:
                    break;  // 16-bit source: no reduction, handled verbatim below
            }
        }
    }

    // Verbatim (non-dithered) input conversion: Q15 source, wider processing
    // formats (lossless widen), or dithering disabled/ineffective.
    switch (this->input_format_) {
        case SampleFormat::Q15:
            convert_samples<Q15SampleConverter<CoeffFormat, false>>(src_bytes, dst, sample_count);
            break;
        case SampleFormat::Q23:
            convert_samples<Q23SampleConverter<CoeffFormat, false>>(src_bytes, dst, sample_count);
            break;
        case SampleFormat::Q23_PACKED:
            convert_samples<Q23PackedSampleConverter<CoeffFormat, false>>(src_bytes, dst,
                                                                          sample_count);
            break;
        case SampleFormat::Q31:
            convert_samples<Q31SampleConverter<CoeffFormat, false>>(src_bytes, dst, sample_count);
            break;
        case SampleFormat::FLOAT32:
            convert_samples<Float32SampleConverter<CoeffFormat, false>>(src_bytes, dst,
                                                                        sample_count);
            break;
        case SampleFormat::FLOAT64:
            convert_samples<Float64SampleConverter<CoeffFormat, false>>(src_bytes, dst,
                                                                        sample_count);
            break;
    }
}

// Templated input conversion loop - concrete SampleType enables inlining
template <typename CoeffFormat>
template <typename InputSampleType>
void MicroResamplerImpl<CoeffFormat>::convert_samples(const uint8_t* src, internal_sample_t* dst,
                                                      size_t sample_count) {
    constexpr size_t INPUT_BYTES_PER_SAMPLE = []() {
        if constexpr (InputSampleType::IS_PACKED) {
            return InputSampleType::BYTES_PER_SAMPLE;
        } else {
            return sizeof(typename InputSampleType::sample_t);
        }
    }();

    for (size_t i = 0; i < sample_count; ++i) {
        // IS_DITHERED is constexpr: dead branch eliminated at compile time (no per-sample empty()
        // check)
        uint32_t* rng = nullptr;
        if constexpr (InputSampleType::IS_DITHERED) {
            rng = this->input_rng_ptrs_[i % this->num_channels_];
        }
        if constexpr (InputSampleType::IS_PACKED) {
            dst[i] = InputSampleType::to_processing_packed(src, rng);
        } else {
            dst[i] = InputSampleType::to_processing(
                *reinterpret_cast<const typename InputSampleType::sample_t*>(src), rng);
        }
        src += INPUT_BYTES_PER_SAMPLE;
    }
}

// ============================================================================
// Explicit template instantiations
// ============================================================================
// With type-erased input/output formats, we only need 5 instantiations
// (one per CoeffFormat) instead of 75+ combinations.
// ============================================================================

template class MicroResamplerImpl<Q15Format>;
template class MicroResamplerImpl<Q31Format>;
template class MicroResamplerImpl<Q31LiteFormat>;
template class MicroResamplerImpl<Float32Format>;
template class MicroResamplerImpl<Float64Format>;

// ============================================================================
// Factory
// ============================================================================
// Picks the MicroResamplerImpl instantiation for the configured coefficient
// format. Uses nothrow new so an allocation failure is reported as nullptr
// on targets built without exceptions, matching initialize()'s contract.

static_assert(get_bytes_per_coefficient(CoefficientFormat::Q15) ==
              sizeof(Q15Format::coefficient_t));
static_assert(get_bytes_per_coefficient(CoefficientFormat::Q31) ==
              sizeof(Q31Format::coefficient_t));
static_assert(get_bytes_per_coefficient(CoefficientFormat::Q31_LITE) ==
              sizeof(Q31LiteFormat::coefficient_t));
static_assert(get_bytes_per_coefficient(CoefficientFormat::FLOAT32) ==
              sizeof(Float32Format::coefficient_t));
static_assert(get_bytes_per_coefficient(CoefficientFormat::FLOAT64) ==
              sizeof(Float64Format::coefficient_t));

template <typename CoeffFormat>
static std::unique_ptr<MicroResampler> create_impl(uint32_t input_rate, uint32_t output_rate,
                                                   int channels, const ResamplerConfig& config) {
    return std::unique_ptr<MicroResampler>(new (std::nothrow) MicroResamplerImpl<CoeffFormat>(
        input_rate, output_rate, channels, config));
}

std::unique_ptr<MicroResampler> MicroResampler::create(uint32_t input_rate, uint32_t output_rate,
                                                       int channels, ResamplerQuality quality) {
    return MicroResampler::create(input_rate, output_rate, channels,
                                  ResamplerConfig().set_quality(quality));
}

std::unique_ptr<MicroResampler> MicroResampler::create(uint32_t input_rate, uint32_t output_rate,
                                                       int channels,
                                                       const ResamplerConfig& config) {
    if (input_rate == 0 || output_rate == 0 || channels <= 0) {
        return nullptr;
    }

    // An explicit FilterDesign bypasses resolve_design(), which always yields a multiple of 8.
    // The SIMD dot-product kernels process whole 8-tap blocks and loop until they reach the end
    // of the row, so any other length would read past it
    if (config.has_filter_design()) {
        const int length = config.get_filter_design().get_filter_length();
        if (length <= 0 || length % 8 != 0 || length > MAX_FILTER_LENGTH) {
            return nullptr;
        }
    }

    switch (config.get_coefficient_format()) {
        case CoefficientFormat::Q15:
            return create_impl<Q15Format>(input_rate, output_rate, channels, config);
        case CoefficientFormat::Q31:
            return create_impl<Q31Format>(input_rate, output_rate, channels, config);
        case CoefficientFormat::Q31_LITE:
            return create_impl<Q31LiteFormat>(input_rate, output_rate, channels, config);
        case CoefficientFormat::FLOAT32:
            return create_impl<Float32Format>(input_rate, output_rate, channels, config);
        case CoefficientFormat::FLOAT64:
            return create_impl<Float64Format>(input_rate, output_rate, channels, config);
    }
    return nullptr;
}

}  // namespace micro_resampler
