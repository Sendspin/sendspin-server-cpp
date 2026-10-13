// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 scalar implementation for Q31Lite 2-channel audio FIR filter
// Supports any length that is a multiple of 8
//
// Same per-tap arithmetic as riscv_dotprod_p4_q31lite_ch1.S (see there):
// acc += (int32_t)(((int64_t)sample * coeff) >> 32) via mulh, with wrapping
// adds, bit-exact with MULT32_32_Q32. Each coefficient is loaded once and
// shared by both channels.

#if defined(__riscv) && \
    (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31) || \
     defined(ESP32S31))

    .text
    .align 4

    // One tap of the single-output stereo kernel, software-pipelined across
    // three register sets: multiplies the samples \l/\r by the coefficient
    // \c (products overwrite the samples), issues the next tap's loads into
    // \nl/\nr/\nc (sample byte offset \soff, coefficient byte offset \coff),
    // and accumulates the previous tap's products \pl/\pr into a4 (left) and
    // a5 (right). The load and accumulate operands are optional
    .macro Q31L_STEREO_TAP l, r, c, nl=, nr=, nc=, soff=0, coff=0, pl=, pr=
    .ifnb \nl
    lw      \nl, \soff(a0)
    .endif
    mulh    \l, \l, \c          // Left product
    .ifnb \nr
    lw      \nr, \soff+4(a0)
    .endif
    mulh    \r, \r, \c          // Right product
    .ifnb \nc
    lw      \nc, \coff(a1)
    .endif
    .ifnb \pl
    add     a4, a4, \pl
    add     a5, a5, \pr
    .endif
    .endm

    // Function: riscv_dotprod_s32_q31lite_ch2
    // Computes dot product of Q31 samples and Q31 coefficients (2 channels)
    // Parameters:
    //   a0: samples (int32_t*) - interleaved L/R, any 4-byte alignment
    //   a1: coeffs (int32_t*) - any 4-byte alignment
    //   a2: length (must be multiple of 8)
    //   a3: output (int32_t*) - [left, right] Q30 accumulators

    .global riscv_dotprod_s32_q31lite_ch2
    .type   riscv_dotprod_s32_q31lite_ch2, @function
riscv_dotprod_s32_q31lite_ch2:
    slli    a2, a2, 2
    add     a2, a2, a1          // a2 = coefficient end (loop exit address)
    li      a4, 0               // a4 = left accumulator
    li      a5, 0               // a5 = right accumulator

    // Three register sets rotate through the 8 taps (X = t0-t2, Y = t3-t5,
    // Z = t6/a6/a7): each tap's loads issue one tap ahead and its
    // accumulates one tap behind
.Lq2_loop:
    lw      t0, 0(a0)           // Tap 0 left sample
    lw      t1, 4(a0)           // Tap 0 right sample
    lw      t2, 0(a1)           // Tap 0 coefficient
    Q31L_STEREO_TAP t0, t1, t2, nl=t3, nr=t4, nc=t5, soff=8, coff=4
    Q31L_STEREO_TAP t3, t4, t5, nl=t6, nr=a6, nc=a7, soff=16, coff=8, pl=t0, pr=t1
    Q31L_STEREO_TAP t6, a6, a7, nl=t0, nr=t1, nc=t2, soff=24, coff=12, pl=t3, pr=t4
    Q31L_STEREO_TAP t0, t1, t2, nl=t3, nr=t4, nc=t5, soff=32, coff=16, pl=t6, pr=a6
    Q31L_STEREO_TAP t3, t4, t5, nl=t6, nr=a6, nc=a7, soff=40, coff=20, pl=t0, pr=t1
    Q31L_STEREO_TAP t6, a6, a7, nl=t0, nr=t1, nc=t2, soff=48, coff=24, pl=t3, pr=t4
    Q31L_STEREO_TAP t0, t1, t2, nl=t3, nr=t4, nc=t5, soff=56, coff=28, pl=t6, pr=a6
    Q31L_STEREO_TAP t3, t4, t5, pl=t0, pr=t1
    addi    a0, a0, 64
    addi    a1, a1, 32
    add     a4, a4, t3          // Drain tap 7
    add     a5, a5, t4
    bne     a1, a2, .Lq2_loop

    sw      a4, 0(a3)
    sw      a5, 4(a3)
    ret

    .size   riscv_dotprod_s32_q31lite_ch2, . - riscv_dotprod_s32_q31lite_ch2

    // One tap of the dual-row stereo kernel: multiplies both channels'
    // samples \l/\r by both rows' coefficients \ca/\cb (products land in t2
    // and the tap's own registers), issuing the next tap's loads into
    // \nl/\nr/\nca/\ncb (sample byte offset \soff, coefficient byte offset
    // \coff) between the multiplies, then accumulates into a6/a7 (row A
    // left/right) and t0/t1 (row B left/right). The loads are optional
    .macro Q31L_STEREO_DUAL_TAP l, r, ca, cb, nl=, nr=, nca=, ncb=, soff=0, coff=0
    mulh    t2, \l, \ca         // Row A left
    .ifnb \nl
    lw      \nl, \soff(a0)
    .endif
    mulh    \ca, \r, \ca        // Row A right
    .ifnb \nr
    lw      \nr, \soff+4(a0)
    .endif
    mulh    \l, \l, \cb         // Row B left
    .ifnb \nca
    lw      \nca, \coff(a3)
    .endif
    mulh    \r, \r, \cb         // Row B right
    .ifnb \ncb
    lw      \ncb, \coff(a4)
    .endif
    add     a6, a6, t2
    add     a7, a7, \ca
    add     t0, t0, \l
    add     t1, t1, \r
    .endm

    // Function: riscv_dotprod_s32_q31lite_ch2_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one call:
    // each stereo sample pair is loaded once and multiplied against both
    // coefficient rows (the ASRC blend pair). Tap arithmetic is identical to
    // the single-output kernel, so each accumulator is bit-exact with it.
    // Parameters:
    //   a0: sample_ptrs (const int32_t* const*) - one delay-line pointer per output
    //   a1: coeffs_a (int32_t*) - row A coefficients
    //   a2: coeffs_b (int32_t*) - row B coefficients
    //   a3: length (bits 0-15, must be multiple of 8) | output count (bits 20-22, 1-4)
    //   a4: output (int32_t*) - [row A L, row A R, row B L, row B R] per output
    //
    // The row pair is FIXED across the batch; only the sample pointer
    // differs per output.
    // Frame: sp+0.. saved s0-s3, sp+16 sample_ptrs cursor, sp+20 output
    // cursor, sp+24 outputs remaining

    .align  4
    .global riscv_dotprod_s32_q31lite_ch2_dual
    .type   riscv_dotprod_s32_q31lite_ch2_dual, @function
