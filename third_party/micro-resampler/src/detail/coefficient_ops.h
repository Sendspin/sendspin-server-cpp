// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file coefficient_ops.h
/// @brief Per-format arithmetic operations used by the polyphase filter dot product
///
/// Defines the FormatOps template, specialized for each sample and coefficient
/// format (Q15Format, Q31Format, Q31LiteFormat, Float64Format, Float32Format).
/// Each specialization provides float-to-coefficient conversion, the standard
/// multiply-accumulate, sample-to-float and float-to-sample conversion, the
/// maximum blend factor, and the raw multiply / shift-and-accumulate operations
/// used by the block-adaptive-Q dot product path.

#ifndef COEFFICIENT_OPS_H
#define COEFFICIENT_OPS_H

#include "fixed_generic.h"
#include "formats.h"

#include <cstdint>

// BLEND_FACTOR_BITS is defined in formats.h

namespace micro_resampler {
namespace detail {

// ============================================================================
// Format Operations Template
// ============================================================================
// Primary template - specialized for each format type below
template <typename Format>
struct FormatOps;

// ============================================================================
// Q15 Format Operations
// ============================================================================
template <>
struct FormatOps<Q15Format> {
    using coefficient_t = typename Q15Format::coefficient_t;
    using processing_t = typename Q15Format::processing_t;
    using accumulator_t = typename Q15Format::accumulator_t;
    using sample_t = typename Q15Format::sample_t;

    static constexpr int PRECISION_BITS = Q15Format::PRECISION_BITS;

    // Convert floating-point value to Q15 coefficient
    // Input is converted to Q31, then shifted down to Q15
    static inline coefficient_t float_to_coefficient(double x) {
        return static_cast<coefficient_t>(PSHR32_SAFE(FLOAT_TO_Q31(x), 31 - PRECISION_BITS));
    }

    // Standard MAC: int16 × Q15 → Q30 accumulator
    static inline accumulator_t mac_coefficient(accumulator_t c, sample_t a, coefficient_t b) {
        return MAC16_16(c, a, b);
    }

    // Multiply for high-precision sample processing: Q31 × Q15 → Q30 (for pre-extracted
    // high-precision taps; the caller accumulates). Written as one 32x32 widening multiply
    // (both operands 32-bit before the promotion) so Xtensa GCC emits a single mulsh;
    // bit-exact with MULT32_32_Q32(a, SHL32(EXTEND32(b), 16)) because the sample operand's
    // low 16 bits are zero, which removes the cross-term truncation the decomposed 32-bit
    // formula otherwise has
    static inline accumulator_t multiply_sample_processing(processing_t a, sample_t b) {
        const int32_t sample_q31 = SHL32(EXTEND32(b), 16);
        return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int64_t>(sample_q31)) >>
                                    32);
    }

    // Convert sample_t to float for LPC extrapolation
    // Q15 samples range [-32768, 32767] → [-1.0, 1.0)
    static inline float sample_to_float(sample_t s) {
        return static_cast<float>(s) / 32768.0F;
    }

    // Convert float to sample_t with saturation
    // [-1.0, 1.0) → [-32768, 32767]
    static inline sample_t sample_from_float(float f) {
        float scaled = f * 32768.0F;
        if (scaled >= 32767.0F) {
            return INT16_MAX;
        }
        if (scaled <= -32768.0F) {
            return -32768;
        }
        return static_cast<sample_t>(scaled);
    }

    // ========================================
    // Blend factor operations for phase interpolation
    // ========================================

    // Maximum blend factor value (1 << BLEND_FACTOR_BITS)
    static constexpr accumulator_t max_blend_factor() {
        return 1 << BLEND_FACTOR_BITS;
    }

    // ========================================
    // Block-adaptive Q operations for dot product
    // ========================================

    // Raw multiply sample by coefficient (no accumulate)
    // Used in block-adaptive-Q path where shift happens after multiply
    static inline accumulator_t multiply_sample_coeff(sample_t s, coefficient_t c) {
        return MULT16_16(s, c);
    }

    // Accumulate product with right shift
    // Used in block-adaptive-Q path: acc += (product >> shift)
    static inline accumulator_t shift_and_accumulate(accumulator_t acc, accumulator_t product,
                                                     int shift) {
        return ADD32(acc, product >> shift);
    }
};

