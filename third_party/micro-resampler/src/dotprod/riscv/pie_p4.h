// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file pie_p4.h
/// @brief ESP32-P4 PIE (esp.*) wrapper for dot product operations
///
/// Provides a unified interface to the assembly implementations for Q15
/// fixed-point dot products on the ESP32-P4's RISC-V PIE SIMD extension, plus
/// the scalar Float32 (FPU) and Q31Lite (mulh) kernels

#ifndef DOTPROD_RISCV_PIE_P4_H
#define DOTPROD_RISCV_PIE_P4_H

#if defined(__riscv) && (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || \
                         defined(CONFIG_IDF_TARGET_ESP32S31) || defined(ESP32S31))

#include "../dotprod.h"
#include <stddef.h>
#include <stdint.h>

// External assembly functions (implemented in dotprod/riscv/riscv_dotprod_p4_*.S)
//
// The shift kernels also apply the pre-extracted max taps while the
// accumulators are still in registers: length_and_taps packs the filter
// length in bits 0-15 and the max tap count in bits 16-19, and
// max_tap_entries points at the 8-byte {int32 coeff, uint16 index_reversed,
// pad} records for the phase (ignored when the packed count is 0)
extern "C" {
int32_t riscv_dotprod_s16_q15_shift_pie_ch1(const int16_t* delay_line, const int16_t* coeffs,
                                            int length_and_taps, const uint8_t* shifts,
                                            int32_t* output, const void* max_tap_entries);

int32_t riscv_dotprod_s16_q15_shift_pie_ch2(const int16_t* delay_line, const int16_t* coeffs,
                                            int length_and_taps, const uint8_t* shifts,
                                            int32_t* output, const void* max_tap_entries);

// Batched variants for the aligned fixed-ratio path: the single-output body
// for up to 4 outputs per call, with per-output delay-line pointers and
// sequential coefficient/shift/max-tap rows (or one repeated row when
// fixed_row is set). packed_args carries the filter length (bits 0-15), max
// tap count (bits 16-19, also the max-tap row stride), output count (bits
// 20-22, 1-4), and fixed_row (bit 23). Outputs are written as count (mono)
// or 2*count (stereo) consecutive accumulators.
void riscv_dotprod_s16_q15_shift_pie_ch1_batch(const int16_t* const* sample_ptrs,
                                               const int16_t* coeffs, int packed_args,
                                               const uint8_t* shifts, int32_t* output,
                                               const void* max_tap_entries);

void riscv_dotprod_s16_q15_shift_pie_ch2_batch(const int16_t* const* sample_ptrs,
                                               const int16_t* coeffs, int packed_args,
                                               const uint8_t* shifts, int32_t* output,
                                               const void* max_tap_entries);

// Dual-row variants: one call computes BOTH blend rows' dot products for up
// to 4 outputs. The row pair is fixed across the batch (the near-unity ASRC
// case); only the sample pointer differs per output. packed_args carries the
// filter length (bits 0-15), max tap count (bits 16-19), and output count
// (bits 20-22, 1-4); the extra pointer carries the row B blocks and both
// rows' max-tap records (see Q15DualRowExtra). Outputs are written as
// [row A, row B] pairs (mono) or [row A L, row A R, row B L, row B R]
// quads (stereo) per output.
void riscv_dotprod_s16_q15_shift_pie_ch1_dual(const int16_t* const* sample_ptrs,
                                              const int16_t* coeffs_a, int packed_args,
                                              const uint8_t* shifts_a, int32_t* output,
                                              const void* extra);

void riscv_dotprod_s16_q15_shift_pie_ch2_dual(const int16_t* const* sample_ptrs,
                                              const int16_t* coeffs_a, int packed_args,
                                              const uint8_t* shifts_a, int32_t* output,
                                              const void* extra);

// Float32 dot product functions (scalar FPU with independent accumulators in
// plain branch loops, avoiding the hwloop/FPU state-save erratum; the P4 has
// no floating-point SIMD)
void riscv_dotprod_f32_pie_ch1(const float* samples, const float* coeffs, int length,
                               float* output);
void riscv_dotprod_f32_pie_ch2(const float* samples, const float* coeffs, int length,
                               float* output);

// Float32 dual-row variants: one call computes both blend rows' dot products
// for up to 4 outputs, loading each sample once. The accumulation pattern
// matches the single-output kernels exactly, so results are bit-identical to
// them despite float addition being non-associative. packed_args carries the
// filter length (bits 0-15) and output count (bits 20-22, 1-4).
void riscv_dotprod_f32_pie_ch1_dual(const float* const* sample_ptrs, const float* coeffs_a,
                                    const float* coeffs_b, int packed_args, float* output);

void riscv_dotprod_f32_pie_ch2_dual(const float* const* sample_ptrs, const float* coeffs_a,
                                    const float* coeffs_b, int packed_args, float* output);

// Q31Lite dot product functions (scalar mulh; PIE has no 32-bit multiply
// lanes). Each tap adds the exact high word of the 64-bit product with a
// wrapping add, bit-exact with the C++ fallback's MULT32_32_Q32. Samples and
// coefficients only need 4-byte alignment; length must be a multiple of 8
// taps.
void riscv_dotprod_s32_q31lite_ch1(const int32_t* samples, const int32_t* coeffs, int length,
                                   int32_t* output);
void riscv_dotprod_s32_q31lite_ch2(const int32_t* samples, const int32_t* coeffs, int length,
                                   int32_t* output);

// Q31Lite dual-row variants: one call computes both blend rows' dot products
// for up to 4 outputs, loading each sample once. packed_args carries the
// filter length (bits 0-15) and output count (bits 20-22, 1-4). Outputs are
// written as [row A, row B] pairs (mono) or [row A L, row A R, row B L,
// row B R] quads (stereo) per output.
void riscv_dotprod_s32_q31lite_ch1_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                                        const int32_t* coeffs_b, int packed_args, int32_t* output);

