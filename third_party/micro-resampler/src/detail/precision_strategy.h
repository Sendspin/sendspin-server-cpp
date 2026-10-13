// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file precision_strategy.h
/// @brief Precision strategy structs for Kaiser-windowed sinc coefficient generation
///
/// Defines a family of self-contained strategy types (single/double
/// precision, table-based double/float/fixed-point, and an optional quad
/// precision strategy) that each supply bessel_i0(), sinc(), and
/// evaluate_window() for computing Kaiser window filter taps.
/// DefaultPrecisionStrategy selects the appropriate strategy for the target
/// platform.

#ifndef PRECISION_STRATEGY_H
#define PRECISION_STRATEGY_H

#include "fixed_point_sinc.h"
#include "kaiser_tables.h"
#include <math.h>

#include <cmath>
#include <cstdint>

// ============================================================================
// Quad Precision Support
// ============================================================================
// Define RESAMPLER_ENABLE_QUAD_PRECISION to enable 128-bit float support
// via Boost.Multiprecision. This is useful for highest-precision coefficient
// generation.
//
// On non-ESP platforms, this is auto-enabled when the Boost headers are on the
// include path. Define RESAMPLER_DISABLE_QUAD_PRECISION to prevent auto-enabling.
// Quad is opt-in (PrecisionStrategyType::QUAD); it is never the default.
#if !defined(RESAMPLER_ENABLE_QUAD_PRECISION) && !defined(RESAMPLER_DISABLE_QUAD_PRECISION)
#if !defined(ESP_PLATFORM) && !defined(ESP32) && !defined(ESP32S3)
#if defined(__has_include)
#if __has_include(<boost/multiprecision/cpp_bin_float.hpp>)
#define RESAMPLER_ENABLE_QUAD_PRECISION 1
#endif
#endif
#endif
#endif

#ifdef RESAMPLER_ENABLE_QUAD_PRECISION
// boost/multiprecision/fwd.hpp only forward-declares the cpp_bin_float
// template; the cpp_bin_float_quad alias and the abs/exp/sqrt overloads used
// below need the full definitions.
#include <boost/math/constants/constants.hpp>      // IWYU pragma: keep
#include <boost/multiprecision/cpp_bin_float.hpp>  // IWYU pragma: keep
#endif

namespace micro_resampler {

// ============================================================================
// Precision Strategy Templates
// ============================================================================
// These strategy structs control the precision used during filter coefficient
// generation. Each strategy is self-contained with all functions needed for
// coefficient calculation:
//
//   - calc_t: The floating-point type used for intermediate calculations
//   - IS_QUAD_PRECISION: Whether this strategy uses quad precision
//   - bessel_i0(x): Modified Bessel function of first kind, order zero
//   - sinc(x): Normalized sinc function sin(πx)/(πx)
//   - evaluate_window(n, length, beta, bessel_denom): Kaiser window at position n
// ============================================================================

// ============================================================================
// Named Constants
// ============================================================================
// Loop bounds, polynomial-table indices, and Q-format shift amounts used by
// the bessel_i0()/evaluate_window() implementations below.
// ============================================================================

// Single-precision Bessel I0 tail-branch polynomial (|x| > 15): 9 terms,
// indices 0..8.
constexpr int BESSEL_I0_TAIL_POLY_TERMS_SINGLE = 9;

// Double/quad-precision Bessel I0 polynomials (Advanpix minimax coefficients).
// Near-branch (|x| < 7.75): 17 terms, indices 0..16.
constexpr int BESSEL_I0_NEAR_POLY_LOOP_START = 15;  // second-highest near-branch index
// Far-branch (|x| >= 7.75): 23 terms, indices 0..22.
constexpr int BESSEL_I0_FAR_POLY_HIGHEST_INDEX = 22;
constexpr int BESSEL_I0_FAR_POLY_LOOP_START = 21;  // second-highest far-branch index

// Q31 fractional bits used when converting alpha to Q31 format in the
// fixed-point Kaiser window.
constexpr int KAISER_ALPHA_Q31_BITS = 31;

// Beta-selection thresholds for the fixed-point Kaiser table strategy
// (supported tables are centered on beta ~= 6, 8, 10, 12).
constexpr int BETA8_SELECT_THRESHOLD = 9;
constexpr int BETA10_SELECT_THRESHOLD = 11;

// Beta match tolerance for the float/double Kaiser table strategies. The
// tables are exact only for their own beta, so anything off the 6/8/10/12
// grid falls back to the computed window instead of silently substituting a
// neighboring table. resolve_design() emits grid-exact betas when table
// generation is selected (DesignConstraints::table_beta_grid).
constexpr double TABLE_BETA_MATCH_TOLERANCE = 0.01;

// Q30 fractional bits used when converting a Q30 fixed-point value to float.
constexpr int Q30_FRACTIONAL_BITS = 30;

// ============================================================================
// Single Precision Strategy
// ============================================================================
// Uses float arithmetic throughout. Fastest but least precise.
// Suitable for resource-constrained embedded systems where speed matters more
// than coefficient precision.

struct SinglePrecisionStrategy {
    using calc_t = float;
    static constexpr bool IS_QUAD_PRECISION = false;

