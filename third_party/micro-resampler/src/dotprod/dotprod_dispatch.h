// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file dotprod_dispatch.h
/// @brief Dot product platform detection and dispatch logic
///
/// Defines the DOTPROD_PLATFORM_* macros from the target architecture,
/// includes the matching platform-specific backend header, and implements
/// compute(), routing each call to the appropriate SIMD or C++ fallback path

#ifndef DOTPROD_DISPATCH_H
#define DOTPROD_DISPATCH_H

// ============================================================================
// Platform Detection
// ============================================================================
// Detect the target platform and define the appropriate macro.
// Only one platform macro should be defined at a time.

#if defined(__XTENSA__)
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3)
#define DOTPROD_PLATFORM_XTENSA_S3 1
#else
#define DOTPROD_PLATFORM_XTENSA_MAC16 1
#endif
#elif defined(__riscv) && (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || \
                           defined(CONFIG_IDF_TARGET_ESP32S31) || defined(ESP32S31))
// The ESP32-S31 runs the same kernels: its xespv 2.1 extension is a superset of
// the P4's PIE (xesppie) -- the toolchain assembles every esp.* instruction these
// kernels use under the S31's -march.
#define DOTPROD_PLATFORM_RISCV_P4 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define DOTPROD_PLATFORM_NEON 1
#else
#define DOTPROD_PLATFORM_GENERIC 1
#endif

// ============================================================================
// Include Platform-Specific Headers
// ============================================================================

#if defined(DOTPROD_PLATFORM_XTENSA_S3)
#include "xtensa/q31lite.h"
#include "xtensa/simd_s3.h"
#elif defined(DOTPROD_PLATFORM_XTENSA_MAC16)
#include "xtensa/mac16.h"
#include "xtensa/q31lite.h"
#elif defined(DOTPROD_PLATFORM_RISCV_P4)
#include "riscv/pie_p4.h"
#elif defined(DOTPROD_PLATFORM_NEON)
#include "arm/neon.h"
#endif

// Always include C++ fallback and max tap extraction
#include "../detail/cycle_profile.h"
#include "dotprod_cpp.h"
#include "dotprod_maxtap.h"

