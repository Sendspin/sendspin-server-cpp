// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file formats.h
/// @brief Coefficient format traits and sample conversion helpers for the resampler
///
/// Defines the CoeffFormat traits structs (Q15Format, Q31Format, Q31LiteFormat,
/// Float32Format, Float64Format) that select the resampler's internal arithmetic
/// precision, plus the SampleConverter templates and fixed-point saturation,
/// shift, and TPDF dither helpers used to convert between external sample
/// formats and each CoeffFormat's processing/accumulator types

#ifndef FORMATS_H
#define FORMATS_H

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace micro_resampler {

// Phase interpolation precision (10-bit blend factor)
static constexpr int BLEND_FACTOR_BITS = 10;

// ============================================================================
// Coefficient Format Type Definitions
// ============================================================================
// These define the types and precision for different coefficient processing modes.
// The MicroResamplerImpl template uses these traits for compile-time selection
// of arithmetic precision.
// ============================================================================

// ----------------------------------------------------------------------------
// Q15 Fixed-Point Format
// ----------------------------------------------------------------------------
// Uses 16-bit coefficients (Q15), 32-bit accumulator (Q30)
// Best for embedded systems with limited memory and no floating-point unit
// Provides ~13-14 effective bits precision (81-88 dB SNR)
struct Q15Format {
    using coefficient_t = int16_t;
    using processing_t = int32_t;
    using accumulator_t = int32_t;
    using sample_t = int16_t;  // Audio samples match coefficient size

    static constexpr int PRECISION_BITS = 15;
    static constexpr bool USE_FLOAT = false;
};

// ----------------------------------------------------------------------------
// Q31 Fixed-Point Format
// ----------------------------------------------------------------------------
// Uses 32-bit coefficients (Q31) with 32-bit samples, 64-bit accumulator (Q62)
// Higher precision than Q15, but slower and uses more memory
// Provides ~15-16 effective bits precision
struct Q31Format {
    using coefficient_t = int32_t;
    using processing_t = int64_t;
    using accumulator_t = int64_t;
    using sample_t =
        int32_t;  // Audio samples are Q31 format (full 32-bit range representing [-1.0, 1.0))

    static constexpr int PRECISION_BITS = 31;
    static constexpr bool USE_FLOAT = false;
};

// ----------------------------------------------------------------------------
// Q31 Lite Fixed-Point Format (32-bit accumulator)
// ----------------------------------------------------------------------------
// Uses 32-bit coefficients (Q31) with 32-bit samples, but 32-bit accumulator
// Suitable for embedded systems without native 64-bit operations
// MAC uses MULT32_32_Q32 which returns high 32 bits (approximately Q30)
struct Q31LiteFormat {
    using coefficient_t = int32_t;
    using processing_t = int32_t;
    using accumulator_t = int32_t;  // Q30 format (high word of Q62)
    using sample_t = int32_t;       // Audio samples are Q31 format

    static constexpr int PRECISION_BITS = 31;
    static constexpr bool USE_FLOAT = false;
};

// ----------------------------------------------------------------------------
// Double-Precision Floating-Point Format
// ----------------------------------------------------------------------------
// Uses double precision floating-point for all operations
// Best for reference implementation, debugging, and systems with FPU
// No quantization noise, highest precision
struct Float64Format {
    using coefficient_t = double;
    using processing_t = double;
    using accumulator_t = double;
    using sample_t = double;  // Audio samples use double precision

    static constexpr int PRECISION_BITS = 0;  // Not applicable for float
    static constexpr bool USE_FLOAT = true;
};

// ----------------------------------------------------------------------------
// Single-Precision Floating-Point Format
// ----------------------------------------------------------------------------
// Uses single precision (32-bit) floating-point for all operations
// Faster than Float64Format on systems with single-precision FPU
// Less precision than double, but suitable for many audio applications
struct Float32Format {
    using coefficient_t = float;
    using processing_t = float;
    using accumulator_t = float;
    using sample_t = float;

    static constexpr int PRECISION_BITS = 0;  // Not applicable for float
    static constexpr bool USE_FLOAT = true;
};

// ============================================================================
// Forward Declarations for Conversion Infrastructure
// ============================================================================
// The SampleConverter structs below rely on utility functions from
// detail/fixed_generic.h and detail/rng_utils.h (internal headers).
// These are included by files that use SampleConverters.
// ============================================================================

// Forward declarations - actual implementations are in internal headers
// When using SampleConverters, include "detail/fixed_generic.h" and "detail/rng_utils.h"

// Scale constants - inline variables to avoid ODR violations. The K_ prefix
// keeps them distinct from fixed_generic.h's identically-valued Q*_SCALE
// constants so both headers can be included in the same translation unit.
inline constexpr double K_Q15_SCALE = 32768.0;
inline constexpr double K_Q23_SCALE = 8388608.0;
inline constexpr double K_Q30_SCALE = 1073741824.0;
inline constexpr double K_Q31_SCALE = 2147483648.0;
inline constexpr double K_Q62_SCALE = 4611686018427387904.0;

inline constexpr double K_Q15_INV_SCALE = 1.0 / K_Q15_SCALE;
inline constexpr double K_Q23_INV_SCALE = 1.0 / K_Q23_SCALE;
inline constexpr double K_Q30_INV_SCALE = 1.0 / K_Q30_SCALE;
inline constexpr double K_Q31_INV_SCALE = 1.0 / K_Q31_SCALE;
inline constexpr double K_Q62_INV_SCALE = 1.0 / K_Q62_SCALE;

// Saturation bounds - maximum representable positive value for the Q-formats
// whose bit width isn't a power of two (so the literal wouldn't already read
// clearly on its own). Q15/Q31 bounds use INT16_MAX/INT32_MAX from <cstdint>
// directly instead, since those are exact matches for the standard limits.
inline constexpr int32_t K_Q23_MAX = 8388607;  // 2^23 - 1

// Composite right-shift amounts for extracting a narrower Q-format value
// directly out of the wide Q62 accumulator_t used by Q31Format.
// shift = 62 - target format's fractional bit count.
inline constexpr int K_Q62_TO_Q23_SHIFT = 39;  // 62 - 23 (Q23 converters' PRECISION_BITS)
inline constexpr int K_Q62_TO_Q15_SHIFT = 47;  // 62 - 15 (Q15Format::PRECISION_BITS)

// Saturation functions (inline to avoid ODR violations)
inline int16_t saturate16_formats(int32_t x) {
    return x > INT16_MAX ? INT16_MAX : x < -32768 ? -32768 : static_cast<int16_t>(x);
}

inline int16_t saturate16_64_formats(int64_t x) {
    return x > INT16_MAX ? INT16_MAX : x < -32768 ? -32768 : static_cast<int16_t>(x);
}

inline int32_t saturate24_formats(int32_t x) {
    return x > K_Q23_MAX ? K_Q23_MAX : x < -8388608 ? -8388608 : x;
}

inline int32_t saturate24_64_formats(int64_t x) {
    return x > K_Q23_MAX ? K_Q23_MAX : x < -8388608 ? -8388608 : static_cast<int32_t>(x);
}

inline int32_t saturate32_formats(int64_t x) {
    return x > INT32_MAX ? INT32_MAX : x < INT32_MIN ? INT32_MIN : static_cast<int32_t>(x);
}

// Saturating 32-bit add: one wrapping add plus a sign test, instead of widening to 64 bits.
// Overflow happened iff a and b share a sign that the wrapped sum does not.
inline int32_t add_sat32_formats(int32_t a, int32_t b) {
    const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
    if (((a ^ sum) & (b ^ sum)) < 0) {
        return a < 0 ? INT32_MIN : INT32_MAX;
    }
    return sum;
}

// Left shift that is well-defined for negative values: C++17 makes `negative << n` undefined
// (C++20 defines it as exactly this two's-complement result). Compiles to a single shift.
inline int32_t shl32_formats(int32_t x, int shift) {
    return static_cast<int32_t>(static_cast<uint32_t>(x) << shift);
}

inline int32_t saturate_shl32_formats(int32_t x, int shift) {
    int32_t max_safe = INT32_MAX >> shift;
    int32_t min_safe = INT32_MIN >> shift;
    if (x > max_safe) {
        return INT32_MAX;
    }
    if (x < min_safe) {
        return INT32_MIN;
    }
    return shl32_formats(x, shift);
}

// 32-bit arithmetic shift right with rounding-to-nearest
inline int32_t pshr32_formats(int32_t a, int shift) {
    return (a + (static_cast<int32_t>(1) << (shift - 1))) >> shift;
}

// Safe version that saturates before adding rounding offset to prevent overflow
inline int32_t pshr32_safe_formats(int32_t x, int shift) {
    if (shift == 0) {
        return x;
    }
    int32_t rounding_offset = static_cast<int32_t>(1) << (shift - 1);
    int32_t max_before_overflow = INT32_MAX - rounding_offset;
    if (x > max_before_overflow) {
        return INT32_MAX >> shift;
    }
    return (x + rounding_offset) >> shift;
}

// 64-bit arithmetic shift right with rounding-to-nearest
inline int64_t pshr64_formats(int64_t a, int shift) {
    return (a + (static_cast<int64_t>(1) << (shift - 1))) >> shift;
}

// Packed Q23 (24-bit) Read/Write Helpers
inline int32_t read_q23_packed_formats(const uint8_t* p) {
    // Reinterpret the top byte as signed so the 24-bit sample sign-extends into int32_t.
    // NOLINTNEXTLINE(bugprone-signed-char-misuse)
    int32_t sample = static_cast<int8_t>(p[2]);
    sample = shl32_formats(sample, 8) | p[1];
    sample = shl32_formats(sample, 8) | p[0];
    return sample;
}

inline void write_q23_packed_formats(uint8_t* p, int32_t sample) {
    p[0] = static_cast<uint8_t>(sample);
    p[1] = static_cast<uint8_t>(sample >> 8);
    p[2] = static_cast<uint8_t>(sample >> 16);
}

// Xorshift32 magic shift constants (Marsaglia's xorshift, 13/17/5 variant)
inline constexpr uint32_t K_XORSHIFT32_SHIFT_A = 13;
inline constexpr uint32_t K_XORSHIFT32_SHIFT_B = 17;

// Fast Xorshift32 PRNG for dithering
inline uint32_t xorshift32_formats(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << K_XORSHIFT32_SHIFT_A;
    x ^= x >> K_XORSHIFT32_SHIFT_B;
    x ^= x << 5;
    *state = x;
    return x;
}

// TPDF dither generation
inline int32_t generate_tpdf_dither_formats(uint32_t* state, int32_t half_lsb) {
    uint32_t r1 = xorshift32_formats(state);
    uint32_t r2 = xorshift32_formats(state);
    int32_t d1 = static_cast<int32_t>(r1 >> 16) - 32768;
    int32_t d2 = static_cast<int32_t>(r2 >> 16) - 32768;
    int32_t sum = d1 + d2;
    return (sum * half_lsb) >> 16;
}

// Optimized TPDF dither for fixed shift value
template <int SHIFT>
inline int32_t generate_tpdf_dither_fixed_formats(uint32_t* state) {
    static_assert(SHIFT > 0, "Shift must be positive");
    constexpr int32_t HALF_LSB = 1 << (SHIFT - 1);
    return generate_tpdf_dither_formats(state, HALF_LSB);
}

// Specialization for SHIFT=15 (most common case)
template <>
inline int32_t generate_tpdf_dither_fixed_formats<Q15Format::PRECISION_BITS>(uint32_t* state) {
    uint32_t r1 = xorshift32_formats(state);
    uint32_t r2 = xorshift32_formats(state);
    int32_t d1 = static_cast<int32_t>(r1 >> 16) - 32768;
    int32_t d2 = static_cast<int32_t>(r2 >> 16) - 32768;
    return (d1 + d2) >> 2;
}

// Specialization for SHIFT=16 (Q31->Q15 conversion)
template <>
inline int32_t generate_tpdf_dither_fixed_formats<16>(uint32_t* state) {
    constexpr int32_t HALF_LSB = 1 << 15;
    return generate_tpdf_dither_formats(state, HALF_LSB);
}

// Specialization for SHIFT=8 (Q23->Q15 conversion)
template <>
inline int32_t generate_tpdf_dither_fixed_formats<8>(uint32_t* state) {
    constexpr int32_t HALF_LSB = 1 << 7;
    return generate_tpdf_dither_formats(state, HALF_LSB);
}

// Float to Q31 in single precision, saturating. Bit-identical, for every non-NaN input, to
// scaling in double and converting back (NaN is undefined either way): multiplying a float by
// 2^31 only moves its exponent, every float of magnitude 2^24 or more is already an integer,
// and no float lies strictly between 2^31 - 128 and 2^31, so the double comparisons against
// INT32_MAX/INT32_MIN select exactly the same inputs. Keeps soft-float double routines out of
// the hot path on single-FPU targets.
inline int32_t float_to_q31_trunc_formats(float x) {
    const float scaled = x * static_cast<float>(K_Q31_SCALE);
    if (scaled >= static_cast<float>(K_Q31_SCALE)) {
        return INT32_MAX;
    }
    if (scaled <= -static_cast<float>(K_Q31_SCALE)) {
        return INT32_MIN;
    }
    return static_cast<int32_t>(scaled);  // Truncates toward zero
}

// As float_to_q31_trunc_formats(), but rounding to nearest (halfway cases away from zero)
inline int32_t float_to_q31_round_formats(float x) {
    const float scaled = x * static_cast<float>(K_Q31_SCALE);
    if (scaled >= static_cast<float>(K_Q31_SCALE)) {
        return INT32_MAX;
    }
    if (scaled <= -static_cast<float>(K_Q31_SCALE)) {
        return INT32_MIN;
    }
    return static_cast<int32_t>(std::lround(scaled));
}

// ============================================================================
// SampleConverter - Unified Input/Output Conversion
// ============================================================================
// These structs provide unified conversion methods for converting between
// sample formats and coefficient format processing types. Each struct is
// templated on CoeffFormat and Dithered flag, using if constexpr for
// compile-time dispatch.
// ============================================================================

// ----------------------------------------------------------------------------
// Q15 Sample Converter
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Q15SampleConverter {
    using sample_t = int16_t;
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 15;
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = false;

    // Convert input sample to processing type
    static processing_t to_processing(sample_t in, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            return in;  // Identity
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                             std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return shl32_formats(in, 16);  // Q15 << 16 = Q31
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            return static_cast<processing_t>(in) * static_cast<processing_t>(K_Q15_INV_SCALE);
        }
    }

    // Convert accumulator to output sample
    static sample_t from_accumulator(accumulator_t acc, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            // Q30 >> 15 = Q15 (the safe shift keeps a near-+2.0 accumulator from overflowing
            // the rounding add and wrapping to negative full scale)
            return saturate16_formats(pshr32_safe_formats(acc, Q15Format::PRECISION_BITS));
        } else if constexpr (std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            // Q30 >> 15 = Q15, with optional dithering
            if constexpr (Dithered) {
                // Clamp to +/-1.0 (Q30) before dithering so the dither and rounding adds stay
                // in 32 bits: near full scale, acc + dither would otherwise overflow int32 and
                // wrap. Bit-identical to adding in 64 bits, since any acc past +/-1.0 rounds
                // to a saturated output for every dither value (|dither| <= 2^14)
                constexpr int32_t Q30_ONE = static_cast<int32_t>(1) << 30;
                const int32_t clamped = acc > Q30_ONE ? Q30_ONE : acc < -Q30_ONE ? -Q30_ONE : acc;
                const int32_t dithered =
                    clamped + generate_tpdf_dither_fixed_formats<Q15Format::PRECISION_BITS>(rng);
                return saturate16_formats(pshr32_formats(dithered, Q15Format::PRECISION_BITS));
            } else {
                return saturate16_formats(pshr32_safe_formats(acc, Q15Format::PRECISION_BITS));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            // Q62 >> 47 = Q15
            if constexpr (Dithered) {
                int32_t q31 = saturate32_formats(pshr64_formats(acc, Q31Format::PRECISION_BITS));
                // Add in 64 bits: a saturated q31 + positive dither would wrap negative in int32
                int64_t dithered =
                    static_cast<int64_t>(q31) + generate_tpdf_dither_fixed_formats<16>(rng);
                int64_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_64_formats(rounded >> 16);
            } else {
                return saturate16_64_formats(pshr64_formats(acc, K_Q62_TO_Q15_SHIFT));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
            if constexpr (Dithered) {
                double scaled = acc * K_Q31_SCALE;
                if (scaled >= static_cast<double>(INT32_MAX)) {
                    scaled = static_cast<double>(INT32_MAX);
                }
                if (scaled <= static_cast<double>(INT32_MIN)) {
                    scaled = static_cast<double>(INT32_MIN);
                }
                int32_t q31 = static_cast<int32_t>(scaled);
                // Add in 64 bits: a saturated q31 + positive dither would wrap negative in int32
                int64_t dithered =
                    static_cast<int64_t>(q31) + generate_tpdf_dither_fixed_formats<16>(rng);
                int64_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_64_formats(rounded >> 16);
            } else {
                double scaled = acc * K_Q15_SCALE;
                if (scaled >= static_cast<double>(INT16_MAX)) {
                    return INT16_MAX;
                }
                if (scaled <= static_cast<double>(INT16_MIN)) {
                    return INT16_MIN;
                }
                return static_cast<sample_t>(std::lround(scaled));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            if constexpr (Dithered) {
                int32_t q31 = float_to_q31_trunc_formats(acc);
                // Add in 64 bits: a saturated q31 + positive dither would wrap negative in int32
                int64_t dithered =
                    static_cast<int64_t>(q31) + generate_tpdf_dither_fixed_formats<16>(rng);
                int64_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_64_formats(rounded >> 16);
            } else {
                float scaled = acc * static_cast<float>(K_Q15_SCALE);
                if (scaled >= static_cast<float>(INT16_MAX)) {
                    return INT16_MAX;
                }
                if (scaled <= static_cast<float>(INT16_MIN)) {
                    return INT16_MIN;
                }
                return static_cast<sample_t>(std::lround(scaled));
            }
        }
    }
};

// ----------------------------------------------------------------------------
// Q23 Sample Converter (32-bit container)
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Q23SampleConverter {
    using sample_t = int32_t;
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 23;
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = false;

    static processing_t to_processing(sample_t in, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            // Q23 >> 8 = Q15
            if constexpr (Dithered) {
                int32_t dithered = in + generate_tpdf_dither_fixed_formats<8>(rng);
                return static_cast<int16_t>(saturate16_formats(pshr32_safe_formats(dithered, 8)));
            } else {
                // Saturate: full-scale Q23 rounds up to 32768, which wraps to -32768 in int16
                return saturate16_formats(pshr32_safe_formats(in, 8));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                             std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return shl32_formats(in, 8);  // Q23 << 8 = Q31
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            return static_cast<processing_t>(in) * static_cast<processing_t>(K_Q23_INV_SCALE);
        }
    }

    static sample_t from_accumulator(accumulator_t acc, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            // Q30 >> 7 = Q23
            return saturate24_formats(pshr32_safe_formats(acc, 7));
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            // Q62 >> 39 = Q23
            return saturate24_64_formats(pshr64_formats(acc, K_Q62_TO_Q23_SHIFT));
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            constexpr int32_t Q23_MAX = static_cast<int32_t>(K_Q23_SCALE) - 1;
            constexpr int32_t Q23_MIN = -static_cast<int32_t>(K_Q23_SCALE);
            auto scaled = acc * static_cast<accumulator_t>(K_Q23_SCALE);
            if (scaled >= static_cast<accumulator_t>(Q23_MAX)) {
                return Q23_MAX;
            }
            if (scaled <= static_cast<accumulator_t>(Q23_MIN)) {
                return Q23_MIN;
            }
            return static_cast<sample_t>(std::lround(scaled));
        }
    }
};

// ----------------------------------------------------------------------------
// Q23 Packed Sample Converter (3-byte format)
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Q23PackedSampleConverter {
    using sample_t = uint8_t;  // Memory type (3 bytes per logical sample)
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 23;
    static constexpr int BYTES_PER_SAMPLE = 3;
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = true;

    // For packed formats, use to_processing_packed instead of to_processing
    static processing_t to_processing_packed(const uint8_t* bytes,
                                             [[maybe_unused]] uint32_t* rng = nullptr) {
        int32_t sample = read_q23_packed_formats(bytes);
        if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            // Q23 >> 8 = Q15, with optional dithering of the 8-bit truncation
            if constexpr (Dithered) {
                int32_t dithered = sample + generate_tpdf_dither_fixed_formats<8>(rng);
                return static_cast<int16_t>(saturate16_formats(pshr32_safe_formats(dithered, 8)));
            } else {
                // Saturate: full-scale Q23 rounds up to 32768, which wraps to -32768 in int16
                return saturate16_formats(pshr32_safe_formats(sample, 8));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                             std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return shl32_formats(sample, 8);
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            return static_cast<processing_t>(sample) * static_cast<processing_t>(K_Q23_INV_SCALE);
        }
    }

    static void from_accumulator_packed(uint8_t* dst, accumulator_t acc,
                                        [[maybe_unused]] uint32_t* rng = nullptr) {
        int32_t q23 = 0;
        if constexpr (std::is_same_v<CoeffFormat, Q15Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            q23 = saturate24_formats(pshr32_safe_formats(acc, 7));
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            q23 = saturate24_64_formats(pshr64_formats(acc, K_Q62_TO_Q23_SHIFT));
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            constexpr int32_t Q23_MAX = static_cast<int32_t>(K_Q23_SCALE) - 1;
            constexpr int32_t Q23_MIN = -static_cast<int32_t>(K_Q23_SCALE);
            auto scaled = acc * static_cast<accumulator_t>(K_Q23_SCALE);
            if (scaled >= static_cast<accumulator_t>(Q23_MAX)) {
                q23 = Q23_MAX;
            } else if (scaled <= static_cast<accumulator_t>(Q23_MIN)) {
                q23 = Q23_MIN;
            } else {
                q23 = static_cast<int32_t>(std::lround(scaled));
            }
        }
        write_q23_packed_formats(dst, q23);
    }
};

// ----------------------------------------------------------------------------
// Q31 Sample Converter
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Q31SampleConverter {
    using sample_t = int32_t;
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 31;
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = false;

    static processing_t to_processing(sample_t in, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return in;  // Identity
        } else if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            // Q31 >> 16 = Q15
            if constexpr (Dithered) {
                constexpr int32_t POS_THRESHOLD = INT32_MAX - (2 * INT16_MAX + 1);
                constexpr int32_t NEG_THRESHOLD = INT32_MIN + static_cast<int32_t>(K_Q15_SCALE);
                if (in > POS_THRESHOLD) {
                    return INT16_MAX;
                }
                if (in < NEG_THRESHOLD) {
                    return INT16_MIN;
                }
                int32_t dither = generate_tpdf_dither_fixed_formats<16>(rng);
                int32_t dithered = in + dither;
                int32_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_formats(rounded >> 16);
            } else {
                return static_cast<int16_t>(pshr32_safe_formats(in, 16));
            }
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            // Scaling by a power of two is exact, so converting first rounds only once, as the
            // double product converted to float did
            return static_cast<processing_t>(in) * static_cast<processing_t>(K_Q31_INV_SCALE);
        }
    }

    static sample_t from_accumulator(accumulator_t acc, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            // Q30 << 1 = Q31 (with saturation)
            return saturate_shl32_formats(acc, 1);
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            // Q62 >> 31 = Q31
            return saturate32_formats(pshr64_formats(acc, Q31Format::PRECISION_BITS));
        } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            return float_to_q31_round_formats(acc);
        } else if constexpr (CoeffFormat::USE_FLOAT) {
            double scaled = static_cast<double>(acc) * K_Q31_SCALE;
            if (scaled >= static_cast<double>(INT32_MAX)) {
                return INT32_MAX;
            }
            if (scaled <= static_cast<double>(INT32_MIN)) {
                return INT32_MIN;
            }
            return static_cast<sample_t>(std::llround(scaled));
        }
    }
};