    // Modified Bessel function I0 - single precision
    // Lower order polynomial sufficient for float precision
    static calc_t bessel_i0(calc_t x) {
        x = std::fabs(x);

        if (x <= 15.0F) {
            static const float P[] = {
                1.0000000000000000F, 3.5156229000000000F, 3.0899424000000000F, 1.2067492000000000F,
                0.2659732000000000F, 0.0360768000000000F, 0.0045813000000000F,
            };

            float t = x / 3.75F;
            float t2 = t * t;

            float result = P[0];
            float tn = t2;
            for (int i = 1; i < 7; i++) {
                result += P[i] * tn;
                tn *= t2;
            }
            return result;
        }
        static const float P[] = {
            0.39894228F,  0.01328592F, 0.00225319F,  -0.00157565F, 0.00916281F,
            -0.02057706F, 0.02635537F, -0.01647633F, 0.00392377F,
        };

        float z = 15.0F / x;
        float result = P[0];
        float zn = z;
        for (int i = 1; i < BESSEL_I0_TAIL_POLY_TERMS_SINGLE; i++) {
            result += P[i] * zn;
            zn *= z;
        }
        return std::exp(x) * result / std::sqrt(x);
    }

    // Normalized sinc function: sin(πx)/(πx)
    static calc_t sinc(calc_t x) {
        if (std::fabs(x) < 1e-7F) {
            return 1.0F;
        }
        return std::sin(static_cast<float>(M_PI) * x) / (static_cast<float>(M_PI) * x);
    }

    // Kaiser window evaluation at position n
    // Requires pre-computed bessel denominator for efficiency
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t bessel_denom) {
        const int half_length = (length - 1) / 2;
        float alpha = static_cast<float>(n - half_length) / half_length;
        float inner = 1.0F - alpha * alpha;
        if (inner < 0.0F) {
            inner = 0.0F;
        }
        float arg = beta * std::sqrt(inner);
        return bessel_i0(arg) / bessel_denom;
    }
};

// ============================================================================
// Double Precision Strategy
// ============================================================================
// Uses double arithmetic throughout. Good balance of precision and speed.
// Recommended for most desktop/server applications.

struct DoublePrecisionStrategy {
    using calc_t = double;
    static constexpr bool IS_QUAD_PRECISION = false;

