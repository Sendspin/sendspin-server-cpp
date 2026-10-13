// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file neon.h
/// @brief ARM NEON dot product backend declarations
///
/// Declares the NEON-accelerated compute functions for Q15 (block shift,
/// including per-channel-count and multichannel variants), Q31, Float64,
/// and Float32 formats; selected by dotprod_dispatch.h on ARM targets

#ifndef DOTPROD_ARM_NEON_H
#define DOTPROD_ARM_NEON_H

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

// Note: This header is always included via dotprod_dispatch.h, which is included
// at the end of dotprod.h. Therefore, DotProdConfig is already defined by the
// time this header is processed.

#include "../../detail/fixed_point.h"  // For q15_t
#include "formats.h"                   // For Q15Format, Q31Format, Float32Format, Float64Format

namespace micro_resampler {
namespace dotprod {
namespace neon {

// ============================================================================
// Q15 Block Shift Declarations (implemented in neon_q15.cpp)
// ============================================================================
void compute_block_shift_ch1(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output);
void compute_block_shift_ch2(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output);
void compute_block_shift_ch2_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output);
void compute_block_shift_3ch(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output);
void compute_block_shift_3ch_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output);
void compute_block_shift_4ch(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output);
void compute_block_shift_4ch_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output);
void compute_block_shift_multichannel(const DotProdConfig<Q15Format>& config,
                                      Q15Format::accumulator_t* output);

// Q15 Block Shift Dispatch (inline template)
template <typename CoeffFormat>
inline void compute_block_shift(const DotProdConfig<CoeffFormat>& config,
                                typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>,
                  "NEON block shift only supports Q15Format");
    switch (config.num_channels) {
        case 1:
            compute_block_shift_ch1(config, output);
            break;
        case 2:
            if (config.length >= 32) {
                compute_block_shift_ch2_wide(config, output);
            } else {
                compute_block_shift_ch2(config, output);
            }
            break;
        case 3:
            if (config.length >= 32) {
                compute_block_shift_3ch_wide(config, output);
            } else {
                compute_block_shift_3ch(config, output);
            }
            break;
        case 4:
            if (config.length >= 32) {
                compute_block_shift_4ch_wide(config, output);
            } else {
                compute_block_shift_4ch(config, output);
            }
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            compute_block_shift_multichannel(config, output);
            break;
        default:
            break;
    }
}

// Simple Q15 compute (currently unused but available)
int32_t compute_simple(const int16_t* samples, const q15_t* coeffs, int length);

// ============================================================================
// Q31 Fixed Precision Declaration (implemented in neon_q31.cpp)
// ============================================================================
void compute_fixed_q31(const DotProdConfig<Q31Format>& config, Q31Format::accumulator_t* output);

// ============================================================================
// Float (double) Fixed Precision Declaration (implemented in neon_float.cpp)
// ============================================================================
void compute_fixed_float(const DotProdConfig<Float64Format>& config,
                         Float64Format::accumulator_t* output);

// ============================================================================
// Float32 Fixed Precision Declaration (implemented in neon_float32.cpp)
// ============================================================================
void compute_fixed_float32(const DotProdConfig<Float32Format>& config,
                           Float32Format::accumulator_t* output);

}  // namespace neon
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // __ARM_NEON || __ARM_NEON__

#endif  // DOTPROD_ARM_NEON_H
