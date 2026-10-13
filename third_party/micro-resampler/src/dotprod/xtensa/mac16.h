// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file mac16.h
/// @brief Xtensa MAC16 (non-S3) wrapper for dot product operations
///
/// Declares the assembly-backed Q15 dot product routines used on Xtensa targets
/// without ESP32-S3 SIMD support

#ifndef DOTPROD_XTENSA_MAC16_H
#define DOTPROD_XTENSA_MAC16_H

#if defined(__XTENSA__) && !(defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

#include "../dotprod.h"
#include <stdint.h>

// External assembly functions (implemented in dotprod/xtensa/xtensa_dotprod*.S)
// Note: Use int16_t directly since q15_t is now namespaced
//
// Alignment contract: coefficient pointers must be 4-byte aligned (the filter
// bank is 16-byte aligned and each phase spans a multiple of 8 taps). The
// shift_ch1 kernel accepts delay-line pointers at any int16 boundary; the
// plain and shift_ch2 kernels require 4-byte alignment (stereo frames are
// 4 bytes, so any frame boundary in the history buffer qualifies).
extern "C" {
int32_t xtensa_dotprod_s16_q15_mac16(const int16_t* delay_line, const int16_t* coeffs, int length);

void xtensa_dotprod_s16_q15_shift_mac16_ch1(const int16_t* delay_line, const int16_t* coeffs,
                                            int length, const uint8_t* shifts, int32_t* output);

void xtensa_dotprod_s16_q15_shift_mac16_ch2(const int16_t* delay_line, const int16_t* coeffs,
                                            int length, const uint8_t* shifts, int32_t* output);
}

namespace micro_resampler {
namespace dotprod {
namespace xtensa_mac16 {

// ============================================================================
// Block Adaptive Q with MAC16 (Q15 only, 1-2 channels)
// ============================================================================

template <typename CoeffFormat>
void compute_block_shift(const DotProdConfig<CoeffFormat>& config,
                         typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>, "Xtensa MAC16 only supports Q15Format");

    if (config.num_channels == 1) {
        xtensa_dotprod_s16_q15_shift_mac16_ch1(config.samples, config.coeffs, config.length,
                                               config.block_shifts, output);
    } else if (config.num_channels == 2) {
        xtensa_dotprod_s16_q15_shift_mac16_ch2(config.samples, config.coeffs, config.length,
                                               config.block_shifts, output);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Fixed Precision with MAC16 (Q15 only, mono only)
// ============================================================================

template <typename CoeffFormat>
int32_t compute_simple(const typename CoeffFormat::sample_t* samples,
                       const typename CoeffFormat::coefficient_t* coeffs, int length) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>, "Xtensa MAC16 only supports Q15Format");

    return xtensa_dotprod_s16_q15_mac16(samples, coeffs, length);
}

}  // namespace xtensa_mac16
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // defined(__XTENSA__) && !(defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

#endif  // DOTPROD_XTENSA_MAC16_H
