// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 scalar implementation for Q31Lite 1-channel audio FIR filter
// Supports any length that is a multiple of 8
//
// Q31Lite has 32-bit coefficients, which PIE's 8/16-bit multiply lanes cannot
// take, so these are scalar RV32IM kernels built on the 32x32 high multiply
// (mulh). They beat the compiled C++ loop through an 8-tap unroll and
// load/multiply scheduling that keeps every loaded value and product at
// least one instruction group away from its consumer.
//
// Each tap computes acc += (int32_t)(((int64_t)sample * coeff) >> 32) with a
// wrapping add, bit-exact with MULT32_32_Q32 (detail/fixed_generic.h selects
// its exact int64 form on RISC-V, which GCC also lowers to a single mulh).
//
// Plain branch loops (no esp.* instructions); the exit test compares a
// cursor against its end, so no counter is kept.

#if defined(__riscv) && \
    (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31) || \
     defined(ESP32S31))

    .text
    .align 4

    // Function: riscv_dotprod_s32_q31lite_ch1
    // Computes dot product of Q31 samples and Q31 coefficients (1 channel)
    // Parameters:
    //   a0: samples (int32_t*) - any 4-byte alignment
    //   a1: coeffs (int32_t*) - any 4-byte alignment
    //   a2: length (must be multiple of 8)
    //   a3: output (int32_t*) - pointer to single Q30 accumulator output

    .global riscv_dotprod_s32_q31lite_ch1
    .type   riscv_dotprod_s32_q31lite_ch1, @function
riscv_dotprod_s32_q31lite_ch1:
    slli    a2, a2, 2
    add     a2, a2, a0          // a2 = sample end (loop exit address)
    li      a4, 0               // a4 = accumulator

.Lq1_loop:
    // Taps 0-1: load both pairs before the multiplies
    lw      t0, 0(a0)           // s0
    lw      t1, 0(a1)           // c0
    lw      t2, 4(a0)           // s1
    lw      t3, 4(a1)           // c1
    mulh    t0, t0, t1
    mulh    t2, t2, t3

    // Tap 2 loads cover tap 0's multiply before its accumulate
    lw      t4, 8(a0)           // s2
    lw      t5, 8(a1)           // c2
    add     a4, a4, t0
    mulh    t4, t4, t5

    // Tap 3
    lw      t6, 12(a0)          // s3
    lw      a5, 12(a1)          // c3
    add     a4, a4, t2
    mulh    t6, t6, a5

    // Tap 4 (registers from taps 0-1 are free again)
    lw      t0, 16(a0)          // s4
    lw      t1, 16(a1)          // c4
    add     a4, a4, t4
    mulh    t0, t0, t1

    // Tap 5
    lw      t2, 20(a0)          // s5
    lw      t3, 20(a1)          // c5
    add     a4, a4, t6
    mulh    t2, t2, t3

    // Tap 6
    lw      t4, 24(a0)          // s6
    lw      t5, 24(a1)          // c6
    add     a4, a4, t0
    mulh    t4, t4, t5

    // Tap 7
    lw      t6, 28(a0)          // s7
    lw      a5, 28(a1)          // c7
    add     a4, a4, t2
    mulh    t6, t6, a5

    // Advance pointers, then drain the last two products
    addi    a0, a0, 32
    addi    a1, a1, 32
    add     a4, a4, t4
    add     a4, a4, t6
    bne     a0, a2, .Lq1_loop

    sw      a4, 0(a3)
    ret

    .size   riscv_dotprod_s32_q31lite_ch1, . - riscv_dotprod_s32_q31lite_ch1

    // One tap of the dual-row kernel, software-pipelined across three
    // register sets: multiplies the sample \s by both rows' coefficients
    // \ca/\cb (products overwrite the coefficients), issues the next tap's
    // loads into \ns/\nca/\ncb from byte offset \off, and accumulates the
    // previous tap's products \pa/\pb into t1 (row A) and t2 (row B). The
    // load and accumulate operands are optional
    .macro Q31L_DUAL_TAP s, ca, cb, ns=, nca=, ncb=, off=0, pa=, pb=
    .ifnb \ns
    lw      \ns, \off(a6)
    .endif
    mulh    \ca, \s, \ca        // Row A product
    .ifnb \nca
    lw      \nca, \off(a7)
    .endif
    mulh    \cb, \s, \cb        // Row B product
    .ifnb \ncb
    lw      \ncb, \off(t0)
    .endif
    .ifnb \pa
    add     t1, t1, \pa
    add     t2, t2, \pb
    .endif
    .endm

    // Function: riscv_dotprod_s32_q31lite_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one call:
    // each sample is loaded once and multiplied against both coefficient
    // rows (the ASRC blend pair), amortizing the call and loop overhead that
    // dominates short filters. Tap arithmetic is identical to the
    // single-output kernel, so each accumulator is bit-exact with it.
    // Parameters:
    //   a0: sample_ptrs (const int32_t* const*) - one delay-line pointer per output
    //   a1: coeffs_a (int32_t*) - row A coefficients
    //   a2: coeffs_b (int32_t*) - row B coefficients
    //   a3: length (bits 0-15, must be multiple of 8) | output count (bits 20-22, 1-4)
    //   a4: output (int32_t*) - [row A, row B] Q30 accumulator pair per output
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.
    // Frame: sp+0.. saved s0-s4

    .align  4
    .global riscv_dotprod_s32_q31lite_ch1_dual
    .type   riscv_dotprod_s32_q31lite_ch1_dual, @function
