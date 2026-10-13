// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

///////////////////////////////////////////////////////////////////////////////
/// @file ratio_tracker.h
/// @brief Phase tracking implementation for MicroResampler
///
/// This file contains the UnifiedRatioTracker class that encapsulates all phase
/// calculation and tracking logic for the resampler. The tracker starts in
/// fixed-ratio mode (for optimal performance) and can permanently transition
/// to ASRC mode when dynamic ratio adjustment is needed.
///
/// Key features:
/// - Starts in FIXED mode with integer arithmetic (aligned path when possible)
/// - One-way transition to ASRC mode when adjust_ppm() or set_ratio() is called
/// - PPM adjustments are relative to the BASE ratio, not unity
/// - Preserves aligned optimization until first ratio adjustment
///
/// This class is an internal implementation detail and should not be used
/// directly. Use MicroResampler instead.
///
/// @note Thread Safety: This class is NOT thread-safe. It maintains mutable
///       state and must be protected by external synchronization if accessed
///       from multiple threads.
///////////////////////////////////////////////////////////////////////////////

#ifndef RATIO_TRACKER_H
#define RATIO_TRACKER_H

#include "coefficient_ops.h"
#include "fixed_generic.h"
#include "formats.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

// Keeps a cold helper out of a hot caller (see step_slew_out_of_line())
#ifndef MICRO_RESAMPLER_NOINLINE
#if defined(__GNUC__) || defined(__clang__)
#define MICRO_RESAMPLER_NOINLINE __attribute__((noinline))
#else
#define MICRO_RESAMPLER_NOINLINE
#endif
#endif

// ============================================================================
// PhaseInfo Structure
// ============================================================================
// Contains phase calculation results used by the resampler's interpolation loop.
// Note: This is defined in include/resampler/resampler.h as a nested struct
// within MicroResampler. This standalone definition is provided for RatioTracker
// classes to use independently.

struct PhaseInfo {
    uint16_t phase_index;         // Which filter phase to use (0 to num_phases-1)
    int32_t blend_factor;         // Fixed-point blend amount with next phase
    uint32_t samples_to_consume;  // How many input samples to consume
};

namespace micro_resampler {
namespace detail {

// ============================================================================
// UnifiedRatioTracker
// ============================================================================
// Unified phase tracking that starts in fixed mode (for optimal performance)
// and can permanently transition to ASRC mode when dynamic ratio adjustment
// is needed. This allows all resamplers to support PPM adjustment on top of
// any base conversion ratio.
//
// Key features:
// - Starts in FIXED mode with integer arithmetic (aligned path when possible)
// - One-way transition to ASRC mode when adjust_ppm() or set_ratio() is called
// - PPM adjustments are relative to the BASE ratio, not unity
// - Preserves aligned optimization until first ratio adjustment

template <typename CoeffFormat>
class UnifiedRatioTracker {
public:
    // ========================================
    // Mode Enumeration
    // ========================================

    enum class Mode : uint8_t { FIXED, ASRC };

    // ========================================
    // Fixed-Point Constants (4.28 format for ASRC mode)
    // ========================================
    //
    // The 4.28 format uses a 32-bit unsigned integer:
    //   - 4 integer bits: represents values 0-15 (max ratio ~15.999)
    //   - 28 fractional bits: provides ~0.004 PPM precision
    //
    // LIMITATION: ASRC mode only supports conversion ratios < 16:1.
    // For ratios >= 16:1 (e.g., 192kHz → 8kHz = 24:1), the ratio cannot be
    // represented in 4.28 format. In these cases, ASRC functions (adjust_ppm,
    // set_ratio, etc.) will silently fail and the resampler continues using
    // the fixed integer ratio, which provides perfect accuracy for the base
    // conversion but cannot compensate for clock drift.
    //
    // Common ratios and their ASRC support:
    //   - 48kHz → 44.1kHz (1.09:1) ✓
    //   - 96kHz → 44.1kHz (2.18:1) ✓
    //   - 192kHz → 44.1kHz (4.35:1) ✓
    //   - 96kHz → 8kHz (12:1) ✓
    //   - 192kHz → 16kHz (12:1) ✓
    //   - 192kHz → 8kHz (24:1) ✗ (exceeds 16:1 limit)
    //   - 176.4kHz → 8kHz (22:1) ✗ (exceeds 16:1 limit)

