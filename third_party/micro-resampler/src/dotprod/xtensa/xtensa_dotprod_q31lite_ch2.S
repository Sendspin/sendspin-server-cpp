// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// Xtensa scalar implementation for Q31Lite 2-channel audio FIR filter
// Supports any length that is a multiple of 8
//
// Q31Lite has 32-bit coefficients, so the S3's 16-bit SIMD lanes cannot be
// used; instead this kernel relies on the scalar 32x32 high multiply (mulsh),
// which both the LX6 (ESP32) and LX7 (ESP32-S3) cores provide. Samples are
// interleaved stereo frames; each coefficient is loaded once and applied to
// both channels. The zero-overhead loop, 4-tap unroll, and load/multiply
// scheduling mirror the 1-channel kernel.
//
// Each tap computes acc[ch] += (int32_t)(((int64_t)sample[ch] * coeff) >> 32),
// bit-exact with MULT32_32_Q32 in both its inline-asm and int64 forms.

#if defined(__XTENSA__)

    .text
    .align 4

    // Function: xtensa_dotprod_s32_q31lite_ch2
    // Computes dot product of interleaved stereo Q31 samples and Q31
    // coefficients (2 channels)
    // Parameters:
    //   a2: samples (int32_t*) - interleaved stereo input samples (L,R)
    //   a3: coeffs (int32_t*) - filter coefficients
    //   a4: length (int) - taps per channel, must be multiple of 8
    //   a5: output (int32_t*) - pointer to two Q30 accumulator outputs

    .global xtensa_dotprod_s32_q31lite_ch2
    .type   xtensa_dotprod_s32_q31lite_ch2, @function
xtensa_dotprod_s32_q31lite_ch2:
    entry   sp, 32

    movi.n  a14, 0              // Left accumulator
    movi.n  a15, 0              // Right accumulator
    srli    a8, a4, 2           // a8 = length / 4 (taps per iteration: 4)

    loopnez a8, .L_q31lite_ch2_loop_end
        // Tap 0: coefficient plus both channels, then next coefficient
        // before the multiplies so every mulsh result rests 3+ instructions
        l32i.n  a6, a3, 0           // c0
        l32i.n  a7, a2, 0           // s0 left
        l32i.n  a9, a2, 4           // s0 right
        l32i.n  a10, a3, 4          // c1
        mulsh   a7, a7, a6
        mulsh   a9, a9, a6

        // Tap 1 sample loads cover the latency of tap 0's accumulates
        l32i.n  a11, a2, 8          // s1 left
        l32i.n  a12, a2, 12         // s1 right
        add.n   a14, a14, a7
        mulsh   a11, a11, a10
        add.n   a15, a15, a9
        mulsh   a12, a12, a10

        // Tap 2 (registers from tap 0 are free again)
        l32i.n  a6, a3, 8           // c2
        l32i.n  a7, a2, 16          // s2 left
        l32i.n  a9, a2, 20          // s2 right
        add.n   a14, a14, a11
        mulsh   a7, a7, a6
        add.n   a15, a15, a12
        mulsh   a9, a9, a6

        // Tap 3
        l32i.n  a10, a3, 12         // c3
        l32i.n  a11, a2, 24         // s3 left
        l32i.n  a12, a2, 28         // s3 right
        add.n   a14, a14, a7
        mulsh   a11, a11, a10
        add.n   a15, a15, a9
        mulsh   a12, a12, a10

        // Advance pointers, then drain the last two products
        addi    a2, a2, 32
        addi    a3, a3, 16
        add.n   a14, a14, a11
        add.n   a15, a15, a12
