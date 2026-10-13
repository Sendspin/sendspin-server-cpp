// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// Xtensa scalar implementation for Q31Lite 1-channel audio FIR filter
// Supports any length that is a multiple of 8
//
// Q31Lite has 32-bit coefficients, so the S3's 16-bit SIMD lanes cannot be
// used; instead this kernel relies on the scalar 32x32 high multiply (mulsh),
// which both the LX6 (ESP32) and LX7 (ESP32-S3) cores provide. It beats the
// compiled C++ loop by using the zero-overhead loop instruction (blocked for
// the compiler by the inline-asm mulsh in detail/xtensa/fixed_lx6.h), an 8-tap
// unroll, and load/multiply scheduling that hides the multiplier latency.
//
// Each tap computes acc += (int32_t)(((int64_t)sample * coeff) >> 32),
// bit-exact with MULT32_32_Q32 in both its inline-asm and int64 forms.

#if defined(__XTENSA__)

    .text
    .align 4

    // Function: xtensa_dotprod_s32_q31lite_ch1
    // Computes dot product of Q31 samples and Q31 coefficients (1 channel)
    // Parameters:
    //   a2: samples (int32_t*) - input samples
    //   a3: coeffs (int32_t*) - filter coefficients
    //   a4: length (int) - must be multiple of 8
    //   a5: output (int32_t*) - pointer to single Q30 accumulator output

    .global xtensa_dotprod_s32_q31lite_ch1
    .type   xtensa_dotprod_s32_q31lite_ch1, @function
xtensa_dotprod_s32_q31lite_ch1:
    entry   sp, 32

    movi.n  a15, 0              // Accumulator
    srli    a8, a4, 3           // a8 = length / 8 (taps per iteration: 8)

    loopnez a8, .L_q31lite_ch1_loop_end
        // Taps 0-1: load both pairs before the multiplies so each mulsh
        // result has 3+ instructions before its accumulate consumes it
        l32i.n  a6, a2, 0           // s0
        l32i.n  a7, a3, 0           // c0
        l32i.n  a9, a2, 4           // s1
        l32i.n  a10, a3, 4          // c1
        mulsh   a6, a6, a7
        mulsh   a9, a9, a10

        // Tap 2 loads cover the latency of tap 0's accumulate
        l32i.n  a11, a2, 8          // s2
        l32i.n  a12, a3, 8          // c2
        add.n   a15, a15, a6
        mulsh   a11, a11, a12

        // Tap 3
        l32i.n  a13, a2, 12         // s3
        l32i.n  a14, a3, 12         // c3
        add.n   a15, a15, a9
        mulsh   a13, a13, a14

        // Tap 4 (registers from taps 0-1 are free again)
        l32i.n  a6, a2, 16          // s4
        l32i.n  a7, a3, 16          // c4
        add.n   a15, a15, a11
        mulsh   a6, a6, a7

        // Tap 5
        l32i.n  a9, a2, 20          // s5
        l32i.n  a10, a3, 20         // c5
        add.n   a15, a15, a13
        mulsh   a9, a9, a10

        // Tap 6
        l32i.n  a11, a2, 24         // s6
        l32i.n  a12, a3, 24         // c6
        add.n   a15, a15, a6
        mulsh   a11, a11, a12

        // Tap 7
        l32i.n  a13, a2, 28         // s7
        l32i.n  a14, a3, 28         // c7
        add.n   a15, a15, a9
        mulsh   a13, a13, a14

        // Advance pointers, then drain the last two products
        addi    a2, a2, 32
        addi    a3, a3, 32
        add.n   a15, a15, a11
        add.n   a15, a15, a13
.L_q31lite_ch1_loop_end:

    s32i.n  a15, a5, 0

    retw.n

    .size   xtensa_dotprod_s32_q31lite_ch1, . - xtensa_dotprod_s32_q31lite_ch1

    // Function: xtensa_dotprod_s32_q31lite_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one call:
    // each sample is loaded once and multiplied against both coefficient rows
    // (the ASRC blend pair), amortizing the call and loop overhead that
    // dominates short filters. Tap arithmetic is identical to the
    // single-output kernel, so each accumulator is bit-exact with it.
    // Parameters:
    //   a2: sample_ptrs (const int32_t* const*) - one delay-line pointer per output
    //   a3: coeffs_a (int32_t*) - row A coefficients
    //   a4: coeffs_b (int32_t*) - row B coefficients
    //   a5: length (bits 0-15, must be multiple of 8) | output count (bits 20-22, 1-4)
    //   a6: output (int32_t*) - [row A, row B] Q30 accumulator pair per output
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.

    .align  4
    .global xtensa_dotprod_s32_q31lite_ch1_dual
    .type   xtensa_dotprod_s32_q31lite_ch1_dual, @function