    static constexpr int FRACTIONAL_BITS = 28;
    static constexpr uint32_t UNITY_RATIO = 1U << FRACTIONAL_BITS;            // 1.0 in 4.28 format
    static constexpr uint32_t FRACTIONAL_MASK = (1U << FRACTIONAL_BITS) - 1;  // 0x0FFFFFFF
    static constexpr uint32_t MAX_ASRC_RATIO = 16;  // Maximum ratio supported by 4.28 format

    // ========================================
    // Initialization (always starts in FIXED mode)
    // ========================================

    // Initialize the tracker with rate information
    // input_rate: Input sample rate (e.g., 44100)
    // output_rate: Output sample rate (e.g., 48000)
    // filter_length: Length of the FIR filter
    // num_phases: Number of polyphase filter phases
    void init(uint32_t input_rate, uint32_t output_rate, int filter_length, uint32_t num_phases) {
        // Start in fixed mode
        this->mode_ = Mode::FIXED;

        // Store original input rate for ASRC mode transition (get_nominal_rate())
        this->original_input_rate_ = input_rate;

        // Calculate GCD for rate simplification
        uint32_t g = gcd(input_rate, output_rate);
        this->simplified_input_rate_ = input_rate / g;
        this->simplified_output_rate_ = output_rate / g;
        this->num_phases_ = num_phases;
        this->filter_length_ = filter_length;

        // Determine if we can use the aligned (zero-blend) path
        // This optimization is valid when phases align exactly with output rate
        this->is_aligned_ = (this->num_phases_ == this->simplified_output_rate_);

        // Initialize ASRC state to safe defaults (will be properly set in enable_asrc_mode)
        this->phase_accumulator_ = 0;
        this->base_ratio_4_28_ = 0;
        this->current_ratio_ = 0;
        this->target_ratio_ = 0;
        this->clear_slew();
        this->cumulative_subsample_offset_ = 0;
        this->initial_skip_count_ = 0;

        // Initialize phase state for proper latency compensation
        this->reset();
    }

    // Reset phase tracking state for proper latency compensation
    // The filter has a group delay of (N-1)/2 samples, so we want the resampler
    // to consume approximately N/2+1 input samples before producing output.
    // NOTE: Preserves the current mode
    void reset() {
        if (this->mode_ == Mode::FIXED) {
            // Initialize phase tracking for latency compensation.
            //
            // For the aligned path (num_phases == simplified_output_rate_):
            //   phase_aligned_accumulator_ = (N/2 + 1) * P_s
            //   This causes samples_to_consume = accumulator / P_s = N/2 + 1
            //   After consuming, accumulator = 0, so phase_index = 0, blend_factor = 0
            //
            // For the non-aligned path (num_phases < simplified_output_rate_):
            //   The calculation uses: difference = phase_output_frames_ * Q_s - phase_input_frames_
            //   * P_s We need difference = (N/2+1) * P_s EXACTLY so that:
            //     samples_to_consume = (N/2+1) * P_s / P_s = N/2+1
            //     fraction_numerator = (N/2+1) * P_s % P_s = 0
            //   This yields phase_index = 0 and blend_factor = 0 after startup consumption.
            //
            //   To achieve this with integers, we solve: A * Q_s - B * P_s = L * P_s
            //   where L = N/2+1, A = phase_output_frames_, B = phase_input_frames_
            //   Rearranging: A * Q_s = (L + B) * P_s
            //   For A to be an integer: (L + B) must be divisible by Q_s (when GCD(P_s,Q_s)=1)
            //   So B = (Q_s - (L % Q_s)) % Q_s, then A = (L + B) * P_s / Q_s
            uint32_t l = this->filter_length_ / 2 + 1;
            uint32_t remainder = l % this->simplified_input_rate_;
            this->phase_input_frames_ =
                (remainder == 0) ? 0 : (this->simplified_input_rate_ - remainder);
            this->phase_output_frames_ = (l + this->phase_input_frames_) *
                                         this->simplified_output_rate_ /
                                         this->simplified_input_rate_;

            this->phase_aligned_accumulator_ =
                (this->filter_length_ / 2 + 1) * this->simplified_output_rate_;
        } else {
            // ASRC mode: reset phase accumulator and subsample offset, keep ratios
            this->phase_accumulator_ = 0;
            this->initial_skip_count_ = this->filter_length_ / 2 + 1;
            this->cumulative_subsample_offset_ = 0;

            // Reset current ratio to base ratio (preserving base_ratio_4_28_)
            this->current_ratio_ = this->base_ratio_4_28_;
            this->target_ratio_ = this->base_ratio_4_28_;
            this->clear_slew();
        }
    }