riscv_dotprod_s32_q31lite_ch2_dual:
    addi    sp, sp, -32
    sw      s0, 0(sp)
    sw      s1, 4(sp)
    sw      s2, 8(sp)
    sw      s3, 12(sp)

    slli    a5, a3, 16
    srli    a5, a5, 14          // a5 = length * 4 (row bytes)
    add     a5, a5, a1          // a5 = row A end (loop exit address)
    srli    a3, a3, 20
    andi    a3, a3, 7
    sw      a0, 16(sp)          // sample_ptrs cursor
    sw      a4, 20(sp)          // Output cursor
    sw      a3, 24(sp)          // Outputs remaining

.Lq2d_output:
    lw      t2, 16(sp)
    lw      a0, 0(t2)           // a0 = delay line for this output
    addi    t2, t2, 4
    sw      t2, 16(sp)
    mv      a3, a1              // a3 = row A cursor
    mv      a4, a2              // a4 = row B cursor
    li      a6, 0               // a6 = row A left accumulator
    li      a7, 0               // a7 = row A right accumulator
    li      t0, 0               // t0 = row B left accumulator
    li      t1, 0               // t1 = row B right accumulator

    // Two register sets alternate through the 8 taps (X = t3-t6,
    // Y = s0-s3); each tap issues the next tap's loads into the idle set
.Lq2d_loop:
    lw      t3, 0(a0)           // Tap 0 left sample
    lw      t4, 4(a0)           // Tap 0 right sample
    lw      t5, 0(a3)           // Tap 0 row A coefficient
    lw      t6, 0(a4)           // Tap 0 row B coefficient
    Q31L_STEREO_DUAL_TAP t3, t4, t5, t6, nl=s0, nr=s1, nca=s2, ncb=s3, soff=8, coff=4
    Q31L_STEREO_DUAL_TAP s0, s1, s2, s3, nl=t3, nr=t4, nca=t5, ncb=t6, soff=16, coff=8
    Q31L_STEREO_DUAL_TAP t3, t4, t5, t6, nl=s0, nr=s1, nca=s2, ncb=s3, soff=24, coff=12
    Q31L_STEREO_DUAL_TAP s0, s1, s2, s3, nl=t3, nr=t4, nca=t5, ncb=t6, soff=32, coff=16
    Q31L_STEREO_DUAL_TAP t3, t4, t5, t6, nl=s0, nr=s1, nca=s2, ncb=s3, soff=40, coff=20
    Q31L_STEREO_DUAL_TAP s0, s1, s2, s3, nl=t3, nr=t4, nca=t5, ncb=t6, soff=48, coff=24
    Q31L_STEREO_DUAL_TAP t3, t4, t5, t6, nl=s0, nr=s1, nca=s2, ncb=s3, soff=56, coff=28
    addi    a0, a0, 64
    addi    a3, a3, 32
    addi    a4, a4, 32
    Q31L_STEREO_DUAL_TAP s0, s1, s2, s3
    bne     a3, a5, .Lq2d_loop

    // Store [row A L, row A R, row B L, row B R], next output
    lw      t2, 20(sp)
    sw      a6, 0(t2)
    sw      a7, 4(t2)
    sw      t0, 8(t2)
    sw      t1, 12(t2)
    addi    t2, t2, 16
    sw      t2, 20(sp)
    lw      t2, 24(sp)
    addi    t2, t2, -1
    sw      t2, 24(sp)
    bnez    t2, .Lq2d_output

    lw      s0, 0(sp)
    lw      s1, 4(sp)
    lw      s2, 8(sp)
    lw      s3, 12(sp)
    addi    sp, sp, 32
    ret

    .size   riscv_dotprod_s32_q31lite_ch2_dual, . - riscv_dotprod_s32_q31lite_ch2_dual

#endif