riscv_dotprod_s32_q31lite_ch1_dual:
    addi    sp, sp, -32
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    sw      s2, 8(sp)
    sw      s3, 12(sp)
    sw      s4, 16(sp)

    slli    a5, a3, 16
    srli    a5, a5, 14          // a5 = length * 4 (row bytes)
    add     a5, a5, a1          // a5 = row A end (loop exit address)
    srli    a3, a3, 20
    andi    a3, a3, 7           // a3 = outputs remaining

.Lq1d_output:
    lw      a6, 0(a0)           // a6 = delay line for this output
    addi    a0, a0, 4
    mv      a7, a1              // a7 = row A cursor
    mv      t0, a2              // t0 = row B cursor
    li      t1, 0               // t1 = row A accumulator
    li      t2, 0               // t2 = row B accumulator

    // Three register sets rotate through the 8 taps (X = t3-t5,
    // Y = t6/s0/s1, Z = s2-s4): each tap's loads issue one tap ahead and its
    // accumulates one tap behind
.Lq1d_loop:
    lw      t3, 0(a6)           // Tap 0 sample
    lw      t4, 0(a7)           // Tap 0 row A coefficient
    lw      t5, 0(t0)           // Tap 0 row B coefficient
    Q31L_DUAL_TAP t3, t4, t5, ns=t6, nca=s0, ncb=s1, off=4
    Q31L_DUAL_TAP t6, s0, s1, ns=s2, nca=s3, ncb=s4, off=8, pa=t4, pb=t5
    Q31L_DUAL_TAP s2, s3, s4, ns=t3, nca=t4, ncb=t5, off=12, pa=s0, pb=s1
    Q31L_DUAL_TAP t3, t4, t5, ns=t6, nca=s0, ncb=s1, off=16, pa=s3, pb=s4
    Q31L_DUAL_TAP t6, s0, s1, ns=s2, nca=s3, ncb=s4, off=20, pa=t4, pb=t5
    Q31L_DUAL_TAP s2, s3, s4, ns=t3, nca=t4, ncb=t5, off=24, pa=s0, pb=s1
    Q31L_DUAL_TAP t3, t4, t5, ns=t6, nca=s0, ncb=s1, off=28, pa=s3, pb=s4
    Q31L_DUAL_TAP t6, s0, s1, pa=t4, pb=t5
    addi    a6, a6, 32
    addi    a7, a7, 32
    addi    t0, t0, 32
    add     t1, t1, s0          // Drain tap 7
    add     t2, t2, s1
    bne     a7, a5, .Lq1d_loop

    // Store [row A, row B], next output
    sw      t1, 0(a4)
    sw      t2, 4(a4)
    addi    a4, a4, 8
    addi    a3, a3, -1
    bnez    a3, .Lq1d_output

    lw      s0, 0(sp)
    lw      s1, 4(sp)
    lw      s2, 8(sp)
    lw      s3, 12(sp)
    lw      s4, 16(sp)
    addi    sp, sp, 32
    ret

    .size   riscv_dotprod_s32_q31lite_ch1_dual, . - riscv_dotprod_s32_q31lite_ch1_dual

#endif