    // ========================================
    // Mode Transition (one-way: FIXED -> ASRC)
    // ========================================

    // Permanently switch from FIXED mode to ASRC mode
    // This syncs the phase position and prepares for dynamic ratio adjustment
    //
    // Returns true if ASRC mode was enabled, false if the ratio exceeds the
    // 4.28 format limit (>= 16:1) and the resampler remains in fixed mode.
    bool enable_asrc_mode() {
        if (this->mode_ == Mode::ASRC) {
            return true;
        }

        // Check if ratio exceeds 4.28 format limit (>= 16:1)
        // The 4.28 format can only represent ratios up to ~15.999
        if (this->simplified_input_rate_ >= this->simplified_output_rate_ * MAX_ASRC_RATIO) {
            // Ratio too large for ASRC mode - remain in fixed mode
            // Fixed mode still provides perfect accuracy for the base conversion,
            // but PPM adjustments will not be applied
            return false;
        }

        // Convert integer ratio to 4.28 fixed-point
        // e.g., 48000/44100 -> 292,567,980
        this->base_ratio_4_28_ = (uint64_t(this->simplified_input_rate_) << FRACTIONAL_BITS) /
                                 this->simplified_output_rate_;
        this->current_ratio_ = this->base_ratio_4_28_;
        this->target_ratio_ = this->base_ratio_4_28_;

        // Sync phase position from EITHER fixed-mode representation to 4.28. Both encode
        // pending = samples_to_consume * simplified_output_rate_ + fraction; the fraction
        // seeds the 4.28 accumulator and the whole samples still owed carry over as an
        // initial skip. Before the first block that is the full N/2+1 startup latency
        // compensation (dropping it would shift output N/2+1 samples late); mid-stream it is
        // whatever the last block left unconsumed when it ran out of input.
        uint32_t pending;
        if (this->is_aligned_) {
            pending = this->phase_aligned_accumulator_;
        } else {
            uint64_t scaled_out =
                static_cast<uint64_t>(this->phase_output_frames_) * this->simplified_input_rate_;
            uint64_t scaled_in =
                static_cast<uint64_t>(this->phase_input_frames_) * this->simplified_output_rate_;
            pending = static_cast<uint32_t>(scaled_out - scaled_in);
        }
        uint32_t frac = pending % this->simplified_output_rate_;
        this->phase_accumulator_ =
            (static_cast<uint64_t>(frac) << FRACTIONAL_BITS) / this->simplified_output_rate_;
        this->initial_skip_count_ = pending / this->simplified_output_rate_;

        // Initialize slewing state
        this->clear_slew();
        this->cumulative_subsample_offset_ = 0;

        this->mode_ = Mode::ASRC;
        return true;
    }

    // ========================================
    // Phase Calculation
    // ========================================

    // Calculate the current phase information
    // Returns phase_index, blend_factor, and samples_to_consume
    // Dispatches to aligned/general/asrc based on mode
    PhaseInfo calculate_phase() {
        // [[likely]] needs C++20; the library builds at C++17, so this is a plain branch.
        if (this->mode_ == Mode::FIXED) {
            return this->is_aligned_ ? this->calculate_phase_aligned()
                                     : this->calculate_phase_general();
        }
        return this->calculate_phase_asrc();
    }

    // ========================================
    // Phase Update Methods
    // ========================================

    // Update state after consuming input samples
    void update_after_consume(size_t samples) {
        if (this->mode_ == Mode::FIXED) {
            if (this->is_aligned_) {
                this->phase_aligned_accumulator_ -= samples * this->simplified_output_rate_;
            } else {
                this->phase_input_frames_ += samples;
            }
        } else {
            // ASRC mode: subtract consumed samples from accumulator (shift by 28 to work with
            // integer part)
            this->phase_accumulator_ -= (static_cast<uint32_t>(samples) << FRACTIONAL_BITS);
        }
    }

