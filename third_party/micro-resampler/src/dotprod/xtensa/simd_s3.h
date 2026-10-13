// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file simd_s3.h
/// @brief ESP32-S3 SIMD (EE.*) wrapper for dot product operations
///
/// Provides a unified interface to the assembly implementations for Q15 fixed-point
/// and Float32 dot products on the ESP32-S3

#ifndef DOTPROD_XTENSA_SIMD_S3_H
#define DOTPROD_XTENSA_SIMD_S3_H

#if defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

#include "../dotprod.h"
#include <stddef.h>
#include <stdint.h>

// External assembly functions (implemented in dotprod/xtensa/xtensa_dotprod_s3*.S)
// Note: Use int16_t directly since q15_t is now namespaced
//
// The shift kernels also apply the pre-extracted max taps while the
// accumulators are still in registers: length_and_taps packs the filter
// length in bits 0-15 and the max tap count in bits 16-19, and
// max_tap_entries points at the 8-byte {int32 coeff, uint16 index_reversed,
// pad} records for the phase (ignored when the packed count is 0)
extern "C" {
int32_t xtensa_dotprod_s16_q15_simd(const int16_t* delay_line, const int16_t* coeffs, int length);

int32_t xtensa_dotprod_s16_q15_shift_simd_ch1(const int16_t* delay_line, const int16_t* coeffs,
                                              int length_and_taps, const uint8_t* shifts,
                                              int32_t* output, const void* max_tap_entries);

int32_t xtensa_dotprod_s16_q15_shift_simd_ch2(const int16_t* delay_line, const int16_t* coeffs,
                                              int length_and_taps, const uint8_t* shifts,
                                              int32_t* output, const void* max_tap_entries);

// Batched variants: one call runs the shift kernel body for up to 4
// consecutive outputs of the aligned fixed path. packed_args carries the
// filter length (bits 0-15), max tap count (bits 16-19), output count
// (bits 20-22, 1-4), and the fixed-row flag (bit 23); sample_ptrs holds one
// delay-line pointer per output. With the flag clear the coefficient, shift,
// and max-tap rows advance one bank row per output; with it set every output
// reuses the first row (single-phase banks).
void xtensa_dotprod_s16_q15_shift_simd_ch1_batch(const int16_t* const* sample_ptrs,
                                                 const int16_t* coeffs, int packed_args,
                                                 const uint8_t* shifts, int32_t* output,
                                                 const void* max_tap_entries);

void xtensa_dotprod_s16_q15_shift_simd_ch2_batch(const int16_t* const* sample_ptrs,
                                                 const int16_t* coeffs, int packed_args,
                                                 const uint8_t* shifts, int32_t* output,
                                                 const void* max_tap_entries);

// Dual-row variants: one call computes BOTH blend rows' dot products for up
// to 4 outputs, loading and aligning each output's delay line once. The row
// pair is fixed across the batch (the near-unity ASRC case); only the sample
// pointer differs per output. packed_args carries the filter length
// (bits 0-15), max tap count (bits 16-19), and output count (bits 20-22,
// 1-4); the extra pointer carries the row B blocks and both rows' max-tap
// records (see Q15DualRowExtra). Outputs are written as [row A, row B] pairs
// (mono) or [row A L, row A R, row B L, row B R] quads (stereo) per output.
void xtensa_dotprod_s16_q15_shift_simd_ch1_dual(const int16_t* const* sample_ptrs,
                                                const int16_t* coeffs_a, int packed_args,
                                                const uint8_t* shifts_a, int32_t* output,
                                                const void* extra);

void xtensa_dotprod_s16_q15_shift_simd_ch2_dual(const int16_t* const* sample_ptrs,
                                                const int16_t* coeffs_a, int packed_args,
                                                const uint8_t* shifts_a, int32_t* output,
                                                const void* extra);

// Float32 dot product functions
void xtensa_dotprod_f32_simd_ch1(const float* samples, const float* coeffs, int length,
                                 float* output);
void xtensa_dotprod_f32_simd_ch2(const float* samples, const float* coeffs, int length,
                                 float* output);

// Float32 dual-row variants: one call computes BOTH blend rows' dot products
// for up to 4 outputs, loading each sample once. The row pair is fixed
// across the batch (the near-unity ASRC case); only the sample pointer
// differs per output. packed_args carries the filter length (bits 0-15) and
// output count (bits 20-22, 1-4). The accumulation pattern matches the
// single-output kernels exactly, so results are bit-identical to them
// despite float addition being non-associative. Outputs are written as
// [row A, row B] pairs (mono) or [row A L, row A R, row B L, row B R]
// quads (stereo) per output. Stereo sample pointers must be 8-byte aligned
// (interleaved stereo float frames).
void xtensa_dotprod_f32_simd_ch1_dual(const float* const* sample_ptrs, const float* coeffs_a,
                                      const float* coeffs_b, int packed_args, float* output);

void xtensa_dotprod_f32_simd_ch2_dual(const float* const* sample_ptrs, const float* coeffs_a,
                                      const float* coeffs_b, int packed_args, float* output);
}