// ----------------------------------------------------------------------------
// Float32 Sample Converter
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Float32SampleConverter {
    using sample_t = float;
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 0;  // N/A for floating-point
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = false;

    static processing_t to_processing(sample_t in, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            return in;  // Identity
        } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
            return static_cast<double>(in);
        } else if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            if constexpr (Dithered) {
                int32_t q31 = float_to_q31_trunc_formats(in);
                constexpr int32_t POS_THRESHOLD = INT32_MAX - (2 * INT16_MAX + 1);
                constexpr int32_t NEG_THRESHOLD = INT32_MIN + static_cast<int32_t>(K_Q15_SCALE);
                if (q31 > POS_THRESHOLD) {
                    return INT16_MAX;
                }
                if (q31 < NEG_THRESHOLD) {
                    return INT16_MIN;
                }
                int32_t dither = generate_tpdf_dither_fixed_formats<16>(rng);
                int32_t dithered = q31 + dither;
                int32_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_formats(rounded >> 16);
            } else {
                float scaled = in * static_cast<float>(K_Q15_SCALE);
                if (scaled >= static_cast<float>(INT16_MAX)) {
                    return INT16_MAX;
                }
                if (scaled <= static_cast<float>(INT16_MIN)) {
                    return INT16_MIN;
                }
                return static_cast<int16_t>(std::lround(scaled));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                             std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return float_to_q31_round_formats(in);
        }
    }

    static sample_t from_accumulator(accumulator_t acc, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            // Converting first rounds once and the power-of-two scale is exact, so this matches
            // the double product converted to float bit for bit (checked over every int32)
            return static_cast<float>(acc) * static_cast<float>(K_Q30_INV_SCALE);
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            // Stays in double: int64 -> float directly would round once where int64 -> double
            // -> float rounds twice, which can differ by one ulp
            return static_cast<float>(static_cast<double>(acc) * K_Q62_INV_SCALE);
        } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
            return static_cast<float>(acc);
        } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            return acc;
        }
    }
};