.L_q31lite_ch2_loop_end:

    s32i.n  a14, a5, 0
    s32i.n  a15, a5, 4

    retw.n

    .size   xtensa_dotprod_s32_q31lite_ch2, . - xtensa_dotprod_s32_q31lite_ch2

    // Function: xtensa_dotprod_s32_q31lite_ch2_dual
    // Computes BOTH blend rows' stereo dot products for up to 4 outputs in
    // one call: each stereo frame is loaded once and multiplied against both
    // coefficient rows (the ASRC blend pair), amortizing the call and loop
    // overhead that dominates short filters. Tap arithmetic is identical to
    // the single-output kernel, so each accumulator is bit-exact with it.
    // Parameters:
    //   a2: sample_ptrs (const int32_t* const*) - one delay-line pointer per output
    //   a3: coeffs_a (int32_t*) - row A coefficients
    //   a4: coeffs_b (int32_t*) - row B coefficients
    //   a5: length (bits 0-15, taps per channel, must be multiple of 8) |
    //       output count (bits 20-22, 1-4)
    //   a6: output (int32_t*) - [row A left, row A right, row B left,
    //       row B right] Q30 accumulators per output
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.

    .align  4
    .global xtensa_dotprod_s32_q31lite_ch2_dual
    .type   xtensa_dotprod_s32_q31lite_ch2_dual, @function
xtensa_dotprod_s32_q31lite_ch2_dual:
    entry   sp, 32

    // Four live accumulators leave too few temporaries for the frame
    // pipeline, so the packed word and output count live in locals
    mov.n   a7, a2              // sample_ptrs cursor
    extui   a8, a5, 20, 3       // Output count
    s32i    a5, sp, 0           // Packed length/count word
    s32i    a8, sp, 4           // Outputs remaining

.L_q31d2_outer:
    l32i.n  a2, a7, 0           // Delay line for this output
    l32i.n  a5, sp, 0
    extui   a5, a5, 0, 16
    srli    a5, a5, 1           // length / 2 (frames per iteration: 2)
    movi.n  a12, 0              // Row A left accumulator
    movi.n  a13, 0              // Row A right accumulator
    movi.n  a14, 0              // Row B left accumulator
    movi.n  a15, 0              // Row B right accumulator

    loopnez a5, .L_q31d2_loop_end
        // Frame 0: both channels against both rows' coefficients
        l32i.n  a8, a2, 0           // s0 left
        l32i.n  a9, a3, 0           // cA0
        l32i.n  a10, a2, 4          // s0 right
        l32i.n  a11, a4, 0          // cB0
        mulsh   a5, a8, a9          // Row A left
        mulsh   a9, a10, a9         // Row A right
        mulsh   a8, a8, a11         // Row B left
        mulsh   a11, a10, a11       // Row B right

        // Frame 1 loads cover the latency of frame 0's accumulates
        l32i.n  a10, a2, 8          // s1 left
        add.n   a12, a12, a5
        add.n   a13, a13, a9
        l32i.n  a5, a3, 4           // cA1
        l32i.n  a9, a2, 12          // s1 right
        add.n   a14, a14, a8
        add.n   a15, a15, a11
        l32i.n  a11, a4, 4          // cB1
        mulsh   a8, a10, a5         // Row A left
        mulsh   a5, a9, a5          // Row A right
        mulsh   a10, a10, a11       // Row B left
        mulsh   a11, a9, a11        // Row B right

        // Advance pointers, then drain the last four products
        addi    a2, a2, 16
        addi    a3, a3, 8
        addi    a4, a4, 8
        add.n   a12, a12, a8
        add.n   a13, a13, a5
        add.n   a14, a14, a10
        add.n   a15, a15, a11
.L_q31d2_loop_end:

    // Store [row A L/R, row B L/R], rewind the coefficient cursors, next output
    s32i.n  a12, a6, 0
    s32i.n  a13, a6, 4
    s32i.n  a14, a6, 8
    s32i.n  a15, a6, 12
    l32i.n  a5, sp, 0
    extui   a5, a5, 0, 16
    slli    a5, a5, 2
    sub     a3, a3, a5
    sub     a4, a4, a5
    l32i.n  a8, sp, 4
    addi.n  a6, a6, 16
    addi.n  a7, a7, 4
    addi.n  a8, a8, -1
    s32i    a8, sp, 4
    bnez    a8, .L_q31d2_outer

    retw.n

    .size   xtensa_dotprod_s32_q31lite_ch2_dual, . - xtensa_dotprod_s32_q31lite_ch2_dual

#endif