    // Modified Bessel function I0 - double precision
    // Based on Advanpix minimax polynomial approximations (2015)
    // Peak relative error: 2.38ε for |x| < 7.75, 0.72ε for |x| >= 7.75
    static calc_t bessel_i0(calc_t x) {
        x = std::abs(x);

        if (x < 7.75) {
            static const double P[] = {
                1.0000000000000000000000801e+00, 2.4999999999999999999629693e-01,
                2.7777777777777777805664954e-02, 1.7361111111111110294015271e-03,
                6.9444444444444568581891535e-05, 1.9290123456788994104574754e-06,
                3.9367598891475388547279760e-08, 6.1511873265092916275099070e-10,
                7.5940584360755226536109511e-12, 7.5940582595094190098755663e-14,
                6.2760839879536225394314453e-16, 4.3583591008893599099577755e-18,
                2.5791926805873898803749321e-20, 1.3141332422663039834197910e-22,
                5.9203280572170548134753422e-25, 2.0732014503197852176921968e-27,
                1.1497640034400735733456400e-29,
            };

            double y = x * 0.5;
            double y2 = y * y;

            double p_val = P[16];
            for (int i = BESSEL_I0_NEAR_POLY_LOOP_START; i >= 0; i--) {
                p_val = p_val * y2 + P[i];
            }
            return y2 * p_val + 1.0;
        }
        static const double P[] = {
            3.9894228040143265335649948e-01,  4.9867785050353992900698488e-02,
            2.8050628884163787533196746e-02,  2.9219501690198775910219311e-02,
            4.4718622769244715693031735e-02,  9.4085204199017869159183831e-02,
            -1.0699095472110916094973951e-01, 2.2725199603010833194037016e+01,
            -1.0026890180180668595066918e+03, 3.1275740782277570164423916e+04,
            -5.9355022509673600842060002e+05, 2.6092888649549172879282592e+06,
            2.3518420447411254516178388e+08,  -8.9270060370015930749184222e+09,
            1.8592340458074104721496236e+11,  -2.6632742974569782078420204e+12,
            2.7752144774934763122129261e+13,  -2.1323049786724612220362154e+14,
            1.1989242681178569338129044e+15,  -4.8049082153027457378879746e+15,
            1.3012646806421079076251950e+16,  -2.1363029690365351606041265e+16,
            1.6069467093441596329340754e+16,
        };

        double z = 1.0 / x;

        double p_val = P[BESSEL_I0_FAR_POLY_HIGHEST_INDEX];
        for (int i = BESSEL_I0_FAR_POLY_LOOP_START; i >= 0; i--) {
            p_val = p_val * z + P[i];
        }

        double exp_half = std::exp(x * 0.5);
        return exp_half * (exp_half * p_val / std::sqrt(x));
    }

    // Normalized sinc function: sin(πx)/(πx)
    static calc_t sinc(calc_t x) {
        if (std::abs(x) < 1e-10) {
            return 1.0;
        }
        return std::sin(M_PI * x) / (M_PI * x);
    }

    // Kaiser window evaluation at position n
    // Requires pre-computed bessel denominator for efficiency
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t bessel_denom) {
        const int half_length = (length - 1) / 2;
        double alpha = static_cast<double>(n - half_length) / half_length;
        double inner = 1.0 - alpha * alpha;
        if (inner < 0.0) {
            inner = 0.0;
        }
        double arg = beta * std::sqrt(inner);
        return bessel_i0(arg) / bessel_denom;
    }
};

// ============================================================================
// Table-Based Double Precision Strategy
// ============================================================================
// Uses pre-computed Kaiser window tables with cubic interpolation.
// Faster than computing Bessel I0 directly, with double precision output.

struct TableDoubleStrategy {
    using calc_t = double;
    static constexpr bool IS_QUAD_PRECISION = false;

    // Modified Bessel function I0 - delegates to DoublePrecisionStrategy
    static calc_t bessel_i0(calc_t x) {
        return DoublePrecisionStrategy::bessel_i0(x);
    }

    // Normalized sinc function: sin(πx)/(πx)
    static calc_t sinc(calc_t x) {
        if (std::abs(x) < 1e-10) {
            return 1.0;
        }
        return std::sin(M_PI * x) / (M_PI * x);
    }