void riscv_dotprod_s32_q31lite_ch2_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                                        const int32_t* coeffs_b, int packed_args, int32_t* output);
}

namespace micro_resampler {
namespace dotprod {
namespace riscv_p4 {

// ============================================================================
// Block Adaptive Q with PIE SIMD (Q15 only, 1-2 channels)
// ============================================================================
// The kernels fold the max-tap MACs into the accumulators before the output
// store, so callers must not run apply_max_taps() afterwards.

template <typename CoeffFormat>
void compute_block_shift(const DotProdConfig<CoeffFormat>& config,
                         typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q15Format>, "ESP32-P4 PIE only supports Q15Format");

    // The kernels read max tap entries as raw 8-byte records; pin the layout
    static_assert(sizeof(MaxTapEntry<CoeffFormat>) == 8, "kernel expects 8-byte max tap entries");
    static_assert(offsetof(MaxTapEntry<CoeffFormat>, coeff) == 0 &&
                      offsetof(MaxTapEntry<CoeffFormat>, index_reversed) == 4,
                  "kernel expects {int32 coeff, uint16 index_reversed} layout");

    const int tap_count = (config.max_tap_entries != nullptr) ? config.max_taps_count : 0;
    const int length_and_taps = config.length | (tap_count << 16);

    if (config.num_channels == 1) {
        riscv_dotprod_s16_q15_shift_pie_ch1(config.samples, config.coeffs, length_and_taps,
                                            config.block_shifts, output, config.max_tap_entries);
    } else if (config.num_channels == 2) {
        riscv_dotprod_s16_q15_shift_pie_ch2(config.samples, config.coeffs, length_and_taps,
                                            config.block_shifts, output, config.max_tap_entries);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Batched Block Adaptive Q with PIE SIMD (Q15 only, 1-2 channels)
// ============================================================================
// One call computes `count` (1-4) consecutive outputs of the aligned fixed
// path: per-output delay-line pointers with sequential coefficient/shift/
// max-tap rows, or one shared row when fixed_row is set (single-phase banks).
// The kernels fold the max-tap MACs exactly like the single-output variants,
// so callers must not run apply_max_taps() afterwards. Same interface as
// xtensa_s3::compute_block_shift_batch().

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
        riscv_dotprod_s16_q15_shift_pie_ch1_batch(sample_ptrs, coeffs, packed_args, shifts, output,
                                                  max_tap_entries);
    } else {
        riscv_dotprod_s16_q15_shift_pie_ch2_batch(sample_ptrs, coeffs, packed_args, shifts, output,
                                                  max_tap_entries);
    }
}

// ============================================================================
// Dual-Row Block Adaptive Q with PIE SIMD (Q15 only, 1-2 channels)
// ============================================================================
// One call computes both blend rows' dot products for `count` (1-4) outputs
// sharing a fixed row pair, loading each delay line once per block. The
// kernels fold each row's max-tap MACs into its own accumulators, so callers
// must not run apply_max_taps() afterwards. The caller blends the
// [row A, row B] accumulator pairs itself.

// Row B pointer block for the dual-row kernels, passed as a single pointer
// to stay within the six register arguments. Field order and offsets are
// read directly by the assembly.
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
        riscv_dotprod_s16_q15_shift_pie_ch1_dual(sample_ptrs, coeffs_a, packed_args, shifts_a,
                                                 output, &extra);
    } else {
        riscv_dotprod_s16_q15_shift_pie_ch2_dual(sample_ptrs, coeffs_a, packed_args, shifts_a,
                                                 output, &extra);
    }
}