xtensa_dotprod_s32_q31lite_ch1_dual:
    entry   sp, 32

    mov.n   a7, a2              // sample_ptrs cursor
    extui   a8, a5, 20, 3       // Output count

.L_q31d1_outer:
    l32i.n  a2, a7, 0           // Delay line for this output
    extui   a9, a5, 0, 16
    srli    a9, a9, 3           // length / 8 (taps per iteration: 8)
    movi.n  a14, 0              // Row A accumulator
    movi.n  a15, 0              // Row B accumulator

    loopnez a9, .L_q31d1_loop_end
        // Taps 0-1: load sample and both rows' coefficients, keeping 3+
        // instructions between each mulsh and its accumulate
        l32i.n  a9, a2, 0           // s0
        l32i.n  a10, a3, 0          // cA0
        l32i.n  a11, a4, 0          // cB0
        l32i.n  a12, a2, 4          // s1
        mulsh   a10, a9, a10
        mulsh   a11, a9, a11
        l32i.n  a9, a3, 4           // cA1
        l32i.n  a13, a4, 4          // cB1
        add.n   a14, a14, a10
        add.n   a15, a15, a11
        mulsh   a9, a12, a9
        mulsh   a13, a12, a13

        // Taps 2-3 loads cover the taps 0-1 accumulates
        l32i.n  a10, a2, 8          // s2
        l32i.n  a11, a3, 8          // cA2
        l32i.n  a12, a4, 8          // cB2
        add.n   a14, a14, a9
        add.n   a15, a15, a13
        mulsh   a11, a10, a11
        mulsh   a12, a10, a12
        l32i.n  a9, a3, 12          // cA3
        l32i.n  a13, a4, 12         // cB3
        l32i.n  a10, a2, 12         // s3
        add.n   a14, a14, a11
        add.n   a15, a15, a12
        mulsh   a9, a10, a9
        mulsh   a13, a10, a13

        // Taps 4-5
        l32i.n  a10, a2, 16         // s4
        l32i.n  a11, a3, 16         // cA4
        l32i.n  a12, a4, 16         // cB4
        add.n   a14, a14, a9
        add.n   a15, a15, a13
        mulsh   a11, a10, a11
        mulsh   a12, a10, a12
        l32i.n  a9, a3, 20          // cA5
        l32i.n  a13, a4, 20         // cB5
        l32i.n  a10, a2, 20         // s5
        add.n   a14, a14, a11
        add.n   a15, a15, a12
        mulsh   a9, a10, a9
        mulsh   a13, a10, a13

        // Taps 6-7
        l32i.n  a10, a2, 24         // s6
        l32i.n  a11, a3, 24         // cA6
        l32i.n  a12, a4, 24         // cB6
        add.n   a14, a14, a9
        add.n   a15, a15, a13
        mulsh   a11, a10, a11
        mulsh   a12, a10, a12
        l32i.n  a9, a3, 28          // cA7
        l32i.n  a13, a4, 28         // cB7
        l32i.n  a10, a2, 28         // s7
        add.n   a14, a14, a11
        add.n   a15, a15, a12
        mulsh   a9, a10, a9
        mulsh   a13, a10, a13

        // Advance pointers, then drain the last two products
        addi    a2, a2, 32
        addi    a3, a3, 32
        addi    a4, a4, 32
        add.n   a14, a14, a9
        add.n   a15, a15, a13
.L_q31d1_loop_end:

    // Store [row A, row B], rewind the coefficient cursors, next output
    s32i.n  a14, a6, 0
    s32i.n  a15, a6, 4
    extui   a9, a5, 0, 16
    slli    a9, a9, 2
    sub     a3, a3, a9
    sub     a4, a4, a9
    addi.n  a6, a6, 8
    addi.n  a7, a7, 4
    addi.n  a8, a8, -1
    bnez    a8, .L_q31d1_outer

    retw.n

    .size   xtensa_dotprod_s32_q31lite_ch1_dual, . - xtensa_dotprod_s32_q31lite_ch1_dual

#endif