    // Kaiser window evaluation using table lookup with cubic interpolation
    // Supports beta values of exactly 6.0, 8.0, 10.0, and 12.0
    // bessel_denom is used for fallback path with off-grid beta values
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t bessel_denom) {
        const int half_length = (length - 1) / 2;
        double alpha = static_cast<double>(n - half_length) / half_length;
        double abs_alpha = std::abs(alpha);

        // Select the table matching the beta value
        if (std::abs(beta - 6.0) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate(abs_alpha, kaiser_tables::KAISER_BETA6_TABLE,
                                              kaiser_tables::OVERSAMPLE_BETA6,
                                              kaiser_tables::TABLE_LENGTH_BETA6);
        }
        if (std::abs(beta - 8.0) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate(abs_alpha, kaiser_tables::KAISER_BETA8_TABLE,
                                              kaiser_tables::OVERSAMPLE_BETA8,
                                              kaiser_tables::TABLE_LENGTH_BETA8);
        }
        if (std::abs(beta - 10.0) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate(abs_alpha, kaiser_tables::KAISER_BETA10_TABLE,
                                              kaiser_tables::OVERSAMPLE_BETA10,
                                              kaiser_tables::TABLE_LENGTH_BETA10);
        }
        if (std::abs(beta - 12.0) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate(abs_alpha, kaiser_tables::KAISER_BETA12_TABLE,
                                              kaiser_tables::OVERSAMPLE_BETA12,
                                              kaiser_tables::TABLE_LENGTH_BETA12);
        }  // Fall back to double precision computation for off-grid beta
        return DoublePrecisionStrategy::evaluate_window(n, length, beta, bessel_denom);
    }
};

// ============================================================================
// Table-Based Float Precision Strategy
// ============================================================================
// Uses pre-computed Kaiser window tables with float interpolation.
// Most memory efficient, ideal for embedded platforms like ESP32.

struct TableFloatStrategy {
    using calc_t = float;
    static constexpr bool IS_QUAD_PRECISION = false;

    // Modified Bessel function I0 - delegates to SinglePrecisionStrategy
    static calc_t bessel_i0(calc_t x) {
        return SinglePrecisionStrategy::bessel_i0(x);
    }

    // Normalized sinc function: sin(πx)/(πx)
    static calc_t sinc(calc_t x) {
        if (std::fabs(x) < 1e-7F) {
            return 1.0F;
        }
        return std::sin(static_cast<float>(M_PI) * x) / (static_cast<float>(M_PI) * x);
    }

    // Kaiser window evaluation using float table lookup with cubic interpolation
    // Supports beta values of exactly 6.0, 8.0, 10.0, and 12.0
    // bessel_denom is used for fallback path with off-grid beta values
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t bessel_denom) {
        const int half_length = (length - 1) / 2;
        float alpha = static_cast<float>(n - half_length) / static_cast<float>(half_length);
        float abs_alpha = std::fabs(alpha);

        // Select the float table matching the beta value
        if (std::abs(beta - 6.0F) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate_f(abs_alpha, kaiser_tables::KAISER_BETA6_TABLE_F,
                                                kaiser_tables::OVERSAMPLE_BETA6,
                                                kaiser_tables::TABLE_LENGTH_BETA6);
        }
        if (std::abs(beta - 8.0F) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate_f(abs_alpha, kaiser_tables::KAISER_BETA8_TABLE_F,
                                                kaiser_tables::OVERSAMPLE_BETA8,
                                                kaiser_tables::TABLE_LENGTH_BETA8);
        }
        if (std::abs(beta - 10.0F) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate_f(abs_alpha, kaiser_tables::KAISER_BETA10_TABLE_F,
                                                kaiser_tables::OVERSAMPLE_BETA10,
                                                kaiser_tables::TABLE_LENGTH_BETA10);
        }
        if (std::abs(beta - 12.0F) < TABLE_BETA_MATCH_TOLERANCE) {
            return kaiser_tables::interpolate_f(abs_alpha, kaiser_tables::KAISER_BETA12_TABLE_F,
                                                kaiser_tables::OVERSAMPLE_BETA12,
                                                kaiser_tables::TABLE_LENGTH_BETA12);
        }  // Fall back to single precision computation for off-grid beta
        return SinglePrecisionStrategy::evaluate_window(n, length, beta, bessel_denom);
    }
};