    // Update state after generating an output sample
    void update_after_output() {
        if (this->mode_ == Mode::FIXED) {
            if (this->is_aligned_) {
                this->phase_aligned_accumulator_ += this->simplified_input_rate_;
            } else {
                this->phase_output_frames_++;
            }
        } else {
            // ASRC mode: advance phase accumulator by current ratio (32-bit add)
            this->phase_accumulator_ += this->current_ratio_;

            // Track cumulative drift from base ratio for subsample offset reporting. Add through
            // uint32_t so a block that drifts past the +/-8 samples an int32 4.28 value holds
            // wraps (defined) instead of overflowing (UB); see cumulative_subsample_offset_.
            this->cumulative_subsample_offset_ =
                static_cast<int32_t>(static_cast<uint32_t>(this->cumulative_subsample_offset_) +
                                     this->current_ratio_ - this->base_ratio_4_28_);

            // The next output advances by the next ratio of an active slew
            this->step_slew();
        }
    }

    // Advance an active slew by one output frame. The ramp steps by the truncated quotient
    // diff / slew_samples every frame and spreads the remainder over the slew one LSB at a
    // time (Bresenham), so it lands exactly on the target after slew_samples frames with no
    // final jump. Driven from update_after_output() and the ASRC fast loop: once per output,
    // never per consume pass, so the duration does not depend on the conversion ratio.
    void step_slew() {
        if (this->slew_samples_remaining_ == 0) {
            return;
        }
        if (--this->slew_samples_remaining_ == 0) {
            this->current_ratio_ = this->target_ratio_;
            return;
        }
        int32_t step = this->ratio_delta_;
        // error + remainder >= length, rearranged so it cannot overflow uint32_t
        if (this->slew_error_ >= this->slew_length_ - this->slew_remainder_) {
            this->slew_error_ -= this->slew_length_ - this->slew_remainder_;
            step += this->slew_carry_;
        } else {
            this->slew_error_ += this->slew_remainder_;
        }
        // Wrapping add through uint32_t: the ramp stays between two valid 4.28 ratios
        this->current_ratio_ += static_cast<uint32_t>(step);
    }

    // Finalize state at end of processing block
    void finalize(size_t consumed) {
        if (this->mode_ == Mode::FIXED) {
            if (this->is_aligned_) {
                // Aligned path needs to sync phase_input_frames_ for compatibility
                this->phase_input_frames_ += consumed;
            }
            // General path: no-op - phase_input_frames_ already updated in update_after_consume
        } else {
            // ASRC mode: wrap subsample offset to stay in +/-1 sample range (% truncates
            // toward zero, keeping the sign of the accumulated drift)
            this->cumulative_subsample_offset_ %= static_cast<int32_t>(UNITY_RATIO);
        }
    }

    // ========================================
    // Initial Skip Handling (ASRC mode only)
    // ========================================

    // Get the number of initial samples to skip (for latency compensation)
    uint32_t get_initial_skip_count() const {
        return this->initial_skip_count_;
    }

    // Consume initial skip samples (returns how many were actually skipped)
    size_t consume_initial_skip(size_t available_samples) {
        if (this->initial_skip_count_ == 0) {
            return 0;
        }

        size_t samples_to_skip = (available_samples < this->initial_skip_count_)
                                     ? available_samples
                                     : this->initial_skip_count_;
        this->initial_skip_count_ -= samples_to_skip;
        return samples_to_skip;
    }

    // ========================================
    // Rate Control API (auto-enables ASRC mode when called)
    // ========================================

    // Adjust ratio by PPM offset from BASE ratio (not unity!)
    // Positive PPM = larger ratio = consume more input per output = fewer output frames
    // This auto-enables ASRC mode
    void adjust_ppm(double ppm_offset, uint32_t slew_samples = 0) {
        this->enable_asrc_mode();

        // PPM adjustment relative to BASE ratio, not unity
        // new_ratio = base_ratio * (1 + ppm * 1e-6)
        constexpr int32_t PPM_DIVISOR = 1000000;  // 1 PPM = 1 part per 1,000,000
        int64_t adjustment =
            static_cast<int64_t>(this->base_ratio_4_28_) * ppm_offset / PPM_DIVISOR;
        uint32_t new_ratio = this->base_ratio_4_28_ + static_cast<int32_t>(adjustment);

        if (slew_samples == 0) {
            this->current_ratio_ = new_ratio;
            this->target_ratio_ = new_ratio;
            this->clear_slew();
        } else {
            this->set_target_ratio_fixed(new_ratio, slew_samples);
        }
    }

    // Set ratio directly (1.0 = unity, <1.0 = speed up output, >1.0 = slow down). The read
    // position stays continuous, so an instant change only steps the pitch
    // This auto-enables ASRC mode
    void set_ratio(double ratio) {
        this->enable_asrc_mode();
        this->set_ratio_fixed(static_cast<uint32_t>(std::llround(ratio * UNITY_RATIO)));
    }