// ============================================================================
// Q31 Format Operations
// ============================================================================
template <>
struct FormatOps<Q31Format> {
    using coefficient_t = typename Q31Format::coefficient_t;
    using processing_t = typename Q31Format::processing_t;
    using accumulator_t = typename Q31Format::accumulator_t;
    using sample_t = typename Q31Format::sample_t;

    static constexpr int PRECISION_BITS = Q31Format::PRECISION_BITS;

    // Convert floating-point value to Q31 coefficient (no shift needed)
    static inline coefficient_t float_to_coefficient(double x) {
        return static_cast<coefficient_t>(PSHR32_SAFE(FLOAT_TO_Q31(x), 31 - PRECISION_BITS));
    }

    // Standard MAC: Q31 sample × Q31 coefficient → Q62 accumulator
    // Q31 × Q31 = Q62 (31 + 31 = 62 fractional bits)
    static inline accumulator_t mac_coefficient(accumulator_t c, sample_t a, coefficient_t b) {
        return ADD64(c, MULT32_32(a, b));
    }

    // Convert sample_t to float for LPC extrapolation
    // Q31 samples: full 32-bit range [-2147483648, 2147483647] → [-1.0, 1.0)
    static inline float sample_to_float(sample_t s) {
        return static_cast<float>(s) / 2147483648.0F;
    }

    // Convert float to sample_t with saturation
    // [-1.0, 1.0) → Q31 range [-2147483648, 2147483647]
    static inline sample_t sample_from_float(float f) {
        double scaled = static_cast<double>(f) * 2147483648.0;
        if (scaled >= 2147483647.0) {
            return INT32_MAX;
        }
        if (scaled <= -2147483648.0) {
            return INT32_MIN;
        }
        return static_cast<sample_t>(scaled);
    }

    // ========================================
    // Blend factor operations for phase interpolation
    // ========================================

    // Maximum blend factor value (1 << BLEND_FACTOR_BITS)
    static constexpr accumulator_t max_blend_factor() {
        return static_cast<accumulator_t>(1) << BLEND_FACTOR_BITS;
    }

    // ========================================
    // Block-adaptive Q operations for dot product
    // ========================================
    // Note: Block-adaptive Q is primarily designed for Q15 mode.
    // Q31 uses fixed precision, but we provide these for template compatibility.

    // Raw multiply sample by coefficient (no accumulate)
    static inline accumulator_t multiply_sample_coeff(sample_t s, coefficient_t c) {
        return MULT32_32(s, c);
    }

    // Accumulate product with right shift
    static inline accumulator_t shift_and_accumulate(accumulator_t acc, accumulator_t product,
                                                     int shift) {
        return acc + SHR64(product, shift);
    }
};

// ============================================================================
// Q31 Lite Format Operations (32-bit accumulator)
// ============================================================================
template <>
struct FormatOps<Q31LiteFormat> {
    using coefficient_t = typename Q31LiteFormat::coefficient_t;
    using processing_t = typename Q31LiteFormat::processing_t;
    using accumulator_t = typename Q31LiteFormat::accumulator_t;
    using sample_t = typename Q31LiteFormat::sample_t;

    static constexpr int PRECISION_BITS = Q31LiteFormat::PRECISION_BITS;

    static inline coefficient_t float_to_coefficient(double x) {
        return static_cast<coefficient_t>(PSHR32_SAFE(FLOAT_TO_Q31(x), 31 - PRECISION_BITS));
    }

    // MAC: Q31 sample x Q31 coefficient -> Q30 accumulator (32-bit)
    // Uses MULT32_32_Q32 which returns high 32 bits of 64-bit product
    static inline accumulator_t mac_coefficient(accumulator_t c, sample_t a, coefficient_t b) {
        return ADD32(c, MULT32_32_Q32(a, b));
    }

    static inline float sample_to_float(sample_t s) {
        return static_cast<float>(s) / 2147483648.0F;
    }

    static inline sample_t sample_from_float(float f) {
        double scaled = static_cast<double>(f) * 2147483648.0;
        if (scaled >= 2147483647.0) {
            return INT32_MAX;
        }
        if (scaled <= -2147483648.0) {
            return INT32_MIN;
        }
        return static_cast<sample_t>(scaled);
    }