// ============================================================================
// Table-Based Fixed-Point Precision Strategy
// ============================================================================
// Uses pre-computed Q30 Kaiser window tables with pure integer arithmetic.
// No floating-point operations required - ideal for embedded platforms without FPU.
// Currently supports only beta=6.0.

struct TableFixedPointStrategy {
    using calc_t = int32_t;  // Q30 format for coefficients
    static constexpr bool IS_QUAD_PRECISION = false;

    // Modified Bessel function I0 - not used for table-based approach
    // Returns 0 as a placeholder since we use pre-computed tables
    static calc_t bessel_i0(calc_t /* x */) {
        return 0;
    }

    // Normalized sinc function: sin(πx)/(πx) in Q30 format
    // Input: x in Q24 format (can represent values up to ±128)
    // Output: sinc(x) in Q30 format
    // Uses the improved 9th-order minimax polynomial for better precision
    static calc_t sinc(calc_t x_q24) {
        return fixed_point_sinc::sinc_minimax_q30(x_q24);
    }

    // Kaiser window evaluation using Q30 table lookup with linear interpolation
    // Supports beta=6.0, 8.0, 10.0, and 12.0
    // Input: n = sample position, length = total filter length
    //        beta = Kaiser parameter (as int32_t, represents actual beta value)
    //        bessel_denom = unused for table-based approach
    // Output: Kaiser window value in Q30 format
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t /* bessel_denom */) {
        const int half_length = (length - 1) / 2;

        // Compute alpha = (n - half_length) / half_length in Q31
        // alpha ∈ [-1, 1] for n ∈ [0, length-1]
        int64_t numerator = static_cast<int64_t>(n - half_length);
        int64_t alpha_q31_64 = (numerator << KAISER_ALPHA_Q31_BITS) / half_length;

        // Take absolute value (Kaiser window is symmetric)
        // Note: When alpha = -1.0, alpha_q31 = -2^31, and |alpha_q31| = 2^31
        // which overflows int32_t. Clamp to INT32_MAX (2^31 - 1) to avoid overflow.
        int64_t abs_alpha_q31_64 = alpha_q31_64 < 0 ? -alpha_q31_64 : alpha_q31_64;
        if (abs_alpha_q31_64 > INT32_MAX) {
            abs_alpha_q31_64 = INT32_MAX;
        }
        int32_t abs_alpha_q31 = static_cast<int32_t>(abs_alpha_q31_64);

        // Select appropriate Q30 table based on beta value
        // Beta is passed as int32_t representing the actual value
        if (beta <= 7) {
            // beta ≈ 6: use beta6 table (32x oversampling)
            return kaiser_tables::interpolate_kaiser_q30_cubic_32x(
                abs_alpha_q31, kaiser_tables::KAISER_BETA6_TABLE_Q30,
                kaiser_tables::TABLE_LENGTH_BETA6);
        }
        if (beta <= BETA8_SELECT_THRESHOLD) {
            // beta ≈ 8: use beta8 table (32x oversampling)
            return kaiser_tables::interpolate_kaiser_q30_cubic_32x(
                abs_alpha_q31, kaiser_tables::KAISER_BETA8_TABLE_Q30,
                kaiser_tables::TABLE_LENGTH_BETA8);
        }
        if (beta <= BETA10_SELECT_THRESHOLD) {
            // beta ≈ 10: use beta10 table (32x oversampling)
            return kaiser_tables::interpolate_kaiser_q30_cubic_32x(
                abs_alpha_q31, kaiser_tables::KAISER_BETA10_TABLE_Q30,
                kaiser_tables::TABLE_LENGTH_BETA10);
        }  // beta ≈ 12 or higher: use beta12 table (64x oversampling)
        return kaiser_tables::interpolate_kaiser_q30_cubic_64x(
            abs_alpha_q31, kaiser_tables::KAISER_BETA12_TABLE_Q30,
            kaiser_tables::TABLE_LENGTH_BETA12);
    }

    // Convenience method to convert Q30 result to float for comparison/output
    static float q30_to_float(calc_t value_q30) {
        return static_cast<double>(value_q30) / static_cast<double>(1 << Q30_FRACTIONAL_BITS);
    }

    // Convert float to Q24 for sinc input (can represent values up to ±128)
    static int32_t float_to_q24(float value) {
        return fixed_point_sinc::float_to_q24(value);
    }

    // ========================================
    // Q50 High-Precision Methods
    // ========================================
    // These methods use Q50 (64-bit) fixed-point for higher precision
    // coefficient generation. The Q50 format preserves full precision during
    // argument reduction in sin(πx), which is critical for large x values.

    // Convert double to Q50 for high-precision sinc input
    static int64_t float_to_q50(double value) {
        return fixed_point_sinc::float_to_q50(value);
    }

    // High-precision sinc using Q50 input
    // Input: x in Q50 format
    // Output: sinc(x) in Q30 format
    static calc_t sinc_q50(int64_t x_q50) {
        return fixed_point_sinc::sinc_minimax_q30_q50(x_q50);
    }
};

