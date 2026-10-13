// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-S3 SIMD implementation for Float32 1-channel audio FIR filter
// Supports any length that is a multiple of 8

#if defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

    .text
    .align 4

    // Function: xtensa_dotprod_f32_simd_ch1
    // Computes dot product of float32 samples and float32 coefficients using SIMD (1 channel)
    // Parameters:
    //   a2: samples (float*) - input samples
    //   a3: coeffs (float*) - filter coefficients
    //   a4: length (int) - must be multiple of 8
    //   a5: output (float*) - pointer to single float output

    .global xtensa_dotprod_f32_simd_ch1
    .type   xtensa_dotprod_f32_simd_ch1, @function
xtensa_dotprod_f32_simd_ch1:
    entry   sp, 32

    // Initialize 4 float accumulators to zero for maximum ILP
    const.s f0, 0
    const.s f1, 0
    const.s f2, 0
    const.s f3, 0

    // Calculate main loop count (length / 4)
    srli    a4, a4, 2               // a4 = length / 4

    // Round down to 16-byte boundary
    movi.n  a12, -16        // Mask: 0xFFFFFFF0
    and     a15, a2, a12    // a15 = a2 rounded down to 16-byte boundary

    // Jump to correct offset case
    extui   a8, a2, 2, 2        // Extract offset index (0-3)
    beqz    a8, .offset_0
    addi    a8, a8, -1
    beqz    a8, .offset_1
    addi    a8, a8, -1
    beqz    a8, .offset_2
    j       .offset_3           // Must be 3

.offset_0:
    // Preload first 4 samples
    ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

    // Unroll by 2 (length is a multiple of 8, so a4 is even), double-buffering
    // samples between f8-f11 and f4-f7 so every load stays ahead of its madd
    srli    a4, a4, 1

    loopnez a4, .L_f32_o0_ch1_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16
        // Load next 4 samples
        ee.ldf.128.ip   f7, f6, f5, f4, a15, 16

        madd.s f0, f8,  f12
        madd.s f1, f9,  f13
        madd.s f2, f10, f14
        madd.s f3, f11, f15

        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16
        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

        madd.s f0, f4,  f12
        madd.s f1, f5,  f13
        madd.s f2, f6,  f14
        madd.s f3, f7,  f15
    .L_f32_o0_ch1_loop_end:
    j    .store_result;

.offset_1:
    // Load first 3 samples, ignore f8
    ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

    loopnez a4, .L_f32_o1_ch1_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16

        madd.s f0, f9,  f12
        madd.s f1, f10, f13
        madd.s f2, f11, f14

        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16
        madd.s f3, f8, f15
    .L_f32_o1_ch1_loop_end:
    j    .store_result;

.offset_2:
    // Load first 2 samples, ignore f8 and f9
    ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

    loopnez a4, .L_f32_o2_ch1_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16

        madd.s f0, f10, f12
        madd.s f1, f11, f13

        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

        madd.s f2, f8,  f14
        madd.s f3, f9,  f15
    .L_f32_o2_ch1_loop_end:
    j    .store_result

.offset_3:
    // Load first 1 sample, ignore f8, f9, f10
    ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

    loopnez a4, .L_f32_o3_ch1_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16

        madd.s f0, f11, f12

        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

        madd.s f1, f8,  f13
        madd.s f2, f9,  f14
        madd.s f3, f10, f15
    .L_f32_o3_ch1_loop_end:

.store_result:

    add.s f0, f0, f1
    add.s f2, f2, f3
    add.s f0, f0, f2

    ssi     f0, a5, 0

    retw.n

    .size   xtensa_dotprod_f32_simd_ch1, . - xtensa_dotprod_f32_simd_ch1

    // Function: xtensa_dotprod_f32_simd_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one call:
    // each sample is loaded once and multiplied against both coefficient rows
    // (the ASRC blend pair). Samples load with scalar lsi (any 4-byte
    // alignment, so no offset-case dispatch); coefficient rows stay on the
    // 128-bit loads (rows are always 16-byte aligned).
    //
    // Float addition is not associative, so the accumulation pattern matches
    // the single-output kernel exactly for bit-identical results: tap k
    // accumulates into accumulator k % 4 and the reduction is
    // (acc0 + acc1) + (acc2 + acc3), per row.
    // Parameters:
    //   a2: sample_ptrs (const float* const*) - one delay-line pointer per output
    //   a3: coeffs_a (float*) - row A coefficients, must be 16-byte aligned
    //   a4: coeffs_b (float*) - row B coefficients, must be 16-byte aligned
    //   a5: length (bits 0-15, must be multiple of 8) | output count (bits 20-22, 1-4)
    //   a6: output (float*) - [row A, row B] pair per output
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.

    .align  4
    .global xtensa_dotprod_f32_simd_ch1_dual
    .type   xtensa_dotprod_f32_simd_ch1_dual, @function
xtensa_dotprod_f32_simd_ch1_dual:
    entry   sp, 32

    mov.n   a7, a2              // sample_ptrs cursor
    extui   a8, a5, 20, 3       // Output count

.L_f32d1_outer:
    l32i.n  a2, a7, 0           // Delay line for this output
    extui   a9, a5, 0, 16
    srli    a9, a9, 2           // length / 4 (taps per iteration: 4)

    // Row A accumulators f0-f3, row B accumulators f4-f7
    const.s f0, 0
    const.s f1, 0
    const.s f2, 0
    const.s f3, 0
    const.s f4, 0
    const.s f5, 0
    const.s f6, 0
    const.s f7, 0

    loopnez a9, .L_f32d1_loop_end
        lsi     f8, a2, 0
        lsi     f9, a2, 4
        lsi     f10, a2, 8
        lsi     f11, a2, 12
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16      // Row A coefficients
        madd.s  f0, f8, f12
        madd.s  f1, f9, f13
        madd.s  f2, f10, f14
        madd.s  f3, f11, f15
        ee.ldf.128.ip   f15, f14, f13, f12, a4, 16      // Row B coefficients
        addi    a2, a2, 16
        madd.s  f4, f8, f12
        madd.s  f5, f9, f13
        madd.s  f6, f10, f14
        madd.s  f7, f11, f15
.L_f32d1_loop_end:

    // Reduce each row in the single-output kernel's exact order
    add.s   f0, f0, f1
    add.s   f2, f2, f3
    add.s   f4, f4, f5
    add.s   f6, f6, f7
    add.s   f0, f0, f2
    add.s   f4, f4, f6

    // Store [row A, row B], rewind the coefficient cursors, next output
    ssi     f0, a6, 0
    ssi     f4, a6, 4
    extui   a9, a5, 0, 16
    slli    a9, a9, 2
    sub     a3, a3, a9
    sub     a4, a4, a9
    addi.n  a6, a6, 8
    addi.n  a7, a7, 4
    addi.n  a8, a8, -1
    bnez    a8, .L_f32d1_outer

    retw.n

    .size   xtensa_dotprod_f32_simd_ch1_dual, . - xtensa_dotprod_f32_simd_ch1_dual

#endif
