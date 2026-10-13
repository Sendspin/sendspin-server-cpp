// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file fixed_point.h
/// @brief Q15/Q31 fixed-point type aliases and per-block shift calculation
///
/// Defines the q15_t and q31_t integer aliases and calculate_block_shift(),
/// which finds the right-shift that packs a block of floating-point
/// coefficients into a target fractional-bit width after conversion to Q31,
/// for SIMD-friendly per-block scaling.

#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include "detail/fixed_generic.h"

#include <cstdint>

namespace micro_resampler {

typedef int16_t q15_t;
typedef int32_t q31_t;

// Q31 format provides 31 fractional bits (int32 minus the sign bit); this is
// the total budget shared between TARGET_BITS and the adaptive shift below.
constexpr int Q31_FRACTIONAL_BITS = 31;

// Template version: Calculate optimal shift for a block of coefficients (for SIMD optimization)
// All coefficients in the block will use the maximum shift needed (most conservative)
// Given a coefficient in range [-1.0, 1.0], finds the minimum shift
// where (coeff * 2^31) >> shift fits in the target precision (TARGET_BITS fractional bits)
// Returns shift value in range [0, 31 - TARGET_BITS]
template <int TARGET_BITS>
static inline uint8_t calculate_block_shift(const double* coeffs, int count) {
    static_assert(TARGET_BITS > 0 && TARGET_BITS <= Q31_FRACTIONAL_BITS,
                  "TARGET_BITS must be in range [1, 31]");
    constexpr int MAX_SHIFT = Q31_FRACTIONAL_BITS - TARGET_BITS;

    uint8_t max_shift = 0;

    for (int i = 0; i < count; i++) {
        // Convert to Q31 using helper function
        q31_t q31_value = FLOAT_TO_Q31(static_cast<float>(coeffs[i]));

        // Take absolute value for shift calculation
        uint32_t abs_value = (q31_value < 0) ? -q31_value : q31_value;

        if (abs_value == 0) {
            continue;  // Zero coefficient needs no shift
        }

        // Find how many bits are needed: 32 - clz(abs_value)
        // Then shift needed is: bits_needed - TARGET_BITS
        int leading_zeros = __builtin_clz(abs_value);
        int bits_needed = 32 - leading_zeros;
        int shift = bits_needed - TARGET_BITS;

        // Clamp to valid range [0, MAX_SHIFT]
        if (shift < 0) {
            shift = 0;
        }
        if (shift > MAX_SHIFT) {
            shift = MAX_SHIFT;
        }

        if (static_cast<uint8_t>(shift) > max_shift) {
            max_shift = static_cast<uint8_t>(shift);
        }
    }

    return max_shift;
}

template <int TARGET_BITS>
static inline uint8_t calculate_block_shift(const float* coeffs, int count) {
    static_assert(TARGET_BITS > 0 && TARGET_BITS <= Q31_FRACTIONAL_BITS,
                  "TARGET_BITS must be in range [1, 31]");
    constexpr int MAX_SHIFT = Q31_FRACTIONAL_BITS - TARGET_BITS;

    uint8_t max_shift = 0;

    for (int i = 0; i < count; i++) {
        // Convert to Q31 using helper function
        q31_t q31_value = FLOAT_TO_Q31(coeffs[i]);

        // Take absolute value for shift calculation
        uint32_t abs_value = (q31_value < 0) ? -q31_value : q31_value;

        if (abs_value == 0) {
            continue;  // Zero coefficient needs no shift
        }

        // Find how many bits are needed: 32 - clz(abs_value)
        // Then shift needed is: bits_needed - TARGET_BITS
        int leading_zeros = __builtin_clz(abs_value);
        int bits_needed = 32 - leading_zeros;
        int shift = bits_needed - TARGET_BITS;

        // Clamp to valid range [0, MAX_SHIFT]
        if (shift < 0) {
            shift = 0;
        }
        if (shift > MAX_SHIFT) {
            shift = MAX_SHIFT;
        }

        if (static_cast<uint8_t>(shift) > max_shift) {
            max_shift = static_cast<uint8_t>(shift);
        }
    }

    return max_shift;
}

}  // namespace micro_resampler

#endif