// ============================================================================
// Quad Precision Strategy (conditionally compiled)
// ============================================================================
// Uses 128-bit floating point via Boost.Multiprecision for highest precision.
// Useful for generating coefficients with minimal precision loss.

#ifdef RESAMPLER_ENABLE_QUAD_PRECISION

struct QuadPrecisionStrategy {
    using calc_t = double;  // Output type (internal calculations use float128)
    using quad_t = boost::multiprecision::cpp_bin_float_quad;
    static constexpr bool IS_QUAD_PRECISION = true;

    // Modified Bessel function I0 - double precision (for interface consistency)
    static calc_t bessel_i0(calc_t x) {
        return DoublePrecisionStrategy::bessel_i0(x);
    }

    // Modified Bessel function I0 - quad precision
    // Uses same polynomial coefficients but with 128-bit precision arithmetic
    static quad_t bessel_i0_quad(quad_t x) {
        using boost::multiprecision::abs;
        using boost::multiprecision::exp;
        using boost::multiprecision::sqrt;

        x = abs(x);

        if (x < quad_t("7.75")) {
            static const quad_t P[] = {
                quad_t("1.0000000000000000000000801e+00"),
                quad_t("2.4999999999999999999629693e-01"),
                quad_t("2.7777777777777777805664954e-02"),
                quad_t("1.7361111111111110294015271e-03"),
                quad_t("6.9444444444444568581891535e-05"),
                quad_t("1.9290123456788994104574754e-06"),
                quad_t("3.9367598891475388547279760e-08"),
                quad_t("6.1511873265092916275099070e-10"),
                quad_t("7.5940584360755226536109511e-12"),
                quad_t("7.5940582595094190098755663e-14"),
                quad_t("6.2760839879536225394314453e-16"),
                quad_t("4.3583591008893599099577755e-18"),
                quad_t("2.5791926805873898803749321e-20"),
                quad_t("1.3141332422663039834197910e-22"),
                quad_t("5.9203280572170548134753422e-25"),
                quad_t("2.0732014503197852176921968e-27"),
                quad_t("1.1497640034400735733456400e-29"),
            };

            quad_t y = x * quad_t("0.5");
            quad_t y2 = y * y;

            quad_t p_val = P[16];
            for (int i = BESSEL_I0_NEAR_POLY_LOOP_START; i >= 0; i--) {
                p_val = p_val * y2 + P[i];
            }
            return y2 * p_val + quad_t(1);
        }
        static const quad_t P[] = {
            quad_t("3.9894228040143265335649948e-01"),  quad_t("4.9867785050353992900698488e-02"),
            quad_t("2.8050628884163787533196746e-02"),  quad_t("2.9219501690198775910219311e-02"),
            quad_t("4.4718622769244715693031735e-02"),  quad_t("9.4085204199017869159183831e-02"),
            quad_t("-1.0699095472110916094973951e-01"), quad_t("2.2725199603010833194037016e+01"),
            quad_t("-1.0026890180180668595066918e+03"), quad_t("3.1275740782277570164423916e+04"),
            quad_t("-5.9355022509673600842060002e+05"), quad_t("2.6092888649549172879282592e+06"),
            quad_t("2.3518420447411254516178388e+08"),  quad_t("-8.9270060370015930749184222e+09"),
            quad_t("1.8592340458074104721496236e+11"),  quad_t("-2.6632742974569782078420204e+12"),
            quad_t("2.7752144774934763122129261e+13"),  quad_t("-2.1323049786724612220362154e+14"),
            quad_t("1.1989242681178569338129044e+15"),  quad_t("-4.8049082153027457378879746e+15"),
            quad_t("1.3012646806421079076251950e+16"),  quad_t("-2.1363029690365351606041265e+16"),
            quad_t("1.6069467093441596329340754e+16"),
        };

        quad_t z = quad_t(1) / x;

        quad_t p_val = P[BESSEL_I0_FAR_POLY_HIGHEST_INDEX];
        for (int i = BESSEL_I0_FAR_POLY_LOOP_START; i >= 0; i--) {
            p_val = p_val * z + P[i];
        }

        quad_t exp_half = exp(x * quad_t("0.5"));
        return exp_half * (exp_half * p_val / sqrt(x));
    }

