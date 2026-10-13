// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file fixed_generic.h
/// @brief Portable fixed-point arithmetic macros and helpers for Q-format math
///
/// Provides platform-independent implementations of the saturating and rounding
/// shift, multiply, and multiply-accumulate macros (for example MULT16_16,
/// MAC16_16, MULT32_32_Q32, PSHR32, PSHR64) used throughout the resampler,
/// together with Q-format scale constants and the float_to_q / q_to_float
/// conversion templates. On Xtensa targets, xtensa/fixed_lx6.h is included
/// afterward to override select macros with hardware-specific versions.

#ifndef FIXED_GENERIC_H
#define FIXED_GENERIC_H

#include <cmath>
#include <cstdint>
#include <limits>

namespace micro_resampler {

// Platform detection for the exact 64-bit high-word multiply in MULT32_32_Q32. RISC-V is
// included even at XLEN 32: with the M extension (every Espressif RISC-V core) GCC lowers
// (int64)a * b >> 32 to a single mulh, so the exact high word is also the cheap one, and it
// matches the Xtensa mulsh override and 64-bit hosts bit for bit; without M it costs one
// __muldi3 call, fewer than the decomposition's three multiplies
#if defined(__x86_64__) || defined(__LP64__) || defined(_WIN64) || defined(__mips) || \
    defined(__riscv)
#define USE_INT64 1
#else
#define USE_INT64 0
#endif

/** 32-bit add that wraps on overflow, as the hardware and the asm kernels do, instead of being
 * undefined. Partial sums that leave the int32 range still reach the right final value when
 * that value fits; a final value outside the range is still wrong (wrapped), so paths that can
 * exceed it must widen and saturate themselves. */
#define ADD32(a, b)                                                        \
    (static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(a)) + \
                          static_cast<uint32_t>(static_cast<int32_t>(b))))

/** Change a 32-bit value into a 16-bit value. The value is assumed to fit in 16-bit, otherwise the
 * result is undefined */
#define EXTRACT16(x) (static_cast<int16_t>(x))
/** Change a 16-bit value into a 32-bit value */
#define EXTEND32(x) (static_cast<int32_t>(x))
#define EXTEND64(x) (static_cast<int64_t>(x))

/** Left shift, well-defined for negative values (C++17 leaves `negative << n` undefined) */
#define SHL32(a, shift) \
    (static_cast<int32_t>(static_cast<uint32_t>(static_cast<int32_t>(a)) << (shift)))
#define SHR32(a, shift) ((a) >> (shift))

/** 32-bit arithmetic shift right with rounding-to-nearest instead of rounding down */
#define PSHR32(a, shift) (SHR32((a) + ((EXTEND32(1) << ((shift)) >> 1)), shift))

static inline int32_t saturating_round_shift_generic(int32_t x, int shift) {
    if (shift == 0) {
        return x;
    }

    // Calculate rounding offset: 2^(shift-1)
    int32_t rounding_offset = static_cast<int32_t>(1) << (shift - 1);

    // Calculate saturation thresholds
    int32_t max_before_overflow = std::numeric_limits<int32_t>::max() - rounding_offset;

    // Check for saturation
    if (x > max_before_overflow) {
        return std::numeric_limits<int32_t>::max() >> shift;
    }

    // Safe to round and shift
    return (x + rounding_offset) >> shift;
}
#define PSHR32_SAFE(a, shift) saturating_round_shift_generic(a, shift)

/** 64-bit arithmetic shift right with rounding-to-nearest instead of rounding down */
#define PSHR64(a, shift) (SHR64((a) + ((EXTEND64(1) << ((shift)) >> 1)), shift))

/** Shift by a and round-to-nearest 32-bit value. Result is a 16-bit value */
#define ROUND16(x, a) (EXTRACT16(PSHR32((x), (a))))
/** Shift by a and round-to-nearest 32-bit value. Result is a saturated 16-bit value */
#define SROUND16(x, a) SATURATE16(PSHR32(x, a));
#define SROUND16_SAFE(x, a) SATURATE16_SAFE(PSHR32_SAFE(x, a));

