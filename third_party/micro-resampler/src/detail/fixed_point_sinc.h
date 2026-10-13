// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file fixed_point_sinc.h
/// @brief Pure integer sinc(x) = sin(πx)/(πx) computation for fixed-point coefficient generation
///
/// Uses a 9th-order minimax polynomial to evaluate sin(πx) and a
/// Newton-Raphson iteration to compute the reciprocal, without any
/// floating-point operations. Provides a Q24-input path plus a
/// higher-precision Q50-input path for argument reduction on large x, along
/// with float/Q24/Q30 conversion helpers.

#ifndef FIXED_POINT_SINC_H
#define FIXED_POINT_SINC_H

#include <cstdint>
#include <cstdlib>

namespace micro_resampler {
namespace fixed_point_sinc {

// ============================================================================
// Constants
// ============================================================================

// π in Q29 format: π * 2^29 = 1686629713.11... ≈ 1686629713
static constexpr int32_t PI_Q29 = 1686629713;

// Threshold for treating x as zero in sinc calculation
// For |x| < this threshold, sinc(x) ≈ 1.0
// In Q24, this represents approximately 1e-5
static constexpr int32_t SINC_ZERO_THRESHOLD_Q24 = 168;  // ~1e-5 in Q24

// Q50 zero threshold (~1e-5 in Q50)
// 1e-5 * 2^50 ≈ 11258999
static constexpr int64_t SINC_ZERO_THRESHOLD_Q50 = 11258999;

// Shift/format-point for Q30 fixed-point values. Used both as a right-shift
// amount when renormalizing a wider intermediate product down to Q30, and as
// the literal Q-format identifier (e.g. `*result_q_format = SHIFT_Q30`).
static constexpr int32_t SHIFT_Q30 = 30;

// Shift representing one-half (0.5) within a Q30 value, i.e. 2^29. Used to
// clamp Newton-Raphson estimates/results to ±0.5 in Q30.
static constexpr int32_t HALF_Q30_SHIFT = 29;

// Q format of the pi_abs_x_q21 argument passed into fixed_reciprocal_positive
// (π|x| kept in Q21 to avoid overflow for large x).
static constexpr int32_t PI_ABS_X_Q_FORMAT = 21;

// ============================================================================
// Minimax polynomial coefficients for sin(πr) where r ∈ [-0.5, 0.5]
// ============================================================================
// These coefficients are optimized using the Remez algorithm to minimize the
// maximum error over the entire interval, unlike Taylor coefficients which
// minimize error only near zero.
//
// 9th-order minimax polynomial for sin(πr):
//   sin(πr) ≈ r * (c1 + r² * (c3 + r² * (c5 + r² * (c7 + r² * c9))))
//
// Minimax coefficients (computed via Remez exchange algorithm):
//   c1 = π                           ≈  3.14159265358979323846
//   c3 = -(π³/6) * adjustment        ≈ -5.16771278004994687390
//   c5 = (π⁵/120) * adjustment       ≈  2.55016403987734540810
//   c7 = -(π⁷/5040) * adjustment     ≈ -0.59926452932078866680
//   c9 = (π⁹/362880) * adjustment    ≈  0.08214588661109374235
//
// The minimax polynomial has approximately 10x lower maximum error compared
// to the Taylor polynomial of the same order over the interval [-0.5, 0.5].
//
// Q format selection to avoid overflow:
//   c1 in Q28 (range [-8, 8))   - π ≈ 3.14
//   c3 in Q27 (range [-16, 16)) - -π³/6 ≈ -5.17
//   c5 in Q28 (range [-8, 8))   - π⁵/120 ≈ 2.55
//   c7 in Q29 (range [-4, 4))   - -π⁷/5040 ≈ -0.60
//   c9 in Q30 (range [-2, 2))   - π⁹/362880 ≈ 0.08

// Minimax coefficients for sin(πr), r ∈ [-0.5, 0.5]
// These are carefully computed minimax (Chebyshev) coefficients
static constexpr int32_t SIN_PI_R_MM_C1_Q28 = 843314857;   // π * 2^28
static constexpr int32_t SIN_PI_R_MM_C3_Q27 = -693598668;  // -π³/6 * 2^27
static constexpr int32_t SIN_PI_R_MM_C5_Q28 = 684554584;   // π⁵/120 * 2^28 (minimax adjusted)
static constexpr int32_t SIN_PI_R_MM_C7_Q29 = -321735530;  // -π⁷/5040 * 2^29 (minimax adjusted)
static constexpr int32_t SIN_PI_R_MM_C9_Q30 = 88206438;    // π⁹/362880 * 2^30 (minimax adjusted)

// ============================================================================
// 9th-order minimax polynomial for sin(πr) where r ∈ [-0.5, 0.5]
// ============================================================================
// Input: r_q31 in Q31 format, representing r ∈ [-0.5, 0.5]
// Output: sin(πr) in Q30 format
//
// Uses 9th-order minimax polynomial with Horner's method.
// This provides better precision than 7th-order Taylor by:
// 1. Using an additional term (c9)
// 2. Using minimax-optimized coefficients
//
// Expected precision: ~28 bits (improved from ~24 bits with Taylor)

inline int32_t sin_polynomial_minimax_q30(int32_t r_q31) {
    // Shift used for the first three Horner steps below: each multiplies the
    // Q30 r² factor by the running sum and steps the result down one Q level
    // to align with the next (lower) minimax coefficient's Q format.
    constexpr int POLY_HORNER_DOWNSHIFT = 31;

    // Shift used for the final two Horner steps, which turn back around and
    // renormalize the last two products up to their target Q28/Q30 formats.
    constexpr int POLY_HORNER_FINAL_SHIFT = 29;

    // r² in Q62, then shift to Q30 for further calculations
    int64_t r2_q62 = static_cast<int64_t>(r_q31) * r_q31;
    int32_t r2_q30 = static_cast<int32_t>(r2_q62 >> 32);  // Q62 >> 32 = Q30

    // Evaluate polynomial using Horner's method:
    // sin(πr) = r * (c1 + r² * (c3 + r² * (c5 + r² * (c7 + r² * c9))))
    // All intermediate sums kept in appropriate Q formats to avoid overflow

    // Step 1: c9 * r² (Q30 * Q30 = Q60, shift to Q29)
    int64_t inner_q60 = static_cast<int64_t>(SIN_PI_R_MM_C9_Q30) * r2_q30;
    int32_t inner_q29 = static_cast<int32_t>(inner_q60 >> POLY_HORNER_DOWNSHIFT);

    // Step 2: c7 + r² * c9 (Q29 + Q29 = Q29)
    int32_t sum0_q29 = SIN_PI_R_MM_C7_Q29 + inner_q29;

    // Step 3: r² * (c7 + r² * c9) (Q30 * Q29 = Q59, shift to Q28)
    int64_t prod1_q59 = static_cast<int64_t>(r2_q30) * sum0_q29;
    int32_t prod1_q28 = static_cast<int32_t>(prod1_q59 >> POLY_HORNER_DOWNSHIFT);

    // Step 4: c5 + r² * (c7 + r² * c9) (Q28 + Q28 = Q28)
    int32_t sum1_q28 = SIN_PI_R_MM_C5_Q28 + prod1_q28;

    // Step 5: r² * (c5 + r² * (c7 + r² * c9)) (Q30 * Q28 = Q58, shift to Q27)
    int64_t prod2_q58 = static_cast<int64_t>(r2_q30) * sum1_q28;
    int32_t prod2_q27 = static_cast<int32_t>(prod2_q58 >> POLY_HORNER_DOWNSHIFT);

    // Step 6: c3 + r² * (c5 + r² * ...) (Q27 + Q27 = Q27)
    int32_t sum2_q27 = SIN_PI_R_MM_C3_Q27 + prod2_q27;

    // Step 7: r² * (c3 + r² * ...) (Q30 * Q27 = Q57, shift to Q28)
    int64_t prod3_q57 = static_cast<int64_t>(r2_q30) * sum2_q27;
    int32_t prod3_q28 = static_cast<int32_t>(prod3_q57 >> POLY_HORNER_FINAL_SHIFT);

    // Step 8: c1 + r² * (c3 + ...) (Q28 + Q28 = Q28)
    int32_t sum3_q28 = SIN_PI_R_MM_C1_Q28 + prod3_q28;

    // Step 9: r * (c1 + r² * ...) (Q31 * Q28 = Q59, shift to Q30)
    int64_t result_q59 = static_cast<int64_t>(r_q31) * sum3_q28;
    int32_t result_q30 = static_cast<int32_t>(result_q59 >> POLY_HORNER_FINAL_SHIFT);

    return result_q30;
}

// ============================================================================
// sin(πx) with argument reduction using minimax polynomial
// ============================================================================
// Input: x_q24 in Q24 format (can represent values up to ±128)
// Output: sin(πx) in Q30 format
//
// This is an improved version of sin_pi_x_q30 that uses the 9th-order
// minimax polynomial for better precision.
//
// Uses argument reduction:
//   sin(πx) = sin(π*k + π*r) where k = round(x), r = x - k
//           = sin(πk)*cos(πr) + cos(πk)*sin(πr)
//           = 0 + (-1)^k * sin(πr)    [since sin(πk)=0, cos(πk)=(-1)^k]
//
// This reduces arbitrary x to r ∈ [-0.5, 0.5]

inline int32_t sin_pi_x_minimax_q30(int32_t x_q24) {
    // k = round(x)
    // In Q24, 0.5 = 2^23 = 8388608
    // Add 0.5 and truncate to get rounded value
    constexpr int Q24_HALF_SHIFT = 23;  // shift for 0.5 in Q24
    int32_t half_q24 = 1 << Q24_HALF_SHIFT;

    // Careful rounding for negative values
    int32_t x_rounded = 0;
    if (x_q24 >= 0) {
        x_rounded = (x_q24 + half_q24) >> 24;
    } else {
        x_rounded = -(((-x_q24) + half_q24) >> 24);
    }

    int32_t k = x_rounded;

    // r = x - k in Q24
    // k in Q24 = k << 24, but we need to be careful with overflow
    int32_t k_q24 = k << 24;
    int32_t r_q24 = x_q24 - k_q24;

    // Convert r from Q24 to Q31 for polynomial evaluation
    // r is in [-0.5, 0.5], so r_q31 = r_q24 << 7
    int32_t r_q31 = r_q24 << 7;

    // Compute sin(πr) using 9th-order minimax polynomial
    int32_t sin_pi_r = sin_polynomial_minimax_q30(r_q31);

    // Apply sign: (-1)^k
    // If k is odd, negate the result
    return (k & 1) ? -sin_pi_r : sin_pi_r;
}

// ============================================================================
// Newton-Raphson reciprocal
// ============================================================================
// Computes 1/d using Newton-Raphson iteration.
// Input: d in Qd format (specified by d_q_format), d > 0
// Output: 1/d, returned Q format stored in result_q_format
//
// Newton-Raphson iteration: x_{n+1} = x_n * (2 - d * x_n)
// 4 iterations gives ~Q30 precision

inline int32_t fixed_reciprocal_positive(int32_t d, int d_q_format, int* result_q_format) {
    // Handle edge cases
    if (d <= 0) {
        *result_q_format = SHIFT_Q30;
        return INT32_MAX;  // Return max value for invalid input
    }

    // Count leading zeros to find position of MSB
    // This tells us the magnitude of d
    int lz = 0;
    int32_t temp = d;
    while (temp > 0 && (temp & 0x40000000) == 0) {
        temp <<= 1;
        lz++;
    }

    // d has MSB at bit (30 - lz)
    // d's true value = d / 2^d_q_format
    // We need to normalize to [0.5, 1.0) for Newton-Raphson
    //
    // After shifting d left by lz, d_norm has MSB at bit 30
    // So d_norm is in range [2^30, 2^31)
    // To interpret as [0.5, 1.0), we treat d_norm as Q31
    // d_norm_true = d_norm / 2^31
    //
    // d_true = d / 2^d_q_format
    // d_norm = d << lz = d * 2^lz
    // d_norm_true = d_norm / 2^31 = (d * 2^lz) / 2^31 = d / 2^(31-lz)
    //
    // For d_norm_true to equal d_true:
    //   d / 2^(31-lz) = d / 2^d_q_format
    //   31 - lz = d_q_format
    //   lz = 31 - d_q_format
    //
    // But our lz is based on the actual bit position, not d_q_format.
    // So d_norm_true = d / 2^(31-lz) is NOT equal to d_true unless d_q_format = 31-lz.
    //
    // The relationship is:
    //   d_norm_true = d_true * 2^(d_q_format - 31 + lz)
    //
    // If lz > 31 - d_q_format, then d_norm_true < d_true (we scaled down)
    // If lz < 31 - d_q_format, then d_norm_true > d_true (we scaled up)
    //
    // After computing 1/d_norm_true using Newton-Raphson in Q30:
    //   x / 2^30 = 1 / d_norm_true = 1 / (d_true * 2^(d_q_format - 31 + lz))
    //            = (1/d_true) * 2^(31 - d_q_format - lz)
    //
    // So: 1/d_true = (x / 2^30) * 2^(d_q_format + lz - 31)
    //             = x / 2^(30 - d_q_format - lz + 31)
    //             = x / 2^(61 - d_q_format - lz)
    //
    // This means x represents 1/d_true in Q(61 - d_q_format - lz) format.

    int32_t d_norm = d << lz;  // Has MSB at bit 30, treated as Q31 for [0.5, 1.0)

    // Initial estimate for 1/d_norm_true where d_norm_true ∈ [0.5, 1.0)
    // For d_norm_true in [0.5, 1.0), 1/d_norm_true in (1.0, 2.0]
    // Linear approximation: x0 ≈ 2.9142 - 2*d_norm_true
    // d_norm is Q31, so d_norm_true = d_norm / 2^31
    // 2.9142 in Q30 = 3130388889
    constexpr int64_t NR_INITIAL_ESTIMATE_CONST_Q30 = 3130388889LL;
    // 2*d_norm_true in Q30 = 2 * (d_norm / 2^31) * 2^30 = d_norm / 2^31 * 2^31 = d_norm
    // But d_norm can be up to 2^31-1, so we use int64_t for the subtraction
    int64_t init_q30 = NR_INITIAL_ESTIMATE_CONST_Q30 - static_cast<int64_t>(d_norm);
    int32_t x = static_cast<int32_t>(init_q30);
    if (x < (1 << HALF_Q30_SHIFT)) {
        x = (1 << HALF_Q30_SHIFT);  // Clamp to minimum 0.5 in Q30 for stability
    }

    // Newton-Raphson iterations
    // x_{n+1} = x_n * (2 - d_norm_true * x_n)
    // d_norm is Q31, x is Q30
    // Shift used to renormalize the Q31 * Q30 = Q61 d_norm*x product to Q30
    constexpr int NR_D_NORM_PRODUCT_SHIFT = 31;
    for (int iter = 0; iter < 4; iter++) {
        // d_norm * x: Q31 * Q30 = Q61, need Q30 result
        // Shift right by 31: (d_norm * x) >> 31 gives Q30
        int64_t dx = static_cast<int64_t>(d_norm) * x;
        int32_t dx_q30 = static_cast<int32_t>(dx >> NR_D_NORM_PRODUCT_SHIFT);

        // 2 - dx: need 2 in Q30 = 2^31
        int64_t two_q30 = 2LL << SHIFT_Q30;
        int32_t residual = static_cast<int32_t>(two_q30 - dx_q30);

        // x * (2 - dx): Q30 * Q30 = Q60, shift to Q30
        int64_t x_new = static_cast<int64_t>(x) * residual;
        x = static_cast<int32_t>(x_new >> SHIFT_Q30);
    }

    // x is now 1/d_norm_true in Q30
    // x / 2^30 = 1 / d_norm_true
    // 1/d_true = (1/d_norm_true) * 2^(d_q_format - 31 + lz)
    // So x represents 1/d_true in Q(30 + 31 - d_q_format - lz) = Q(61 - d_q_format - lz)
    // 61 = Newton-Raphson Q30 result precision (30) + d_norm Q31 normalization (31)
    constexpr int NR_Q_FORMAT_BASE = 61;
    *result_q_format = NR_Q_FORMAT_BASE - d_q_format - lz;

    return x;
}

// ============================================================================
// sinc(x) = sin(πx) / (πx) using minimax polynomial
// ============================================================================
// Input: x_q24 in Q24 format (can represent values up to ±128)
// Output: sinc(x) in Q30 format
//
// Uses a 9th-order minimax polynomial for the sin(πx) calculation.
//
// Special case: sinc(0) = 1.0
// For |x| < threshold, return 1.0 to avoid division by zero

inline int32_t sinc_minimax_q30(int32_t x_q24) {
    // Handle x ≈ 0: sinc(0) = 1.0
    int32_t abs_x = x_q24 < 0 ? -x_q24 : x_q24;
    if (abs_x < SINC_ZERO_THRESHOLD_Q24) {
        return 1 << SHIFT_Q30;  // 1.0 in Q30
    }

    // sinc is an even function: sinc(-x) = sinc(x)
    // So we work with |x| to simplify sign handling
    int32_t abs_x_q24 = abs_x;

    // Compute sin(π|x|) in Q30 using minimax polynomial
    int32_t sin_pi_abs_x = sin_pi_x_minimax_q30(abs_x_q24);

    // Compute π|x| - keep in a wider Q format to avoid overflow for large x
    // |x| is Q24, π is Q29
    // π|x|: Q24 * Q29 = Q53
    // For large x (up to 128), π*x can be up to ~402, so Q24 would overflow
    // We keep it in Q21 instead (range up to ±1024)
    int64_t pi_abs_x_q53 = static_cast<int64_t>(abs_x_q24) * PI_Q29;
    int32_t pi_abs_x_q21 = static_cast<int32_t>(pi_abs_x_q53 >> 32);

    // Compute 1/(π|x|)
    int recip_q_format = 0;
    int32_t recip = fixed_reciprocal_positive(pi_abs_x_q21, PI_ABS_X_Q_FORMAT, &recip_q_format);

    // sin(π|x|) / (π|x|)
    // sin_pi_abs_x is Q30, recip is Q(recip_q_format)
    // Product: Q(30 + recip_q_format), want Q30
    int64_t result_q = static_cast<int64_t>(sin_pi_abs_x) * recip;

    // Shift to Q30
    int shift = recip_q_format;
    int32_t result = 0;
    if (shift >= 0) {
        result = static_cast<int32_t>(result_q >> shift);
    } else {
        result = static_cast<int32_t>(result_q << (-shift));
    }

    // Clamp to valid Q30 range (sinc values are in [-0.22, 1.0])
    if (result > (1 << SHIFT_Q30)) {
        result = (1 << SHIFT_Q30);
    }
    if (result < -(1 << HALF_Q30_SHIFT)) {
        result = -(1 << HALF_Q30_SHIFT);
    }

    return result;
}

// ============================================================================
// Q50 Functions for Higher Precision Sinc Computation
// ============================================================================
// The Q50 format uses 64-bit integers with 50 fractional bits.
// This provides much higher precision for argument reduction in sin(πx)
// calculations, especially for large x values (e.g., x = 63.5 in a 128-tap filter).
//
// Key improvement: In the argument reduction step r = x - round(x), Q50 preserves
// full precision for the fractional part r ∈ [-0.5, 0.5], which then converts to
// Q31 without precision loss.

// Convert double to Q50 format
// Q50 can represent values in approximately [-8192, 8192)
inline int64_t float_to_q50(double value) {
    constexpr int64_t ONE_Q50 = 1LL << 50;
    constexpr double MAX_VAL = 8192.0;
    if (value >= MAX_VAL) {
        return INT64_MAX;
    }
    if (value <= -MAX_VAL) {
        return INT64_MIN;
    }
    return static_cast<int64_t>(value * static_cast<double>(ONE_Q50));
}

// ============================================================================
// sin(πx) with Q50 argument reduction using minimax polynomial
// ============================================================================
// Input: x_q50 in Q50 format (can represent values up to ±8192)
// Output: sin(πx) in Q30 format
//
// Uses argument reduction with 64-bit precision:
//   sin(πx) = sin(π*k + π*r) where k = round(x), r = x - k
//           = sin(πk)*cos(πr) + cos(πk)*sin(πr)
//           = 0 + (-1)^k * sin(πr)    [since sin(πk)=0, cos(πk)=(-1)^k]
//
// The key advantage over Q24 is that r = x - k is computed in Q50,
// preserving full fractional precision before conversion to Q31 for
// polynomial evaluation.

inline int32_t sin_pi_x_minimax_q30_q50(int64_t x_q50) {
    constexpr int64_t HALF_Q50 = 1LL << 49;
    constexpr int64_t ONE_Q50 = 1LL << 50;

    // Shift to extract the rounded integer part k from a Q50 value (Q50 >> 50 = integer)
    constexpr int Q50_ROUND_SHIFT = 50;

    // k = round(x) using 64-bit arithmetic
    int64_t k = 0;
    if (x_q50 >= 0) {
        k = (x_q50 + HALF_Q50) >> Q50_ROUND_SHIFT;
    } else {
        k = -(((-x_q50) + HALF_Q50) >> Q50_ROUND_SHIFT);
    }

    // r = x - k in Q50 (r ∈ [-0.5, 0.5])
    int64_t r_q50 = x_q50 - (k * ONE_Q50);

    // Convert r from Q50 to Q31 for polynomial evaluation
    // r is in [-0.5, 0.5], so this preserves full Q31 precision
    // Q50 >> 19 = Q31
    constexpr int Q50_TO_Q31_SHIFT = 19;
    int32_t r_q31 = static_cast<int32_t>(r_q50 >> Q50_TO_Q31_SHIFT);

    // Compute sin(πr) using 9th-order minimax polynomial
    int32_t sin_pi_r = sin_polynomial_minimax_q30(r_q31);

    // Apply sign: (-1)^k
    // If k is odd, negate the result
    return ((k & 1) != 0) ? -sin_pi_r : sin_pi_r;
}

// ============================================================================
// sinc(x) = sin(πx) / (πx) using Q50 input for higher precision
// ============================================================================
// Input: x_q50 in Q50 format (can represent values up to ±8192)
// Output: sinc(x) in Q30 format
//
// This version uses Q50 for the argument reduction in sin(πx),
// providing significantly higher precision for large x values compared
// to the Q24 version.
//
// Special case: sinc(0) = 1.0
// For |x| < threshold, return 1.0 to avoid division by zero

inline int32_t sinc_minimax_q30_q50(int64_t x_q50) {
    // Handle x ≈ 0: sinc(0) = 1.0
    int64_t abs_x_q50 = x_q50 < 0 ? -x_q50 : x_q50;
    if (abs_x_q50 < SINC_ZERO_THRESHOLD_Q50) {
        return 1 << SHIFT_Q30;  // 1.0 in Q30
    }

    // For small x (|x| < 0.125), use Taylor series for better precision:
    // sinc(x) = 1 - (πx)²/6 + (πx)⁴/120 - ...
    // This avoids the precision loss from computing 1/(πx) for small x.
    // Threshold: 0.125 in Q50 = 140737488355328
    constexpr int64_t TAYLOR_THRESHOLD_Q50 = 140737488355328LL;

    if (abs_x_q50 < TAYLOR_THRESHOLD_Q50) {
        // Use Taylor series: sinc(x) = 1 - (πx)²/6 + (πx)⁴/120 - ...
        // This avoids precision loss from computing 1/(πx) for small x.
        //
        // Compute (πx)² carefully to avoid overflow:
        // π² ≈ 9.8696, stored as Q20 to keep intermediate products small
        constexpr int64_t PI_SQ_Q20 = 10347335LL;  // round(π² * 2^20)

        // Shift used to renormalize a Q50-scaled quantity down to Q30
        // (also removes the Q20 scale factor from PI_SQ_Q20 below)
        constexpr int Q50_TO_Q30_SHIFT = 20;

        // Convert x from Q50 to Q30 for manageable arithmetic
        int64_t x_q30 = abs_x_q50 >> Q50_TO_Q30_SHIFT;

        // x² in Q60
        int64_t x_sq_q60 = x_q30 * x_q30;

        // (πx)² = π² * x² : PI_SQ_Q20 * x_sq_q60 gives Q80, shift by 50 gives Q30
        int64_t pi_x_sq_q30 = (PI_SQ_Q20 * (x_sq_q60 >> SHIFT_Q30)) >> Q50_TO_Q30_SHIFT;

        // sinc(x) ≈ 1 - (πx)²/6 + (πx)⁴/120
        // 1/6 in Q30 = 178956971
        constexpr int64_t ONE_SIXTH_Q30 = 178956971LL;
        constexpr int64_t ONE_Q30 = 1LL << 30;

        // term1 = (πx)²/6 in Q30
        int64_t term1_q30 = (pi_x_sq_q30 * ONE_SIXTH_Q30) >> SHIFT_Q30;

        // For better accuracy, add the (πx)⁴/120 term
        // (πx)⁴ = ((πx)²)², but need to be careful with Q formats
        // pi_x_sq_q30 * pi_x_sq_q30 gives Q60
        int64_t pi_x_4_q60 = pi_x_sq_q30 * pi_x_sq_q30;
        // 1/120 in Q30 = 8947849
        constexpr int64_t ONE_120TH_Q30 = 8947849LL;
        int64_t term2_q30 = ((pi_x_4_q60 >> SHIFT_Q30) * ONE_120TH_Q30) >> SHIFT_Q30;

        int64_t result = ONE_Q30 - term1_q30 + term2_q30;

        return static_cast<int32_t>(result);
    }

    // For larger x, use the standard sin(πx)/(πx) formula
    // Compute sin(π|x|) in Q30 using Q50 argument reduction
    int32_t sin_pi_abs_x = sin_pi_x_minimax_q30_q50(abs_x_q50);

    // Compute π|x| for the denominator
    // We need to reduce x to avoid overflow in the multiplication
    // Reduce x from Q50 to Q21 (shift right by 29), then multiply by PI_Q29
    // This gives π|x| in Q50, which we then reduce to Q21 for the reciprocal
    constexpr int Q50_TO_Q21_SHIFT = 29;
    int32_t abs_x_q21 = static_cast<int32_t>(abs_x_q50 >> Q50_TO_Q21_SHIFT);
    int64_t pi_abs_x_q50 = static_cast<int64_t>(abs_x_q21) * PI_Q29;
    int32_t pi_abs_x_q21 = static_cast<int32_t>(pi_abs_x_q50 >> Q50_TO_Q21_SHIFT);

    // Compute 1/(π|x|)
    int recip_q_format = 0;
    int32_t recip = fixed_reciprocal_positive(pi_abs_x_q21, PI_ABS_X_Q_FORMAT, &recip_q_format);

    // sin(π|x|) / (π|x|)
    // sin_pi_abs_x is Q30, recip is Q(recip_q_format)
    // Product: Q(30 + recip_q_format), want Q30
    int64_t result_q = static_cast<int64_t>(sin_pi_abs_x) * recip;

    // Shift to Q30
    int shift = recip_q_format;
    int32_t result = 0;
    if (shift >= 0) {
        result = static_cast<int32_t>(result_q >> shift);
    } else {
        result = static_cast<int32_t>(result_q << (-shift));
    }

    // Clamp to valid Q30 range (sinc values are in [-0.22, 1.0])
    if (result > (1 << SHIFT_Q30)) {
        result = (1 << SHIFT_Q30);
    }
    if (result < -(1 << HALF_Q30_SHIFT)) {
        result = -(1 << HALF_Q30_SHIFT);
    }

    return result;
}

// ============================================================================
// Convenience conversion functions
// ============================================================================

// Convert float to Q24 format (for sinc input)
inline int32_t float_to_q24(float value) {
    // Q24 can represent values in [-128, 128)
    if (value >= 128.0F) {
        return INT32_MAX;
    }
    if (value <= -128.0F) {
        return INT32_MIN;
    }
    return static_cast<int32_t>(value * static_cast<float>(1 << 24));
}

// Convert Q30 to float (for sinc output)
inline float q30_to_float(int32_t value_q30) {
    return static_cast<float>(value_q30) / static_cast<float>(1 << SHIFT_Q30);
}

}  // namespace fixed_point_sinc
}  // namespace micro_resampler

#endif  // FIXED_POINT_SINC_H