namespace micro_resampler {
namespace dotprod {
namespace xtensa_s3 {

// ============================================================================
// Block Adaptive Q with SIMD (Q15 only, 1-2 channels)
// ============================================================================
// The kernels fold the max-tap MACs into the accumulators before the output
// store, so callers must not run apply_max_taps() afterwards.

template <typename CoeffFormat>
void compute_block_shift(const DotProdConfig<CoeffFormat>& config,
                         typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>, "Xtensa S3 SIMD only supports Q15Format");

    // The kernels read max tap entries as raw 8-byte records; pin the layout
    static_assert(sizeof(MaxTapEntry<CoeffFormat>) == 8, "kernel expects 8-byte max tap entries");
    static_assert(offsetof(MaxTapEntry<CoeffFormat>, coeff) == 0 &&
                      offsetof(MaxTapEntry<CoeffFormat>, index_reversed) == 4,
                  "kernel expects {int32 coeff, uint16 index_reversed} layout");

    const int tap_count = (config.max_tap_entries != nullptr) ? config.max_taps_count : 0;
    const int length_and_taps = config.length | (tap_count << 16);

    if (config.num_channels == 1) {
        xtensa_dotprod_s16_q15_shift_simd_ch1(config.samples, config.coeffs, length_and_taps,
                                              config.block_shifts, output, config.max_tap_entries);
    } else if (config.num_channels == 2) {
        xtensa_dotprod_s16_q15_shift_simd_ch2(config.samples, config.coeffs, length_and_taps,
                                              config.block_shifts, output, config.max_tap_entries);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Batched Block Adaptive Q with SIMD (Q15 only, 1-2 channels)
// ============================================================================
// One call computes `count` (1-4) consecutive outputs of the aligned fixed
// path: per-output delay-line pointers with sequential coefficient/shift/
// max-tap rows, or one shared row when fixed_row is set (single-phase banks).
// The kernels fold the max-tap MACs exactly like the single-output variants,
// so callers must not run apply_max_taps() afterwards. Outputs are written as
// count (mono) or 2*count (stereo) consecutive accumulators.

inline void compute_block_shift_batch(const int16_t* const* sample_ptrs, const int16_t* coeffs,
                                      const uint8_t* shifts,
                                      const MaxTapEntry<Q15Format>* max_tap_entries, int length,
                                      int tap_count, int count, bool fixed_row, int num_channels,
                                      int32_t* output) {
    // The kernels read max tap entries as raw 8-byte records; pin the layout
    static_assert(sizeof(MaxTapEntry<Q15Format>) == 8, "kernel expects 8-byte max tap entries");
    static_assert(offsetof(MaxTapEntry<Q15Format>, coeff) == 0 &&
                      offsetof(MaxTapEntry<Q15Format>, index_reversed) == 4,
                  "kernel expects {int32 coeff, uint16 index_reversed} layout");

    const int taps = (max_tap_entries != nullptr) ? tap_count : 0;
    const int packed_args = length | (taps << 16) | (count << 20) | (fixed_row ? (1 << 23) : 0);

    if (num_channels == 1) {
        xtensa_dotprod_s16_q15_shift_simd_ch1_batch(sample_ptrs, coeffs, packed_args, shifts,
                                                    output, max_tap_entries);
    } else {
        xtensa_dotprod_s16_q15_shift_simd_ch2_batch(sample_ptrs, coeffs, packed_args, shifts,
                                                    output, max_tap_entries);
    }
}

// ============================================================================
// Dual-Row Block Adaptive Q with SIMD (Q15 only, 1-2 channels)
// ============================================================================
// One call computes both blend rows' dot products for `count` (1-4) outputs
// sharing a fixed row pair, loading each delay line once. The kernels fold
// each row's max-tap MACs into its own accumulators, so callers must not run
// apply_max_taps() afterwards. The caller blends the [row A, row B]
// accumulator pairs itself.

// Row B pointer block for the dual-row kernels, passed as a single pointer
// to stay within the windowed ABI's six register arguments. Field order and
// offsets are read directly by the assembly.
struct Q15DualRowExtra {
    const int16_t* coeffs_b;
    const uint8_t* shifts_b;
    const void* entries_a;
    const void* entries_b;
};

inline void compute_block_shift_dual(const int16_t* const* sample_ptrs, const int16_t* coeffs_a,
                                     const uint8_t* shifts_a,
                                     const MaxTapEntry<Q15Format>* entries_a,
                                     const int16_t* coeffs_b, const uint8_t* shifts_b,
                                     const MaxTapEntry<Q15Format>* entries_b, int length,
                                     int tap_count, int count, int num_channels, int32_t* output) {
    // The kernels read max tap entries as raw 8-byte records; pin the layout
    static_assert(sizeof(MaxTapEntry<Q15Format>) == 8, "kernel expects 8-byte max tap entries");
    static_assert(offsetof(MaxTapEntry<Q15Format>, coeff) == 0 &&
                      offsetof(MaxTapEntry<Q15Format>, index_reversed) == 4,
                  "kernel expects {int32 coeff, uint16 index_reversed} layout");

    const int taps = (entries_a != nullptr) ? tap_count : 0;
    const int packed_args = length | (taps << 16) | (count << 20);
    const Q15DualRowExtra extra{coeffs_b, shifts_b, entries_a, entries_b};

    if (num_channels == 1) {
        xtensa_dotprod_s16_q15_shift_simd_ch1_dual(sample_ptrs, coeffs_a, packed_args, shifts_a,
                                                   output, &extra);
    } else {
        xtensa_dotprod_s16_q15_shift_simd_ch2_dual(sample_ptrs, coeffs_a, packed_args, shifts_a,
                                                   output, &extra);
    }
}

// ============================================================================
// Fixed Precision with SIMD (Q15 only, mono only)
// ============================================================================
// Currently unused in the resampler, but available for future use.

template <typename CoeffFormat>
int32_t compute_simple(const typename CoeffFormat::sample_t* samples,
                       const typename CoeffFormat::coefficient_t* coeffs, int length) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>, "Xtensa S3 SIMD only supports Q15Format");