    static constexpr accumulator_t max_blend_factor() {
        return 1 << BLEND_FACTOR_BITS;
    }

    static inline accumulator_t multiply_sample_coeff(sample_t s, coefficient_t c) {
        return MULT32_32_Q32(s, c);
    }

    static inline accumulator_t shift_and_accumulate(accumulator_t acc, accumulator_t product,
                                                     int shift) {
        return ADD32(acc, product >> shift);
    }
};

// ============================================================================
// Float64Format Operations
// ============================================================================
template <>
struct FormatOps<Float64Format> {
    using coefficient_t = typename Float64Format::coefficient_t;
    using processing_t = typename Float64Format::processing_t;
    using accumulator_t = typename Float64Format::accumulator_t;
    using sample_t = typename Float64Format::sample_t;

    static constexpr int PRECISION_BITS = Float64Format::PRECISION_BITS;

    // Convert floating-point value to coefficient (identity)
    static inline coefficient_t float_to_coefficient(double x) {
        return static_cast<coefficient_t>(x);
    }

    // Standard MAC: floating-point multiply-add
    static inline accumulator_t mac_coefficient(accumulator_t c, sample_t a, coefficient_t b) {
        return c + a * b;
    }

    // Convert sample_t to float for LPC extrapolation
    // Double sample → float (simple cast)
    static inline float sample_to_float(sample_t s) {
        return static_cast<float>(s);
    }

    // Convert float to sample_t
    // Float → double sample (simple cast)
    static inline sample_t sample_from_float(float f) {
        return static_cast<sample_t>(f);
    }

    // ========================================
    // Blend factor operations for phase interpolation
    // ========================================
    // For floating-point, blend operations are trivial - no shifting needed

    // Maximum blend factor value (used for normalization)
    static constexpr int max_blend_factor() {
        return 1 << BLEND_FACTOR_BITS;
    }

    // ========================================
    // Block-adaptive Q operations for dot product
    // ========================================
    // For floating-point, these are trivial - shifts are ignored

    // Raw multiply sample by coefficient (no accumulate)
    static inline accumulator_t multiply_sample_coeff(sample_t s, coefficient_t c) {
        return s * c;
    }

    // Accumulate product with right shift (shift ignored for float)
    static inline accumulator_t shift_and_accumulate(accumulator_t acc, accumulator_t product,
                                                     int shift) {
        (void)shift;
        return acc + product;
    }
};

// ============================================================================
// Float32Format Operations
// ============================================================================
template <>
struct FormatOps<Float32Format> {
    using coefficient_t = typename Float32Format::coefficient_t;
    using processing_t = typename Float32Format::processing_t;
    using accumulator_t = typename Float32Format::accumulator_t;
    using sample_t = typename Float32Format::sample_t;

    static constexpr int PRECISION_BITS = Float32Format::PRECISION_BITS;

    // Convert floating-point value to coefficient (cast from double)
    static inline coefficient_t float_to_coefficient(double x) {
        return static_cast<coefficient_t>(x);
    }

    // Standard MAC: floating-point multiply-add
    static inline accumulator_t mac_coefficient(accumulator_t c, sample_t a, coefficient_t b) {
        return c + a * b;
    }

    // Convert sample_t to float for LPC extrapolation (identity)
    static inline float sample_to_float(sample_t s) {
        return s;
    }

    // Convert float to sample_t (identity)
    static inline sample_t sample_from_float(float f) {
        return f;
    }

    // ========================================
    // Blend factor operations for phase interpolation
    // ========================================

    // Maximum blend factor value (used for normalization)
    static constexpr int max_blend_factor() {
        return 1 << BLEND_FACTOR_BITS;
    }

    // ========================================
    // Block-adaptive Q operations for dot product
    // ========================================

    // Raw multiply sample by coefficient (no accumulate)
    static inline accumulator_t multiply_sample_coeff(sample_t s, coefficient_t c) {
        return s * c;
    }

    // Accumulate product with right shift (shift ignored for float)
    static inline accumulator_t shift_and_accumulate(accumulator_t acc, accumulator_t product,
                                                     int shift) {
        (void)shift;
        return acc + product;
    }
};

}  // namespace detail
}  // namespace micro_resampler

#endif  // COEFFICIENT_OPS_H