    void set_ratio_fixed(uint32_t ratio_4_28) {
        this->enable_asrc_mode();
        this->current_ratio_ = ratio_4_28;
        this->target_ratio_ = ratio_4_28;
        this->clear_slew();
    }

    // Smoothly transition to new ratio over slew_samples
    // This auto-enables ASRC mode
    void set_target_ratio(double ratio, uint32_t slew_samples) {
        this->enable_asrc_mode();
        this->set_target_ratio_fixed(static_cast<uint32_t>(std::llround(ratio * UNITY_RATIO)),
                                     slew_samples);
    }

    void set_target_ratio_fixed(uint32_t ratio_4_28, uint32_t slew_samples) {
        this->enable_asrc_mode();
        this->target_ratio_ = ratio_4_28;

        if (slew_samples == 0 || this->current_ratio_ == this->target_ratio_) {
            this->current_ratio_ = this->target_ratio_;
            this->clear_slew();
        } else {
            // diff = q * slew_samples + r with |r| < slew_samples; step_slew() adds q every frame
            // and r's sign on |r| evenly spaced frames, so the slew_samples steps sum to diff.
            // The last step always lands on target_ratio_ directly, so a one-frame slew needs no
            // step (its q, the whole diff, may not fit int32_t); for two or more frames
            // |q| <= |diff| / 2 fits
            const int64_t diff = static_cast<int64_t>(this->target_ratio_) -
                                 static_cast<int64_t>(this->current_ratio_);
            const int64_t n = slew_samples;
            this->ratio_delta_ = (n == 1) ? 0 : static_cast<int32_t>(diff / n);
            const int64_t remainder = diff % n;
            this->slew_remainder_ = static_cast<uint32_t>(remainder < 0 ? -remainder : remainder);
            this->slew_carry_ = remainder < 0 ? -1 : 1;
            this->slew_length_ = slew_samples;
            this->slew_error_ = 0;
            this->slew_samples_remaining_ = slew_samples;
        }
    }

    // ========================================
    // Aligned Fast-Path Accessors
    // ========================================
    // The aligned fixed-mode hot loop in process_block_inner keeps the phase
    // accumulator in a register and advances it with an incremental DDA update
    // (add + conditional subtract) instead of calling calculate_phase() with
    // its div/mod per pass. These accessors let that loop load the state once
    // per block and store it back afterwards. The accumulator encodes
    // samples_to_consume * simplified_output_rate + phase_index.

    // True when the incremental aligned path may be used
    bool is_fixed_aligned() const {
        return this->mode_ == Mode::FIXED && this->is_aligned_;
    }

    uint32_t get_aligned_accumulator() const {
        return static_cast<uint32_t>(this->phase_aligned_accumulator_);
    }

    void set_aligned_accumulator(uint32_t accumulator) {
        this->phase_aligned_accumulator_ = static_cast<int32_t>(accumulator);
    }

    // ========================================
    // ASRC Fast-Path Accessors
    // ========================================
    // The ASRC hot loop keeps the 4.28 phase accumulator and the ratio in
    // registers and advances them per output instead of calling
    // calculate_phase()/update_after_*() through the tracker per pass. These
    // accessors let that loop load the state once per block and store it back
    // afterwards; while a slew is active the loop steps it with step_slew().

    // Slew step for the ASRC hot loop, kept out of line so the cold slewing path
    // adds no code or register pressure to the loop body (measured on the P4: an
    // inlined step costs the steady-ratio loop about 1%). Returns the ratio for
    // the next output.
    MICRO_RESAMPLER_NOINLINE uint32_t step_slew_out_of_line() {
        this->step_slew();
        return this->current_ratio_;
    }

    uint32_t get_asrc_accumulator() const {
        return this->phase_accumulator_;
    }

    void set_asrc_accumulator(uint32_t accumulator) {
        this->phase_accumulator_ = accumulator;
    }

    // Decode a caller-held 4.28 accumulator into phase info: the exact
    // arithmetic of calculate_phase_asrc(), shared so the fast path stays
    // bit-identical to the generic loop
    static PhaseInfo decode_asrc_phase(uint32_t accumulator, uint32_t num_phases) {
        return decode_phase_4_28(accumulator, num_phases);
    }

