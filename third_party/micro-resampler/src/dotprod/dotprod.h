// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file dotprod.h
/// @brief Centralized dot product API for polyphase filter bank resampling
///
/// Declares DotProdConfig, SIMDTraits, and the compute() entry point, which
/// provides a unified interface that dispatches to platform-specific
/// implementations (Xtensa SIMD, ARM NEON, or C++ fallback)

#ifndef DOTPROD_H
#define DOTPROD_H

#include "sinc_table.h"  // For MaxTapEntry

#include <cstdint>
#include <type_traits>

namespace micro_resampler {
namespace dotprod {

// ============================================================================
// Dot Product Configuration
// ============================================================================
// Contains all parameters needed for a single dot product computation.
// This struct is passed to the compute() function, which dispatches to
// the appropriate implementation based on platform and format.

template <typename CoeffFormat>
struct DotProdConfig {
    // Input data
    const typename CoeffFormat::sample_t* samples;      // Input samples (interleaved)
    const typename CoeffFormat::coefficient_t* coeffs;  // Filter coefficients for this phase
    int length;                                         // Number of taps (filter length)
    int num_channels;                                   // Number of interleaved channels

    // Adaptive Q parameters (set to nullptr if not used)
    const uint8_t* block_shifts;  // Per-8-coefficient block shifts (PATH 1)

    // Max tap extraction (set to nullptr if not used)
    const MaxTapEntry<CoeffFormat>* max_tap_entries;  // Pre-extracted max taps
    int max_taps_count;                               // Number of max taps (0 if disabled)
};

// ============================================================================
// SIMD Capability Traits
// ============================================================================

template <typename CoeffFormat>
struct SIMDTraits {
    // SIMD is available for Q15Format, Q31Format, Float64Format, and Float32Format on ARM NEON
    static constexpr bool AVAILABLE = std::is_same_v<typename CoeffFormat::sample_t, int16_t> ||
                                      std::is_same_v<typename CoeffFormat::sample_t, int32_t> ||
                                      std::is_same_v<typename CoeffFormat::sample_t, double> ||
                                      std::is_same_v<typename CoeffFormat::sample_t, float>;

    // Maximum channels supported by SIMD implementations
    // ARM NEON supports up to 8 channels with the multichannel implementation
    static constexpr int MAX_CHANNELS = 8;
};

// ============================================================================
// Compute Dot Product (Main Entry Point)
// ============================================================================
// Dispatches to appropriate implementation based on:
//   - CoeffFormat (Q15, Q31, Float)
//   - Platform (Xtensa S3, Xtensa MAC16, ARM NEON, generic C++)
//   - Adaptive Q mode (block shifts, per-tap shifts, or fixed precision)
//
// The output array must be pre-allocated with at least num_channels elements.

template <typename CoeffFormat>
void compute(const DotProdConfig<CoeffFormat>& config, typename CoeffFormat::accumulator_t* output);

}  // namespace dotprod
}  // namespace micro_resampler

// Include the dispatch implementation (must come after declarations)
#include "dotprod_dispatch.h"  // IWYU pragma: keep (defines compute(); never included directly)

#endif  // DOTPROD_H
