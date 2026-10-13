// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file q31lite.h
/// @brief Xtensa scalar wrapper for Q31Lite dot product operations
///
/// Declares the assembly-backed Q31Lite fixed-precision dot product routines.
/// Q31Lite coefficients are 32-bit, which no Xtensa SIMD unit can multiply, so
/// these kernels are scalar mulsh loops shared by every Xtensa target (LX6 and
/// LX7); they win over compiled C++ via zero-overhead loops and unrolling

#ifndef DOTPROD_XTENSA_Q31LITE_H
#define DOTPROD_XTENSA_Q31LITE_H

#if defined(__XTENSA__)

#include "../dotprod.h"
#include <stdint.h>

// External assembly functions (implemented in dotprod/xtensa/xtensa_dotprod_q31lite*.S)
//
// Alignment contract: samples and coefficients are int32, so both pointers are
// naturally 4-byte aligned; no further alignment is required. Length must be a
// multiple of 8 taps (the filter-length contract for every design).
extern "C" {
void xtensa_dotprod_s32_q31lite_ch1(const int32_t* samples, const int32_t* coeffs, int length,
                                    int32_t* output);

void xtensa_dotprod_s32_q31lite_ch2(const int32_t* samples, const int32_t* coeffs, int length,
                                    int32_t* output);

// Dual-row variants: one call computes BOTH blend rows' dot products for up
// to 4 outputs, loading each sample once. The row pair is fixed across the
// batch (the near-unity ASRC case); only the sample pointer differs per
// output. packed_args carries the filter length (bits 0-15) and output count
// (bits 20-22, 1-4). Outputs are written as [row A, row B] pairs (mono) or
// [row A L, row A R, row B L, row B R] quads (stereo) per output.
void xtensa_dotprod_s32_q31lite_ch1_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                                         const int32_t* coeffs_b, int packed_args, int32_t* output);

void xtensa_dotprod_s32_q31lite_ch2_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                                         const int32_t* coeffs_b, int packed_args, int32_t* output);
}

namespace micro_resampler {
namespace dotprod {
namespace xtensa_q31lite {

// ============================================================================
// Fixed Precision (Q31Lite only, 1-2 channels)
// ============================================================================

template <typename CoeffFormat>
void compute_fixed(const DotProdConfig<CoeffFormat>& config,
                   typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q31LiteFormat>,
                  "Xtensa Q31Lite kernels only support Q31LiteFormat");

    if (config.num_channels == 1) {
        xtensa_dotprod_s32_q31lite_ch1(config.samples, config.coeffs, config.length, output);
    } else if (config.num_channels == 2) {
        xtensa_dotprod_s32_q31lite_ch2(config.samples, config.coeffs, config.length, output);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Dual-Row Fixed Precision (Q31Lite only, 1-2 channels)
// ============================================================================
// One call computes both blend rows' dot products for `count` (1-4) outputs
// sharing a fixed row pair, loading each sample once. The caller blends the
// [row A, row B] accumulator pairs itself.

inline void compute_fixed_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                               const int32_t* coeffs_b, int length, int count, int num_channels,
                               int32_t* output) {
    const int packed_args = length | (count << 20);

    if (num_channels == 1) {
        xtensa_dotprod_s32_q31lite_ch1_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    } else {
        xtensa_dotprod_s32_q31lite_ch2_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    }
}

}  // namespace xtensa_q31lite
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // defined(__XTENSA__)

#endif  // DOTPROD_XTENSA_Q31LITE_H