    // Apply `outputs` per-output subsample-offset advances at once, given the
    // wrapping sum of the ratios those outputs advanced by. Unsigned arithmetic
    // wraps mod 2^32, matching the repeated wrapping uint32_t adds of
    // update_after_output() whatever the ratios (a slew changes them per
    // output); the per-block % in finalize() then runs on the same value.
    void advance_subsample_offset(uint32_t ratio_sum, uint32_t outputs) {
        this->cumulative_subsample_offset_ =
            static_cast<int32_t>(static_cast<uint32_t>(this->cumulative_subsample_offset_) +
                                 ratio_sum - this->base_ratio_4_28_ * outputs);
    }

    // ========================================
    // Query Methods
    // ========================================

    // Mode query
    bool is_fixed_mode() const {
        return this->mode_ == Mode::FIXED;
    }

    bool is_asrc_mode() const {
        return this->mode_ == Mode::ASRC;
    }

    // Check if ASRC mode is supported for this conversion ratio
    // Returns false if the ratio >= 16:1 (exceeds 4.28 format limit)
    bool is_asrc_supported() const {
        return this->simplified_input_rate_ < this->simplified_output_rate_ * MAX_ASRC_RATIO;
    }

    // Get the base conversion ratio (input_rate / output_rate)
    double get_base_ratio() const {
        return static_cast<double>(this->simplified_input_rate_) /
               static_cast<double>(this->simplified_output_rate_);
    }

    // Rate query
    uint32_t get_input_rate() const {
        return this->simplified_input_rate_;
    }

    uint32_t get_output_rate() const {
        return this->simplified_output_rate_;
    }

    uint32_t get_num_phases() const {
        return this->num_phases_;
    }

    // ASRC-specific query methods
    double get_current_ratio() const {
        if (this->mode_ == Mode::FIXED) {
            return this->get_base_ratio();
        }
        return static_cast<double>(this->current_ratio_) / static_cast<double>(UNITY_RATIO);
    }

    uint32_t get_current_ratio_fixed() const {
        if (this->mode_ == Mode::FIXED) {
            return this->base_ratio_4_28_ != 0
                       ? this->base_ratio_4_28_
                       : static_cast<uint32_t>(
                             (uint64_t(this->simplified_input_rate_) << FRACTIONAL_BITS) /
                             this->simplified_output_rate_);
        }
        return this->current_ratio_;
    }

    double get_target_ratio() const {
        if (this->mode_ == Mode::FIXED) {
            return this->get_base_ratio();
        }
        return static_cast<double>(this->target_ratio_) / static_cast<double>(UNITY_RATIO);
    }

    uint32_t get_target_ratio_fixed() const {
        if (this->mode_ == Mode::FIXED) {
            return this->base_ratio_4_28_ != 0
                       ? this->base_ratio_4_28_
                       : static_cast<uint32_t>(
                             (uint64_t(this->simplified_input_rate_) << FRACTIONAL_BITS) /
                             this->simplified_output_rate_);
        }
        return this->target_ratio_;
    }

    bool is_slewing() const {
        return this->mode_ == Mode::ASRC && this->slew_samples_remaining_ > 0;
    }

    // Get the current fractional input position (for debugging/monitoring)
    double get_phase_position() const {
        if (this->mode_ == Mode::FIXED) {
            if (this->is_aligned_) {
                return static_cast<double>(this->phase_aligned_accumulator_ %
                                           this->simplified_output_rate_) /
                       static_cast<double>(this->simplified_output_rate_);
            }
            uint64_t scaled_out =
                static_cast<uint64_t>(this->phase_output_frames_) * this->simplified_input_rate_;
            uint64_t scaled_in =
                static_cast<uint64_t>(this->phase_input_frames_) * this->simplified_output_rate_;
            uint32_t diff = static_cast<uint32_t>(scaled_out - scaled_in);
            uint32_t frac = diff % this->simplified_output_rate_;
            return static_cast<double>(frac) / static_cast<double>(this->simplified_output_rate_);
        }
        return static_cast<double>(this->phase_accumulator_) / static_cast<double>(UNITY_RATIO);
    }

    // Get subsample offset from base ratio (in samples, range approximately -1 to +1)
    double get_subsample_offset() const {
        if (this->mode_ == Mode::FIXED) {
            return 0.0;  // No drift in fixed mode
        }
        return static_cast<double>(this->cumulative_subsample_offset_) /
               static_cast<double>(UNITY_RATIO);
    }