// ============================================================================
// Fixed Precision with scalar FPU (Float32, 1-2 channels)
// ============================================================================

template <typename CoeffFormat>
void compute_fixed_float32(const DotProdConfig<CoeffFormat>& config,
                           typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Float32Format>,
                  "ESP32-P4 float kernels only support Float32Format");

    if (config.num_channels == 1) {
        riscv_dotprod_f32_pie_ch1(config.samples, config.coeffs, config.length, output);
    } else if (config.num_channels == 2) {
        riscv_dotprod_f32_pie_ch2(config.samples, config.coeffs, config.length, output);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Dual-Row Fixed Precision with scalar FPU (Float32, 1-2 channels)
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
        riscv_dotprod_f32_pie_ch1_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    } else {
        riscv_dotprod_f32_pie_ch2_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    }
}

// ============================================================================
// Fixed Precision with scalar mulh (Q31Lite, 1-2 channels)
// ============================================================================

template <typename CoeffFormat>
void compute_fixed_q31lite(const DotProdConfig<CoeffFormat>& config,
                           typename CoeffFormat::accumulator_t* output) {
    static_assert(std::is_same_v<CoeffFormat, Q31LiteFormat>,
                  "ESP32-P4 Q31Lite kernels only support Q31LiteFormat");

    if (config.num_channels == 1) {
        riscv_dotprod_s32_q31lite_ch1(config.samples, config.coeffs, config.length, output);
    } else if (config.num_channels == 2) {
        riscv_dotprod_s32_q31lite_ch2(config.samples, config.coeffs, config.length, output);
    }
    // Note: >2 channels should be handled by caller falling back to C++
}

// ============================================================================
// Dual-Row Fixed Precision with scalar mulh (Q31Lite, 1-2 channels)
// ============================================================================
// One call computes both blend rows' dot products for `count` (1-4) outputs
// sharing a fixed row pair, loading each sample once. The caller blends the
// [row A, row B] accumulator pairs itself. Same interface as
// xtensa_q31lite::compute_fixed_dual().

inline void compute_fixed_q31lite_dual(const int32_t* const* sample_ptrs, const int32_t* coeffs_a,
                                       const int32_t* coeffs_b, int length, int count,
                                       int num_channels, int32_t* output) {
    const int packed_args = length | (count << 20);

    if (num_channels == 1) {
        riscv_dotprod_s32_q31lite_ch1_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    } else {
        riscv_dotprod_s32_q31lite_ch2_dual(sample_ptrs, coeffs_a, coeffs_b, packed_args, output);
    }
}

}  // namespace riscv_p4
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // __riscv && (ESP32P4 || ESP32S31)

#endif  // DOTPROD_RISCV_PIE_P4_H
