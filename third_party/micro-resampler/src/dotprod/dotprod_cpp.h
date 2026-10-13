// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file dotprod_cpp.h
/// @brief Portable C++ fallback implementations for dot product operations
///
/// Provides compute_block_adaptive_q_cpp() (PATH 1b) and
/// compute_fixed_precision_cpp() (PATH 3), used when platform-specific SIMD
/// is not available, or for formats/channel counts SIMD does not support
/// (e.g., Q31, more than 2 channels)

#ifndef DOTPROD_CPP_H
#define DOTPROD_CPP_H

#include "detail/coefficient_ops.h"
#include "dotprod.h"

#include <cstdint>
#include <cstring>

namespace micro_resampler {
namespace dotprod {
namespace detail {

// ============================================================================
// PATH 1b: Block Adaptive Q - C++ Fallback
// ============================================================================
// Processes coefficients in blocks of 8, with per-block right shifts.
// Shifts each product immediately before accumulating, matching ESP32-S3 SIMD
// behavior. This allows direct accumulation into the output buffer.

template <typename CoeffFormat>
void compute_block_adaptive_q_cpp(const DotProdConfig<CoeffFormat>& config,
                                  typename CoeffFormat::accumulator_t* output) {
    // Initialize output to zero
    std::memset(output, 0, config.num_channels * sizeof(typename CoeffFormat::accumulator_t));

    size_t num_blocks = config.length / 8;
    for (size_t i = 0; i < num_blocks; ++i) {
        uint8_t block_shift = config.block_shifts[i];

        for (size_t j = 0; j < 8; ++j) {
            typename CoeffFormat::coefficient_t tap = config.coeffs[i * 8 + j];
            for (int ch = 0; ch < config.num_channels; ++ch) {
                // Shift each product immediately, then accumulate directly into output
                // This matches ESP32-S3 SIMD behavior where shift is part of MAC
                typename CoeffFormat::accumulator_t product =
                    ::micro_resampler::detail::FormatOps<CoeffFormat>::multiply_sample_coeff(
                        config.samples[config.num_channels * (i * 8 + j) + ch], tap);
                output[ch] =
                    ::micro_resampler::detail::FormatOps<CoeffFormat>::shift_and_accumulate(
                        output[ch], product, block_shift);
            }
        }
    }
}

// ============================================================================
// PATH 3: Fixed Precision - C++ Only
// ============================================================================
// No adaptive shifts. Simple MAC loop.
// Specialized versions for 1 and 2 channels keep accumulators in registers.

template <typename CoeffFormat>
void compute_fixed_precision_cpp_mono(const DotProdConfig<CoeffFormat>& config,
                                      typename CoeffFormat::accumulator_t* output) {
    typename CoeffFormat::accumulator_t acc0 = 0;

    for (int i = 0; i < config.length; ++i) {
        acc0 = ::micro_resampler::detail::FormatOps<CoeffFormat>::mac_coefficient(
            acc0, config.samples[i], config.coeffs[i]);
    }

    output[0] = acc0;
}

template <typename CoeffFormat>
void compute_fixed_precision_cpp_stereo(const DotProdConfig<CoeffFormat>& config,
                                        typename CoeffFormat::accumulator_t* output) {
    typename CoeffFormat::accumulator_t acc0 = 0;
    typename CoeffFormat::accumulator_t acc1 = 0;

    const auto* samples = config.samples;
    const auto* coeffs = config.coeffs;
    const int length = config.length;

    for (int i = 0; i < length; ++i) {
        const auto tap = coeffs[i];
        const int idx = i * 2;
        acc0 = ::micro_resampler::detail::FormatOps<CoeffFormat>::mac_coefficient(
            acc0, samples[idx], tap);
        acc1 = ::micro_resampler::detail::FormatOps<CoeffFormat>::mac_coefficient(
            acc1, samples[idx + 1], tap);
    }

    output[0] = acc0;
    output[1] = acc1;
}

template <typename CoeffFormat>
void compute_fixed_precision_cpp(const DotProdConfig<CoeffFormat>& config,
                                 typename CoeffFormat::accumulator_t* output) {
    // Specialized paths for common channel counts keep accumulators in registers
    if (config.num_channels == 1) {
        compute_fixed_precision_cpp_mono(config, output);
        return;
    }
    if (config.num_channels == 2) {
        compute_fixed_precision_cpp_stereo(config, output);
        return;
    }

    // Generic fallback for >2 channels
    std::memset(output, 0, config.num_channels * sizeof(typename CoeffFormat::accumulator_t));

    for (int i = 0; i < config.length; ++i) {
        typename CoeffFormat::processing_t tap = config.coeffs[i];
        for (int ch = 0; ch < config.num_channels; ++ch) {
            output[ch] = ::micro_resampler::detail::FormatOps<CoeffFormat>::mac_coefficient(
                output[ch], config.samples[config.num_channels * i + ch], tap);
        }
    }
}

}  // namespace detail
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // DOTPROD_CPP_H