    // Get nominal/base rate (for ASRC compatibility)
    uint32_t get_nominal_rate() const {
        return this->original_input_rate_;  // Return original input rate as nominal
    }

private:
    // ========================================
    // Internal Phase Calculation Methods
    // ========================================

    // Aligned path: phases align exactly with output rate (no interpolation needed)
    PhaseInfo calculate_phase_aligned() {
        PhaseInfo info{};

        info.samples_to_consume = this->phase_aligned_accumulator_ / this->simplified_output_rate_;
        info.phase_index = this->phase_aligned_accumulator_ % this->simplified_output_rate_;
        info.blend_factor = 0;

        return info;
    }

    // General path: phases don't align with output rate (interpolation required)
    PhaseInfo calculate_phase_general() {
        PhaseInfo info{};

        // position = (total_output * step_size) - total_input
        const uint32_t scaled_output_frames =
            this->phase_output_frames_ * this->simplified_input_rate_;
        const uint32_t scaled_input_frames =
            this->phase_input_frames_ * this->simplified_output_rate_;
        const uint32_t difference = scaled_output_frames - scaled_input_frames;

        // Reset counters to prevent overflow
        if (difference == 0) {
            this->phase_input_frames_ = 0;
            this->phase_output_frames_ = 0;
        }

        // Calculate integer part (samples to consume)
        info.samples_to_consume = difference / this->simplified_output_rate_;

        // Calculate fractional part for phase selection
        const uint32_t fraction_numerator = difference % this->simplified_output_rate_;

        // Calculate phase index and blend factor
        // Note: When num_phases_ == simplified_output_rate_, callers use the aligned path instead
        info.phase_index = (fraction_numerator * this->num_phases_) / this->simplified_output_rate_;

        const uint32_t remainder =
            (fraction_numerator * this->num_phases_) % this->simplified_output_rate_;
        // Fractional part scaled to BLEND_FACTOR_BITS bits for blending (with rounding applied)
        constexpr auto MAX_BLEND = FormatOps<CoeffFormat>::max_blend_factor();
        info.blend_factor = (remainder * MAX_BLEND + (this->simplified_output_rate_ / 2)) /
                            this->simplified_output_rate_;

        // Rounding can push the blend factor up to MAX_BLEND, which overflows the
        // Q31 blend multiply in AccumulatorBlender (blend << (31 - BLEND_FACTOR_BITS)
        // hits 2^31). Clamp to MAX_BLEND - 1; the error is below blend quantization.
        if (info.blend_factor >= static_cast<int32_t>(MAX_BLEND)) {
            info.blend_factor = static_cast<int32_t>(MAX_BLEND) - 1;
        }

        return info;
    }

    // ASRC path: uses 4.28 fixed-point arithmetic
    PhaseInfo calculate_phase_asrc() {
        return decode_phase_4_28(this->phase_accumulator_, this->num_phases_);
    }

    // Stateless body of the ASRC phase calculation, shared with the fast-path
    // decode_asrc_phase() accessor
    static PhaseInfo decode_phase_4_28(uint32_t accumulator, uint32_t num_phases) {
        PhaseInfo info{};

        // Calculate current position from accumulator (4.28 format)
        // Upper 4 bits = integer samples ahead of consumed position (0-15 range)
        // Lower 28 bits = fractional position within sample
        uint32_t integer_part = accumulator >> FRACTIONAL_BITS;
        uint32_t fractional = accumulator & FRACTIONAL_MASK;

        info.samples_to_consume = integer_part;

        // Calculate phase index and blend factor from fractional position
        // All 32-bit arithmetic: fractional is 28 bits, num_phases is max 10 bits
        // We shift fractional right by 6 to leave room: (28-6) + 10 = 32 bits max
        // Bits of fractional precision remaining in phase_product after the
        // frac_scaled shift above (28 fractional bits - 6-bit pre-shift = 22).
        constexpr int PHASE_PRODUCT_FRACTIONAL_BITS = 22;
        constexpr uint32_t PHASE_PRODUCT_FRACTIONAL_MASK = 0x3FFFFF;  // 22-bit fractional mask

        uint32_t frac_scaled = fractional >> 6;             // Keep upper 22 bits
        uint32_t phase_product = frac_scaled * num_phases;  // Fits in 32 bits
        info.phase_index =
            static_cast<uint16_t>(phase_product >> PHASE_PRODUCT_FRACTIONAL_BITS);  // Integer part
        uint32_t blend_raw = phase_product & PHASE_PRODUCT_FRACTIONAL_MASK;  // 22-bit fractional

        // Scale blend to BLEND_FACTOR_BITS using rounding shift
        info.blend_factor = static_cast<int32_t>(PSHR32(blend_raw, 22 - BLEND_FACTOR_BITS));

        // The rounding shift can produce 1 << BLEND_FACTOR_BITS, which overflows the
        // Q31 blend multiply in AccumulatorBlender. Clamp to the max representable
        // blend; the error is below blend quantization.
        constexpr int32_t BLEND_LIMIT = (1 << BLEND_FACTOR_BITS) - 1;
        if (info.blend_factor > BLEND_LIMIT) {
            info.blend_factor = BLEND_LIMIT;
        }

        return info;
    }

