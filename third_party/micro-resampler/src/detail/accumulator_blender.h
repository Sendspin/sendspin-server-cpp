// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file accumulator_blender.h
/// @brief Blends two filter-phase accumulators at full precision
///
/// Defines the AccumulatorBlender template, specialized for each coefficient
/// format (Q15Format, Q31LiteFormat, Q31Format, Float64Format, Float32Format).
/// Each specialization's blend() function linearly interpolates between the
/// accumulators of two adjacent polyphase filter phases for phase interpolation.

#ifndef ACCUMULATOR_BLENDER_H
#define ACCUMULATOR_BLENDER_H

#include "fixed_generic.h"
#include "formats.h"

#include <cstdint>

namespace micro_resampler {

template <typename CoeffFormat>
struct AccumulatorBlender;

// Q15Format: Blend Q30 accumulators
template <>
struct AccumulatorBlender<Q15Format> {
    using accumulator_t = int32_t;

    // Blend two Q30 accumulators using lerp: result = acc1 + (acc2 - acc1) * blend_ratio
    // blend_factor: 0 = 100% acc1, max_blend = 100% acc2
    // Uses single MULT32_32_Q32 (mulsh on ESP32) instead of two multiplies
    static inline accumulator_t blend(accumulator_t acc1, accumulator_t acc2, int blend_factor,
                                      int blend_bits) {
        // diff is small for adjacent filter phases, safe to shift to Q31
        // Q31 total fractional bits (int32 minus sign bit); function-local to avoid
        // colliding with same-named constants in other headers pulled into a TU.
        constexpr int Q31_FRACTIONAL_BITS = 31;
        int32_t diff = acc2 - acc1;
        int32_t diff_q31 = SHL32(diff, 1);                                       // Q30 -> Q31
        int32_t blend_q31 = blend_factor << (Q31_FRACTIONAL_BITS - blend_bits);  // Q31 blend ratio
        int32_t delta_q30 = MULT32_32_Q32(diff_q31, blend_q31);  // Q31 * Q31 >> 32 = Q30
        return acc1 + delta_q30;
    }
};

// Q31LiteFormat: Blend Q30 accumulators (same approach as Q15Format)
template <>
struct AccumulatorBlender<Q31LiteFormat> {
    using accumulator_t = int32_t;
    static inline accumulator_t blend(accumulator_t acc1, accumulator_t acc2, int blend_factor,
                                      int blend_bits) {
        // Q31 total fractional bits (int32 minus sign bit); function-local to avoid
        // colliding with same-named constants in other headers pulled into a TU.
        constexpr int Q31_FRACTIONAL_BITS = 31;
        int32_t diff = acc2 - acc1;
        int32_t diff_q31 = SHL32(diff, 1);                                       // Q30 -> Q31
        int32_t blend_q31 = blend_factor << (Q31_FRACTIONAL_BITS - blend_bits);  // Q31 blend ratio
        int32_t delta_q30 = MULT32_32_Q32(diff_q31, blend_q31);  // Q31 * Q31 >> 32 = Q30
        return acc1 + delta_q30;
    }
};

// Q31Format: Blend Q62 accumulators
template <>
struct AccumulatorBlender<Q31Format> {
    using accumulator_t = int64_t;

    // Blend two Q62 accumulators, returning Q62 result
    static inline accumulator_t blend(accumulator_t acc1, accumulator_t acc2, int blend_factor,
                                      int blend_bits) {
        // For Q62, we need to be careful about overflow
        // Pre-shift to make room for blend multiply
        accumulator_t acc1_shifted = PSHR64(acc1, blend_bits);
        accumulator_t acc2_shifted = PSHR64(acc2, blend_bits);

        int max_blend = 1 << blend_bits;
        return acc1_shifted * (max_blend - blend_factor) + acc2_shifted * blend_factor;
    }
};

// Float64Format: Blend double accumulators
template <>
struct AccumulatorBlender<Float64Format> {
    using accumulator_t = double;

    // Blend two double accumulators using linear interpolation
    static inline accumulator_t blend(accumulator_t acc1, accumulator_t acc2, int blend_factor,
                                      int blend_bits) {
        double blend_ratio =
            static_cast<double>(blend_factor) / static_cast<double>(1 << blend_bits);
        return acc1 + (acc2 - acc1) * blend_ratio;
    }
};

// Float32Format: Blend float accumulators
template <>
struct AccumulatorBlender<Float32Format> {
    using accumulator_t = float;

    // Blend two float accumulators using linear interpolation
    static inline accumulator_t blend(accumulator_t acc1, accumulator_t acc2, int blend_factor,
                                      int blend_bits) {
        float blend_ratio = static_cast<float>(blend_factor) / static_cast<float>(1 << blend_bits);
        return acc1 + (acc2 - acc1) * blend_ratio;
    }
};

}  // namespace micro_resampler

#endif  // ACCUMULATOR_BLENDER_H