// ----------------------------------------------------------------------------
// Float64 Sample Converter
// ----------------------------------------------------------------------------
template <typename CoeffFormat, bool Dithered = false>
struct Float64SampleConverter {
    using sample_t = double;
    using processing_t = typename CoeffFormat::sample_t;
    using accumulator_t = typename CoeffFormat::accumulator_t;
    static constexpr int PRECISION_BITS = 0;  // N/A for floating-point
    static constexpr bool IS_DITHERED = Dithered;
    static constexpr bool IS_PACKED = false;

    static processing_t to_processing(sample_t in, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
            return in;  // Identity
        } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            return static_cast<float>(in);
        } else if constexpr (std::is_same_v<CoeffFormat, Q15Format>) {
            if constexpr (Dithered) {
                double scaled = in * K_Q31_SCALE;
                if (scaled >= static_cast<double>(INT32_MAX)) {
                    scaled = static_cast<double>(INT32_MAX);
                }
                if (scaled <= static_cast<double>(INT32_MIN)) {
                    scaled = static_cast<double>(INT32_MIN);
                }
                int32_t q31 = static_cast<int32_t>(scaled);
                constexpr int32_t POS_THRESHOLD = INT32_MAX - (2 * INT16_MAX + 1);
                constexpr int32_t NEG_THRESHOLD = INT32_MIN + static_cast<int32_t>(K_Q15_SCALE);
                if (q31 > POS_THRESHOLD) {
                    return INT16_MAX;
                }
                if (q31 < NEG_THRESHOLD) {
                    return INT16_MIN;
                }
                int32_t dither = generate_tpdf_dither_fixed_formats<16>(rng);
                int32_t dithered = q31 + dither;
                int32_t rounded = dithered + (1 << Q15Format::PRECISION_BITS);
                return saturate16_formats(rounded >> 16);
            } else {
                double scaled = in * K_Q15_SCALE;
                if (scaled >= static_cast<double>(INT16_MAX)) {
                    return INT16_MAX;
                }
                if (scaled <= static_cast<double>(INT16_MIN)) {
                    return INT16_MIN;
                }
                return static_cast<int16_t>(std::round(scaled));
            }
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format> ||
                             std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            double scaled = in * K_Q31_SCALE;
            if (scaled >= static_cast<double>(INT32_MAX)) {
                return INT32_MAX;
            }
            if (scaled <= static_cast<double>(INT32_MIN)) {
                return INT32_MIN;
            }
            return static_cast<int32_t>(std::round(scaled));
        }
    }

    static sample_t from_accumulator(accumulator_t acc, [[maybe_unused]] uint32_t* rng = nullptr) {
        if constexpr (std::is_same_v<CoeffFormat, Q15Format> ||
                      std::is_same_v<CoeffFormat, Q31LiteFormat>) {
            return acc * K_Q30_INV_SCALE;
        } else if constexpr (std::is_same_v<CoeffFormat, Q31Format>) {
            return static_cast<double>(acc) * K_Q62_INV_SCALE;
        } else if constexpr (std::is_same_v<CoeffFormat, Float64Format>) {
            return acc;
        } else if constexpr (std::is_same_v<CoeffFormat, Float32Format>) {
            return static_cast<double>(acc);
        }
    }
};

}  // namespace micro_resampler

#endif  // FORMATS_H
