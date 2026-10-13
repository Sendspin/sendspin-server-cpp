// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-S3 SIMD Float32 stereo dot product implementation
// Processes interleaved stereo samples [L0 R0 L1 R1 ...] with scalar floating-point unit

#if defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

    .text
    .align 4

    // Function: xtensa_dotprod_f32_simd_ch2
    // Computes dot product for 2-channel (stereo) float32 audio with interleaved samples
    // Parameters:
    //   a2: samples (float*) - interleaved stereo [L0 R0 L1 R1 L2 R2 ...]
    //   a3: coeffs (float*) - filter coefficients
    //   a4: length (int) - number of taps, must be multiple of 8
    //   a5: output (float*) - pointer to 2 floats [L_result, R_result]
    // Returns:
    //   void (results stored in output array)

    .global xtensa_dotprod_f32_simd_ch2
    .type   xtensa_dotprod_f32_simd_ch2, @function
xtensa_dotprod_f32_simd_ch2:
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

    // Jump to correct offset case (stereo samples, so only offsets 0 or 2 are possible)
    extui   a8, a2, 2, 2        // Extract offset index (0-3)
    beqz    a8, .offset_0
    j       .offset_2           // Must be 2

.offset_0:
    // Preload first 4 samples
    ee.ldf.128.ip   f7, f6, f5, f4, a15, 16

    loopnez a4, .L_f32_o0_ch2_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16
        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

        // Tap 1
        madd.s f0, f4,  f12
        madd.s f1, f5,  f12

        // Tap 2
        madd.s f2, f6,  f13
        madd.s f3, f7,  f13

        // Load first 4 samples of the next iteration
        ee.ldf.128.ip   f7, f6, f5, f4, a15, 16

        // Tap 3
        madd.s f0, f8,  f14
        madd.s f1, f9,  f14

        // Tap 4
        madd.s f2, f10, f15
        madd.s f3, f11, f15
    .L_f32_o0_ch2_loop_end:

    j    .store_result;

.offset_2:
    // Load first 2 samples, ignore f4 and f5
    ee.ldf.128.ip   f7, f6, f5, f4, a15, 16

    loopnez a4, .L_f32_o2_ch2_loop_end
        // Load coefficients
        ee.ldf.128.ip   f15, f14, f13, f12, a3, 16
        // Load next 4 samples
        ee.ldf.128.ip   f11, f10, f9, f8, a15, 16

        // Tap 1
        madd.s f0, f6, f12
        madd.s f1, f7, f12

        // Tap 2
        madd.s f2, f8, f13
        madd.s f3, f9, f13

        // Tap 3
        madd.s f0, f10, f14
        madd.s f1, f11, f14

        // Load next 4 samples
        ee.ldf.128.ip   f7, f6, f5, f4, a15, 16

        // Tap 4
        madd.s f2, f4, f15
        madd.s f3, f5, f15
    .L_f32_o2_ch2_loop_end:

.store_result:

    add.s f0, f0, f2
    add.s f1, f1, f3

    ssi   f0, a5, 0
    ssi   f1, a5, 4

    retw.n

    .size   xtensa_dotprod_f32_simd_ch2, . - xtensa_dotprod_f32_simd_ch2

    // Function: xtensa_dotprod_f32_simd_ch2_dual
    // Computes BOTH blend rows' stereo dot products for up to 4 outputs in
    // one call: each interleaved frame is loaded once and multiplied against
    // both coefficient rows (the ASRC blend pair). Frames load with
    // ee.ldf.64.ip (interleaved stereo float frames are always 8-byte
    // aligned, so no offset-case dispatch); coefficients load with scalar
    // lsi, two taps per group.
    //
    // Float addition is not associative, so the accumulation pattern matches
    // the single-output kernel exactly for bit-identical results:
    // even-index frames accumulate into acc0/acc1 (L/R), odd-index frames
    // into acc2/acc3, and the reduction is acc0 + acc2 and acc1 + acc3, per
    // row.
    // Parameters:
    //   a2: sample_ptrs (const float* const*) - one delay-line pointer per
    //       output; each must be 8-byte aligned (interleaved stereo frames)
    //   a3: coeffs_a (float*) - row A coefficients
    //   a4: coeffs_b (float*) - row B coefficients
    //   a5: length (bits 0-15, taps per channel, must be multiple of 8) |
    //       output count (bits 20-22, 1-4)
    //   a6: output (float*) - [row A left, row A right, row B left,
    //       row B right] per output
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.

    .align  4
    .global xtensa_dotprod_f32_simd_ch2_dual
    .type   xtensa_dotprod_f32_simd_ch2_dual, @function
xtensa_dotprod_f32_simd_ch2_dual:
    entry   sp, 32

    mov.n   a7, a2              // sample_ptrs cursor
    extui   a8, a5, 20, 3       // Output count

.L_f32d2_outer:
    l32i.n  a2, a7, 0           // Delay line for this output
    extui   a9, a5, 0, 16
    srli    a9, a9, 1           // length / 2 (frames per iteration: 2)

    // Row A accumulators f0-f3 (L even, R even, L odd, R odd),
    // row B accumulators f4-f7
    const.s f0, 0
    const.s f1, 0
    const.s f2, 0
    const.s f3, 0
    const.s f4, 0
    const.s f5, 0
    const.s f6, 0
    const.s f7, 0

    loopnez a9, .L_f32d2_loop_end
        ee.ldf.64.ip    f9, f8, a2, 8       // Frame 0: L, R
        ee.ldf.64.ip    f11, f10, a2, 8     // Frame 1: L, R
        lsi     f12, a3, 0                  // Row A coefficients
        lsi     f13, a3, 4
        lsi     f14, a4, 0                  // Row B coefficients
        lsi     f15, a4, 4
        madd.s  f0, f8, f12
        madd.s  f1, f9, f12
        madd.s  f2, f10, f13
        madd.s  f3, f11, f13
        addi    a3, a3, 8
        madd.s  f4, f8, f14
        madd.s  f5, f9, f14
        madd.s  f6, f10, f15
        madd.s  f7, f11, f15
        addi    a4, a4, 8
.L_f32d2_loop_end:

    // Reduce each row in the single-output kernel's exact order
    add.s   f0, f0, f2
    add.s   f1, f1, f3
    add.s   f4, f4, f6
    add.s   f5, f5, f7

    // Store [row A L/R, row B L/R], rewind the coefficient cursors, next
    // output
    ssi     f0, a6, 0
    ssi     f1, a6, 4
    ssi     f4, a6, 8
    ssi     f5, a6, 12
    extui   a9, a5, 0, 16
    slli    a9, a9, 2
    sub     a3, a3, a9
    sub     a4, a4, a9
    addi.n  a6, a6, 16
    addi.n  a7, a7, 4
    addi.n  a8, a8, -1
    bnez    a8, .L_f32d2_outer

    retw.n

    .size   xtensa_dotprod_f32_simd_ch2_dual, . - xtensa_dotprod_f32_simd_ch2_dual

#endif // defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))