namespace micro_resampler {
namespace dotprod {
namespace detail {

// ============================================================================
// Block Adaptive Q Dispatch (Q15 only)
// ============================================================================
// Tries SIMD for Q15 with up to 8 channels, otherwise falls back to C++.
// Note: Q31 and Float do NOT use block adaptive Q - they use fixed precision.
// Returns true when the selected kernel also applied the max-tap entries
// (the S3 kernels fold them into the accumulators), so the caller must skip
// apply_max_taps()

template <typename CoeffFormat>
bool compute_block_adaptive_q(const DotProdConfig<CoeffFormat>& config,
                              typename CoeffFormat::accumulator_t* output) {
    constexpr bool CAN_USE_SIMD = SIMDTraits<CoeffFormat>::AVAILABLE;
    constexpr bool IS_Q15 = std::is_same_v<CoeffFormat, Q15Format>;

    if constexpr (CAN_USE_SIMD && IS_Q15) {
        if (config.num_channels <= SIMDTraits<CoeffFormat>::MAX_CHANNELS) {
#if defined(DOTPROD_PLATFORM_XTENSA_S3)
            if (config.num_channels <= 2) {
                xtensa_s3::compute_block_shift(config, output);
                return true;
            }
            // >2 channels falls through to C++
#elif defined(DOTPROD_PLATFORM_RISCV_P4)
            if (config.num_channels <= 2) {
                riscv_p4::compute_block_shift(config, output);
                return true;
            }
            // >2 channels falls through to C++
#elif defined(DOTPROD_PLATFORM_NEON)
            neon::compute_block_shift(config, output);
            return false;
#elif defined(DOTPROD_PLATFORM_XTENSA_MAC16)
            if (config.num_channels <= 2) {
                xtensa_mac16::compute_block_shift(config, output);
                return false;
            }
            // >2 channels falls through to C++
#endif
        }
    }

    // C++ fallback for:
    // - >8 channels (S3/NEON) or >2 channels (MAC16)
    // - Generic platform
    compute_block_adaptive_q_cpp(config, output);
    return false;
}

// ============================================================================
// Fixed Precision Dispatch (Q31, Q31Lite, Float, and Float32)
// ============================================================================
// Tries SIMD for Q31/Float/Float32 with up to 8 channels, or the scalar
// mulsh/mulh kernels for Q31Lite on Xtensa and the ESP32-P4 with 1-2 channels,
// otherwise falls back to C++.

template <typename CoeffFormat>
void compute_fixed_precision(const DotProdConfig<CoeffFormat>& config,
                             typename CoeffFormat::accumulator_t* output) {
#if defined(DOTPROD_PLATFORM_XTENSA_S3)
    constexpr bool is_float32 = std::is_same_v<CoeffFormat, Float32Format>;

    if constexpr (is_float32) {
        if (config.num_channels <= 2) {
            xtensa_s3::compute_fixed_float32(config, output);
            return;
        }
    }
#elif defined(DOTPROD_PLATFORM_RISCV_P4)
    constexpr bool is_float32 = std::is_same_v<CoeffFormat, Float32Format>;
    constexpr bool is_q31lite = std::is_same_v<CoeffFormat, Q31LiteFormat>;

    if constexpr (is_float32) {
        if (config.num_channels <= 2) {
            riscv_p4::compute_fixed_float32(config, output);
            return;
        }
    } else if constexpr (is_q31lite) {
        if (config.num_channels <= 2) {
            riscv_p4::compute_fixed_q31lite(config, output);
            return;
        }
    }
#endif

#if defined(DOTPROD_PLATFORM_XTENSA_S3) || defined(DOTPROD_PLATFORM_XTENSA_MAC16)
    constexpr bool is_q31lite = std::is_same_v<CoeffFormat, Q31LiteFormat>;

    if constexpr (is_q31lite) {
        if (config.num_channels <= 2) {
            xtensa_q31lite::compute_fixed(config, output);
            return;
        }
    }
#endif

#if defined(DOTPROD_PLATFORM_NEON)
    constexpr bool IS_Q31 = std::is_same_v<CoeffFormat, Q31Format>;
    constexpr bool IS_FLOAT = std::is_same_v<CoeffFormat, Float64Format>;
    constexpr bool IS_FLOAT32 = std::is_same_v<CoeffFormat, Float32Format>;

    if (config.num_channels <= SIMDTraits<CoeffFormat>::MAX_CHANNELS) {
        if constexpr (IS_Q31) {
            neon::compute_fixed_q31(config, output);
            return;
        } else if constexpr (IS_FLOAT) {
            neon::compute_fixed_float(config, output);
            return;
        } else if constexpr (IS_FLOAT32) {
            neon::compute_fixed_float32(config, output);
            return;
        }
    }
#endif

    // C++ fallback
    compute_fixed_precision_cpp(config, output);
}

}  // namespace detail

// ============================================================================
// Main Compute Function Implementation
// ============================================================================

template <typename CoeffFormat>
void compute(const DotProdConfig<CoeffFormat>& config,
             typename CoeffFormat::accumulator_t* output) {
    if (config.block_shifts != nullptr) {
        // PATH 1: Block Adaptive Q (returns true when the kernel already
        // folded the max-tap entries into the accumulators)
        bool max_taps_applied = detail::compute_block_adaptive_q(config, output);

        // Apply max tap extraction if enabled and not fused into the kernel
        if (!max_taps_applied && config.max_tap_entries != nullptr && config.max_taps_count > 0) {
            MR_PROF(uint32_t prof_maxtap = profile::ccount();)
            detail::apply_max_taps(config, output);
            MR_PROF(profile::mark(prof_maxtap, profile::g_cycles.maxtap);)
        }
    } else {
        // PATH 3: Fixed Precision (NEON optimized for Q31/Float)
        detail::compute_fixed_precision(config, output);
    }
}

}  // namespace dotprod
}  // namespace micro_resampler

#endif  // DOTPROD_DISPATCH_H