#define ROUND16_64(x, a) (EXTRACT16(PSHR64((x), (a))))
#define SROUND16_64(x, a) SATURATE16(PSHR64(x, a));

#define SATURATE(x, a) (((x) > (a) ? (a) : (x) < -(a) ? -(a) : (x)))

static inline int16_t saturate16(int32_t x) {
    return x > INT16_MAX ? INT16_MAX : x < -32768 ? -32768 : static_cast<int16_t>(x);
}
#define SATURATE16(x) saturate16(x)

// ============================================================================
// Q-format scale constants for accumulator-to-float conversion
// ============================================================================
// Q15: 1.0 = 2^15 = 32768
// Q23: 1.0 = 2^23 = 8388608
// Q30: 1.0 = 2^30 = 1073741824
// Q31: 1.0 = 2^31 = 2147483648
// Q62: 1.0 = 2^62 = 4611686018427387904
// ============================================================================

constexpr double Q15_SCALE = 32768.0;
constexpr double Q23_SCALE = 8388608.0;
constexpr double Q30_SCALE = 1073741824.0;
constexpr double Q31_SCALE = 2147483648.0;
constexpr double Q62_SCALE = 4611686018427387904.0;

constexpr double Q15_INV_SCALE = 1.0 / Q15_SCALE;
constexpr double Q23_INV_SCALE = 1.0 / Q23_SCALE;
constexpr double Q30_INV_SCALE = 1.0 / Q30_SCALE;
constexpr double Q31_INV_SCALE = 1.0 / Q31_SCALE;
constexpr double Q62_INV_SCALE = 1.0 / Q62_SCALE;

// Maximum usable fractional bits for a signed 32-bit Q-format value (1 bit reserved for sign)
constexpr int MAX_Q_FRACTIONAL_BITS = 31;

// Generalized float to Q-format conversion
// FRACTIONAL_BITS: number of fractional bits (e.g., 31 for Q31, 29 for Q29, 15 for Q15)
// Returns int32_t value in specified Q-format
template <int FRACTIONAL_BITS>
static inline int32_t float_to_q(double x) {
    static_assert(FRACTIONAL_BITS > 0 && FRACTIONAL_BITS <= MAX_Q_FRACTIONAL_BITS,
                  "FRACTIONAL_BITS must be in range [1, 31]");

    // Calculate range: for Qn format, range is [-2^(32-n-1), 2^(32-n-1))
    // For Q31: [-1.0, 1.0), Q29: [-4.0, 4.0), Q15: [-65536.0, 65536.0)
    constexpr int INTEGER_BITS = 32 - FRACTIONAL_BITS - 1;  // -1 for sign bit
    constexpr double MAX_VALUE = (1 << INTEGER_BITS);       // Upper bound (exclusive)
    constexpr double MIN_VALUE = -(1 << INTEGER_BITS);      // Lower bound (inclusive)
    constexpr double SCALE = (1LL << FRACTIONAL_BITS);

    // Saturate to valid range
    if (x >= MAX_VALUE) {
        return INT32_MAX;
    }
    if (x <= MIN_VALUE) {
        return INT32_MIN;
    }

    return static_cast<int32_t>(std::round(x * SCALE));
}

#define FLOAT_TO_Q15(x) float_to_q<15>(x)
#define FLOAT_TO_Q29(x) float_to_q<29>(x)
#define FLOAT_TO_Q31(x) float_to_q<31>(x)