    return xtensa_dotprod_s16_q15_simd(samples, coeffs, length);
}

// ============================================================================
// Fixed Precision with SIMD (Float32, 1-2 channels)
// ============================================================================

template <typename CoeffFormat>
void compute_fixed_float32(const DotProdConfig<CoeffFormat>& config,
                           typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Float32Format>,
                  "Xtensa S3 float SIMD only supports Float32Format");

    if (config.num_channels == 1) {
        xtensa_dotprod_f32_simd_ch1(config.samples, config.coeffs, config.length, output);
    } else if (config.num_channels == 2) {
        xtensa_dotprod_f32_simd_ch2(config.samples, config.coeffs, config.length, output);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Dual-Row Fixed Precision with SIMD (Float32, 1-2 channels)
// ============================================================================
// One call computes both blend rows' dot products for `count` (1-4) outputs
// sharing a fixed row pair, loading each sample once. Results are
// bit-identical to the single-output kernels (same accumulation pattern).
// The caller blends the [row A, row B] accumulator pairs itself.

inline void compute_fixed_float32_dual(const float* const* sample_ptrs, const float* coeffs_a,
                                       const float* coeffs_b, int length, int count,
                                       int num_channels, float* output) {
    const int packed_args = length | (count << 20);

    if (num_channels == 1) {
        xtensa_dotprod_f32_simd_ch1_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    } else {
        xtensa_dotprod_f32_simd_ch2_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    }
}

}  // namespace xtensa_s3
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

#endif  // DOTPROD_XTENSA_SIMD_S3_H