    // Double precision sinc (used for calc_t output)
    static calc_t sinc(calc_t x) {
        return DoublePrecisionStrategy::sinc(x);
    }

    // Kaiser window evaluation - returns double but uses quad precision internally
    // The incoming bessel_denom (double) is ignored: we recompute the denominator
    // in quad precision from beta so both numerator and denominator stay at full
    // precision. Dividing a quad numerator by a double denom loses accuracy on
    // very long filters / high beta (LUDICROUS).
    static calc_t evaluate_window(int n, int length, calc_t beta, calc_t /*bessel_denom*/) {
        using boost::multiprecision::sqrt;

        const quad_t beta_q = quad_t(beta);
        const quad_t denominator = bessel_i0_quad(beta_q);
        const int half_length = (length - 1) / 2;
        quad_t alpha = quad_t(n - half_length) / quad_t(half_length);
        quad_t inner = quad_t(1) - alpha * alpha;
        if (inner < quad_t(0)) {
            inner = quad_t(0);
        }
        quad_t arg = beta_q * sqrt(inner);
        return static_cast<double>(bessel_i0_quad(arg) / denominator);
    }
};

#endif  // RESAMPLER_ENABLE_QUAD_PRECISION

// ============================================================================
// Default Precision Strategy
// ============================================================================
// Platform-specific default:
// - ESP32/embedded: TableFloatStrategy (memory efficient)
// - Desktop: DoublePrecisionStrategy (QuadPrecisionStrategy is opt-in)

#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ESP32S3)
using DefaultPrecisionStrategy = TableFloatStrategy;
#else
using DefaultPrecisionStrategy = DoublePrecisionStrategy;
#endif

}  // namespace micro_resampler

#endif  // PRECISION_STRATEGY_H