// Generalized Q-format to float conversion
// FRACTIONAL_BITS: number of fractional bits (e.g., 31 for Q31, 29 for Q29, 15 for Q15)
// Returns double value from specified Q-format
template <int FRACTIONAL_BITS>
static inline double q_to_float(int32_t x) {
    static_assert(FRACTIONAL_BITS > 0 && FRACTIONAL_BITS <= MAX_Q_FRACTIONAL_BITS,
                  "FRACTIONAL_BITS must be in range [1, 31]");
    constexpr double SCALE = (1LL << FRACTIONAL_BITS);
    return static_cast<double>(x) / SCALE;
}

#define Q15_TO_FLOAT(x) q_to_float<15>(x)

/** This shift macro should be used internal to this file only **/
#define SHR(a, shift) ((a) >> (shift))

/** Multiply a 16-bit signed value by a 16-bit unsigned value. The result is a 32-bit signed value
 */
#define MULT16_16SU(a, b) \
    (static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int32_t>(static_cast<uint16_t>(b)))

/** 16x16 multiplication where the result fits in 32 bits */
#define MULT16_16(a, b)                                \
    ((static_cast<int32_t>(static_cast<int16_t>(a))) * \
     (static_cast<int32_t>(static_cast<int16_t>(b))))

/** 32x32 multiplication, followed by a 32-bit shift right. Results fits in 32 bits. The
 * USE_INT64 form is the exact high word; the 16-bit decomposition fallback drops the low x low
 * product and floors the cross terms separately, so it can differ from it in the last bit */
#if USE_INT64
#define MULT32_32_Q32(a, b) \
    (static_cast<int32_t>(SHR(static_cast<int64_t>(a) * static_cast<int64_t>(b), 32)))
#else
#define MULT32_32_Q32(a, b)                                              \
    ADD32(ADD32(MULT16_16(SHR((a), 16), SHR((b), 16)),                   \
                SHR(MULT16_16SU(SHR((a), 16), ((b) & 0x0000ffff)), 16)), \
          SHR(MULT16_16SU(SHR((b), 16), ((a) & 0x0000ffff)), 16))
#endif

/** 16x16 multiply-add where the result fits in 32 bits */
#define MAC16_16(c, a, b) (ADD32((c), MULT16_16((a), (b))))

/** 16x16 mulitply-shift-add where the result fits in 32 bits */
#define MSAC16_16(c, a, b, shift) (ADD32((c), PSHR32(MULT16_16(a, b), shift)))

// ============================================================================
// 64-bit operations for Q31 coefficient mode
// ============================================================================

/** 64-bit add that wraps on overflow instead of being undefined (same caveat as ADD32) */
#define ADD64(a, b)                                                        \
    (static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(a)) + \
                          static_cast<uint64_t>(static_cast<int64_t>(b))))
#define SHL64(a, shift) \
    (static_cast<int64_t>(static_cast<uint64_t>(static_cast<int64_t>(a)) << (shift)))
#define SHR64(a, shift) (static_cast<int64_t>(a) >> (shift))

/** 16x32 multiplication where the result fits in 64 bits */
#define MULT16_32(a, b) \
    (static_cast<int64_t>(static_cast<int16_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)))

/** 16x32 multiply-add where the result fits in 64 bits */
#define MAC16_32(c, a, b) (ADD64((c), MULT16_32((a), (b))))

/** 16x32 multiply-shift-add where the result fits in 64 bits */
#define MSAC16_32(c, a, b, shift) (ADD64((c), SHR64(MULT16_32(a, b), shift)))

/** 32x32 multiplication, result is 64 bits */
#define MULT32_32(a, b) \
    (static_cast<int64_t>(static_cast<int32_t>(a)) * static_cast<int64_t>(static_cast<int32_t>(b)))

/** 32x32 multiplication, followed by a 64-bit shift right. Result fits in 64 bits */
#define MULT32_32_Q64(a, b, shift) (SHR64(MULT32_32(a, b), shift))

// Xtensa-specific optimizations (must come after generic definitions so #undef works)
#ifdef __xtensa__
#include "xtensa/fixed_lx6.h"
#endif

}  // namespace micro_resampler

#endif