    // End any slew in progress (the caller sets current_ratio_/target_ratio_)
    void clear_slew() {
        this->ratio_delta_ = 0;
        this->slew_remainder_ = 0;
        this->slew_carry_ = 0;
        this->slew_length_ = 0;
        this->slew_error_ = 0;
        this->slew_samples_remaining_ = 0;
    }

    // ========================================
    // GCD Helper
    // ========================================

    static uint32_t gcd(uint32_t a, uint32_t b) {
        while (b != 0) {
            uint32_t t = b;
            b = a % b;
            a = t;
        }
        return a;
    }

    // ========================================
    // State
    // ========================================

    // Mode tracking
    // Default member initializers below are defensive: init() unconditionally
    // overwrites every field before first use, but they keep the class free
    // of uninitialized-read warnings without requiring a constructor.
    Mode mode_ = Mode::FIXED;

    // Original input rate (before GCD simplification), used by get_nominal_rate()
    uint32_t original_input_rate_ = 0;

    // Fixed-mode state (32-bit integer arithmetic)
    uint32_t simplified_input_rate_ = 0;
    uint32_t simplified_output_rate_ = 0;
    uint32_t phase_input_frames_ = 0;
    uint32_t phase_output_frames_ = 0;
    int32_t phase_aligned_accumulator_ = 0;
    bool is_aligned_ = false;  // True if using aligned (zero-blend) path

    // ASRC-mode state (4.28 fixed-point)
    // Phase accumulator: 4.28 fixed-point representing fractional input position
    // Upper 4 bits = integer sample index (relative to consumed, 0-15 range)
    // Lower 28 bits = fractional position for phase interpolation
    uint32_t phase_accumulator_ = 0;

    // Base ratio: the original conversion ratio in 4.28 format
    // e.g., for 48kHz->44.1kHz, this is (48000/44100) << 28
    uint32_t base_ratio_4_28_ = 0;

    // Rate ratio: 4.28 fixed-point (input samples per output sample)
    // These are adjusted versions of base_ratio_4_28_ (with PPM applied)
    uint32_t current_ratio_ = 0;
    uint32_t target_ratio_ = 0;
    // Slew state (see step_slew()): per-output quotient step, the remainder's magnitude and
    // sign, the slew length it is spread over, and the Bresenham error accumulator
    int32_t ratio_delta_ = 0;
    uint32_t slew_remainder_ = 0;
    int32_t slew_carry_ = 0;
    uint32_t slew_length_ = 0;
    uint32_t slew_error_ = 0;
    uint32_t slew_samples_remaining_ = 0;

    // Cumulative subsample offset from base ratio (4.28 format, signed)
    // Wrapped to +/-UNITY_RATIO range. Positive = running slow, negative = running fast
    // A block drifting more than +/-8 samples wraps int32: large set_ratio() jumps, or
    // ppm-level drift only in very large blocks (+/-1000 ppm needs ~8000 outputs in one
    // block). The wrapped value stays correct modulo one sample (2^28 divides 2^32); only
    // which side of zero it lands on can change, an ambiguity per-block truncation already
    // has.
    int32_t cumulative_subsample_offset_ = 0;

    // Initial samples to consume for latency compensation.
    // The 4.28 format only supports 4 bits of integer part (0-15), but proper
    // latency compensation requires skipping filter_length/2+1 samples initially.
    uint32_t initial_skip_count_ = 0;

    // Common state
    uint32_t num_phases_ = 0;
    int filter_length_ = 0;
};

}  // namespace detail
}  // namespace micro_resampler

#endif  // RATIO_TRACKER_H
