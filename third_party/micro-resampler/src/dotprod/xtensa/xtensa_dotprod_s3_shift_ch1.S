// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

// ESP32-S3 SIMD implementation for 1-channel audio FIR filter
// Supports any length that is a multiple of 8

#if defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

    // Saturate a folded max-tap sum once, matching apply_max_taps(): \acc holds the wrapped
    // \base + tap_sum, and the tap sum itself cannot wrap (FilterDesign caps the tap count; see
    // dotprod_maxtap.h), so the add overflowed iff \base and the tap sum share a sign that
    // \acc does not. Clobbers \t0-\t2
    .macro MAXTAP_SATURATE acc, base, t0, t1, t2
    sub     \t0, \acc, \base       // tap sum
    xor     \t1, \base, \acc
    xor     \t0, \t0, \acc
    and     \t1, \t1, \t0          // negative iff the add overflowed
    srai    \t0, \base, 31          // 0 or -1 by the sign of base
    movi.n  \t2, -1
    srli    \t2, \t2, 1             // INT32_MAX
    xor     \t2, \t2, \t0          // INT32_MAX or INT32_MIN
    movltz  \acc, \t2, \t1
    .endm

    .text
    .align 4

    // Function: xtensa_dotprod_s16_q15_shift_simd_ch1
    // Computes dot product of int16 samples and Q15 coefficients using SIMD (1 channel),
    // then folds in the pre-extracted max-tap MACs while the accumulator is still live
    // Parameters:
    //   a2: delay_line (int16_t*) - any alignment supported
    //   a3: coeffs (q15_t*) - must be 16-byte aligned
    //   a4: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19)
    //   a5: shifts (uint8_t*) - shifts for each block of 8 samples
    //   a6: output (int32_t*) - pointer to single int32 output
    //   a7: max_tap_entries - 8-byte {int32 coeff, uint16 index_reversed, pad} records,
    //       ignored when the packed tap count is 0

    .global xtensa_dotprod_s16_q15_shift_simd_ch1
    .type   xtensa_dotprod_s16_q15_shift_simd_ch1, @function
xtensa_dotprod_s16_q15_shift_simd_ch1:
    entry   sp, 32

    // Initialize accumulator
    movi.n  a12, 0              // Single channel accumulator

    // Unpack args; keep the sample base for max-tap indexing (a2 advances)
    mov.n   a14, a2             // a14 = delay_line base
    extui   a15, a4, 16, 4      // a15 = max tap count (0-6)
    extui   a4, a4, 0, 16       // a4 = filter length

    // Calculate loop count for 32-sample batches
    srli    a8, a4, 5           // a8 = length / 32

    // Preload samples from potentially misaligned delay_line
    ee.ld.128.usar.ip  q0, a2, 16       // Setup SAR, load raw into q0
    ee.vld.128.ip      q1, a2, 16       // Load raw into q1
    ee.src.q.ld.ip     q2, a2, 16, q0, q1   // q0 = aligned, load raw into q2
    ee.src.q.ld.ip     q3, a2, 16, q1, q2   // q1 = aligned, load raw into q3
    // State: q0,q1 = aligned samples; q2,q3 = raw queue data

    l8ui            a9, a5, 0       // Load shift for batch 0
    ee.vld.128.ip   q4, a3, 16      // Preload first coefficients
    ee.zero.accx                    // Clear vector accumulator

    // Remaining batches: (length % 32) / 8 (length in a4 is dead after this)
    extui   a4, a4, 3, 2            // a4 = 0-3 remaining batches

    // Skip main loop if fewer than 32 samples
    beqz    a8, .L_ch1_remainder

    // Main loop (32 samples per iteration = 4 batches of 8)
    loopnez a8, .L_ch1_loop_end
.L_ch1_loop:
    // Batch 0: q0 × q4, queue(q2,q3) -> q2 aligned, q0 raw
    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q4, q2, q3
    l8ui a10, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    // Batch 1: q1 × q5, queue(q3,q0) -> q3 aligned, q1 raw
    ee.vmulas.s16.accx.ld.ip.qup   q1, a2, 16, q1, q5, q3, q0
    l8ui a9, a5, 2
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11
    // State: q2,q3 = aligned; q0,q1 = raw

    // Batch 2: q2 × q4, queue(q0,q1) -> q0 aligned, q2 raw
    ee.vmulas.s16.accx.ld.ip.qup   q2, a2, 16, q2, q4, q0, q1
    l8ui a10, a5, 3
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    // Batch 3: q3 × q5, queue(q1,q2) -> q1 aligned, q3 raw
    ee.vmulas.s16.accx.ld.ip.qup   q3, a2, 16, q3, q5, q1, q2
    addi.n a5, a5, 4
    l8ui a9, a5, 0
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11
    // State: q0,q1 = aligned; q2,q3 = raw (back to initial state)

.L_ch1_loop_end:

.L_ch1_remainder:

    beqz    a4, .L_ch1_done
    addi    a4, a4, -1

    // Remainder batch 0
    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q4, q2, q3
    l8ui a10, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    beqz    a4, .L_ch1_done
    addi    a4, a4, -1

    // Remainder batch 1
    ee.vmulas.s16.accx   q1, q5
    l8ui a9, a5, 2
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11

    beqz    a4, .L_ch1_done

    // Remainder batch 2
    ee.vmulas.s16.accx   q2, q4
    ee.srs.accx a11, a9, 0
    ee.zero.accx
    add a12, a12, a11

.L_ch1_done:
    // Fold in the pre-extracted max taps: acc += (coeff * (sample << 16)) >> 32
    // per entry, then saturate once, matching apply_max_taps() exactly (sample
    // low bits are zero, so the single mulsh is bit-exact)
    beqz    a15, .L_ch1_store
    mov.n   a8, a12             // a8 = dot product before the max taps
    loopnez a15, .L_ch1_maxtap_end
.L_ch1_maxtap:
    l16ui   a9, a7, 4           // index_reversed
    l32i    a10, a7, 0          // Q31 coefficient
    addx2   a9, a9, a14         // &delay_line[index]
    addi.n  a7, a7, 8
    l16si   a9, a9, 0           // sample
    slli    a9, a9, 16          // promote to Q31
    mulsh   a9, a10, a9         // high 32 bits of coeff * sample_q31
    add.n   a12, a12, a9
.L_ch1_maxtap_end:
    MAXTAP_SATURATE a12, a8, a9, a10, a11

.L_ch1_store:
    s32i a12, a6, 0

    retw.n

    .size   xtensa_dotprod_s16_q15_shift_simd_ch1, . - xtensa_dotprod_s16_q15_shift_simd_ch1

    // Function: xtensa_dotprod_s16_q15_shift_simd_ch1_batch
    // Runs the ch1 shift kernel body for up to 4 consecutive outputs in one
    // call, amortizing the call and dispatch overhead across the batch
    // Parameters:
    //   a2: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a3: coeffs (q15_t*) - first row, must be 16-byte aligned
    //   a4: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4) | fixed-row flag (bit 23)
    //   a5: shifts (uint8_t*) - shifts for the first row
    //   a6: output (int32_t*) - one int32 output per batch entry
    //   a7: max_tap_entries - 8-byte records for the first row, ignored when
    //       the packed tap count is 0
    //
    // With the fixed-row flag clear the coefficient, shift, and max-tap rows
    // advance one bank row per output (the aligned fixed-mode visit order is
    // sequential); with it set every output reuses the first row
    // (single-phase banks)

    .align  4
    .global xtensa_dotprod_s16_q15_shift_simd_ch1_batch
    .type   xtensa_dotprod_s16_q15_shift_simd_ch1_batch, @function
xtensa_dotprod_s16_q15_shift_simd_ch1_batch:
    entry   sp, 48

    // Outer-loop state, spilled to locals (sp+0..31; sp+32..47 is the base
    // save area): the kernel body clobbers every register except a6/a7
    extui   a8, a4, 20, 3
    s32i    a4, sp, 0           // Packed length/taps/flags word
    s32i    a2, sp, 4           // sample_ptrs cursor
    s32i    a3, sp, 8           // Coefficient row
    s32i    a5, sp, 12          // Shift row
    s32i    a7, sp, 16          // Max-tap row (fixed-row rewind)
    s32i    a8, sp, 20          // Outputs remaining

.L_ch1b_output:
    l32i    a2, sp, 4
    l32i    a3, sp, 8
    l32i    a5, sp, 12
    l32i    a2, a2, 0           // a2 = delay_line for this output

    // ---- Kernel body (mirrors xtensa_dotprod_s16_q15_shift_simd_ch1) ----
    movi.n  a12, 0              // Single channel accumulator

    mov.n   a14, a2             // a14 = delay_line base
    extui   a15, a4, 16, 4      // a15 = max tap count (0-6)
    extui   a4, a4, 0, 16       // a4 = filter length

    srli    a8, a4, 5           // a8 = length / 32

    ee.ld.128.usar.ip  q0, a2, 16       // Setup SAR, load raw into q0
    ee.vld.128.ip      q1, a2, 16       // Load raw into q1
    ee.src.q.ld.ip     q2, a2, 16, q0, q1   // q0 = aligned, load raw into q2
    ee.src.q.ld.ip     q3, a2, 16, q1, q2   // q1 = aligned, load raw into q3

    l8ui            a9, a5, 0       // Load shift for batch 0
    ee.vld.128.ip   q4, a3, 16      // Preload first coefficients
    ee.zero.accx                    // Clear vector accumulator

    extui   a4, a4, 3, 2            // a4 = 0-3 remaining batches

    beqz    a8, .L_ch1b_remainder

    loopnez a8, .L_ch1b_loop_end
.L_ch1b_loop:
    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q4, q2, q3
    l8ui a10, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    ee.vmulas.s16.accx.ld.ip.qup   q1, a2, 16, q1, q5, q3, q0
    l8ui a9, a5, 2
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11

    ee.vmulas.s16.accx.ld.ip.qup   q2, a2, 16, q2, q4, q0, q1
    l8ui a10, a5, 3
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    ee.vmulas.s16.accx.ld.ip.qup   q3, a2, 16, q3, q5, q1, q2
    addi.n a5, a5, 4
    l8ui a9, a5, 0
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11

.L_ch1b_loop_end:

.L_ch1b_remainder:

    beqz    a4, .L_ch1b_maxtap_check
    addi    a4, a4, -1

    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q4, q2, q3
    l8ui a10, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q5, a3, 16
    ee.zero.accx
    add a12, a12, a11

    beqz    a4, .L_ch1b_maxtap_check
    addi    a4, a4, -1

    ee.vmulas.s16.accx   q1, q5
    l8ui a9, a5, 2
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add a12, a12, a11

    beqz    a4, .L_ch1b_maxtap_check

    ee.vmulas.s16.accx   q2, q4
    ee.srs.accx a11, a9, 0
    ee.zero.accx
    add a12, a12, a11

.L_ch1b_maxtap_check:
    beqz    a15, .L_ch1b_store
    mov.n   a8, a12             // a8 = dot product before the max taps
    loopnez a15, .L_ch1b_maxtap_end
.L_ch1b_maxtap:
    l16ui   a9, a7, 4           // index_reversed
    l32i    a10, a7, 0          // Q31 coefficient
    addx2   a9, a9, a14         // &delay_line[index]
    addi.n  a7, a7, 8
    l16si   a9, a9, 0           // sample
    slli    a9, a9, 16          // promote to Q31
    mulsh   a9, a10, a9         // high 32 bits of coeff * sample_q31
    add.n   a12, a12, a9
.L_ch1b_maxtap_end:
    MAXTAP_SATURATE a12, a8, a9, a10, a11

.L_ch1b_store:
    s32i    a12, a6, 0
    // ---- End kernel body ----

    // Advance to the next output
    addi.n  a6, a6, 4
    l32i    a8, sp, 20
    l32i    a4, sp, 0
    addi.n  a8, a8, -1
    s32i    a8, sp, 20
    beqz    a8, .L_ch1b_done

    l32i    a9, sp, 4
    addi.n  a9, a9, 4
    s32i    a9, sp, 4

    bbsi    a4, 23, .L_ch1b_fixed_row

    // Sequential rows: coeffs += 2*length, shifts += length/8; the max-tap
    // loop above already advanced a7 by 8*count (== the entry row stride)
    extui   a9, a4, 0, 16
    l32i    a10, sp, 8
    l32i    a11, sp, 12
    addx2   a10, a9, a10
    srli    a9, a9, 3
    add.n   a11, a11, a9
    s32i    a10, sp, 8
    s32i    a11, sp, 12
    j       .L_ch1b_output

.L_ch1b_fixed_row:
    l32i    a7, sp, 16          // Rewind max taps to the shared row
    j       .L_ch1b_output

.L_ch1b_done:
    retw.n

    .size   xtensa_dotprod_s16_q15_shift_simd_ch1_batch, . - xtensa_dotprod_s16_q15_shift_simd_ch1_batch

    // Function: xtensa_dotprod_s16_q15_shift_simd_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one call.
    // Each output's delay line is loaded and aligned once and multiplied
    // against two coefficient rows (the ASRC blend pair) by alternating the
    // rows' coefficient blocks against the single ACCX; each row's block
    // shifts and max-tap records apply to its own accumulator.
    // Parameters:
    //   a2: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a3: coeffs_a (q15_t*) - row A coefficients, must be 16-byte aligned
    //   a4: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4)
    //   a5: shifts_a (uint8_t*) - row A shifts
    //   a6: output (int32_t*) - [row A, row B] accumulator pair per output
    //   a7: extra (const Q15DualRowExtra*) - {coeffs_b, shifts_b, entries_a,
    //       entries_b}; the entries pointers are ignored when the packed tap
    //       count is 0
    //
    // The row pair is FIXED across the batch (the near-unity ASRC case: the
    // blend pair is stable for many consecutive outputs); only the sample
    // pointer differs per output.

    .align  4
    .global xtensa_dotprod_s16_q15_shift_simd_ch1_dual
    .type   xtensa_dotprod_s16_q15_shift_simd_ch1_dual, @function
xtensa_dotprod_s16_q15_shift_simd_ch1_dual:
    entry   sp, 48

    // Outer-loop state, spilled to locals (sp+0..31; sp+32..47 is the base
    // save area): the kernel body clobbers every register except a7
    extui   a8, a4, 20, 3
    s32i    a4, sp, 0           // Packed length/taps/count word
    s32i    a2, sp, 4           // sample_ptrs cursor
    s32i    a3, sp, 8           // Row A coefficient base
    s32i    a5, sp, 12          // Row A shift base
    s32i    a6, sp, 16          // Output cursor
    s32i    a8, sp, 20          // Outputs remaining

.L_ch1d_output:
    l32i    a2, sp, 4
    l32i    a3, sp, 8
    l32i    a5, sp, 12
    l32i    a2, a2, 0           // a2 = delay_line for this output
    l32i    a4, a7, 0           // a4 = row B coefficient base
    l32i    a6, a7, 4           // a6 = row B shift base
    s32i    a2, sp, 24          // Delay base (max-tap indexing)

    movi.n  a12, 0              // Row A accumulator
    movi.n  a14, 0              // Row B accumulator

    l32i    a8, sp, 0
    extui   a8, a8, 0, 16       // a8 = filter length
    extui   a9, a8, 3, 2
    s32i    a9, sp, 28          // Remainder batches (0-3)
    srli    a8, a8, 5           // a8 = length / 32

    // Preload samples from potentially misaligned delay_line
    ee.ld.128.usar.ip  q0, a2, 16       // Setup SAR, load raw into q0
    ee.vld.128.ip      q1, a2, 16       // Load raw into q1
    ee.src.q.ld.ip     q2, a2, 16, q0, q1   // q0 = aligned, load raw into q2
    ee.src.q.ld.ip     q3, a2, 16, q1, q2   // q1 = aligned, load raw into q3

    l8ui            a9, a5, 0       // Row A shift for batch 0
    l8ui            a10, a6, 0      // Row B shift for batch 0
    ee.vld.128.ip   q4, a3, 16      // Row A coefficients for batch 0
    ee.vld.128.ip   q5, a4, 16      // Row B coefficients for batch 0
    ee.zero.accx

    beqz    a8, .L_ch1d_remainder

    // Main loop (32 samples per iteration = 4 batches of 8, each against
    // both rows; coefficient vectors double-buffer q4/q5 <-> q6/q7 and the
    // shift registers alternate a9/a13 (row A) and a10/a15 (row B))
    loopnez a8, .L_ch1d_loop_end
.L_ch1d_loop:
    // Batch 0, row A: q0 x q4
    ee.vmulas.s16.accx  q0, q4
    l8ui    a13, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q6, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    // Batch 0, row B: q0 x q5, queue(q2,q3) -> q2 aligned, q0 raw
    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q5, q2, q3
    l8ui    a15, a6, 1
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q7, a4, 16
    ee.zero.accx
    add     a14, a14, a11

    // Batch 1: q1 x (q6, q7), queue(q3,q0) -> q3 aligned, q1 raw
    ee.vmulas.s16.accx  q1, q6
    l8ui    a9, a5, 2
    ee.srs.accx a11, a13, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx.ld.ip.qup   q1, a2, 16, q1, q7, q3, q0
    l8ui    a10, a6, 2
    ee.srs.accx a11, a15, 0
    ee.vld.128.ip   q5, a4, 16
    ee.zero.accx
    add     a14, a14, a11
    // State: q2,q3 = aligned; q0,q1 = raw

    // Batch 2: q2 x (q4, q5), queue(q0,q1) -> q0 aligned, q2 raw
    ee.vmulas.s16.accx  q2, q4
    l8ui    a13, a5, 3
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q6, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx.ld.ip.qup   q2, a2, 16, q2, q5, q0, q1
    l8ui    a15, a6, 3
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q7, a4, 16
    ee.zero.accx
    add     a14, a14, a11

    // Batch 3: q3 x (q6, q7), queue(q1,q2) -> q1 aligned, q3 raw
    ee.vmulas.s16.accx  q3, q6
    addi.n  a5, a5, 4
    l8ui    a9, a5, 0
    ee.srs.accx a11, a13, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx.ld.ip.qup   q3, a2, 16, q3, q7, q1, q2
    addi.n  a6, a6, 4
    l8ui    a10, a6, 0
    ee.srs.accx a11, a15, 0
    ee.vld.128.ip   q5, a4, 16
    ee.zero.accx
    add     a14, a14, a11
    // State: q0,q1 = aligned; q2,q3 = raw (back to initial state)
.L_ch1d_loop_end:

.L_ch1d_remainder:
    l32i    a8, sp, 28
    beqz    a8, .L_ch1d_maxtap
    addi    a8, a8, -1

    // Remainder batch 0: q0 x (q4, q5)
    ee.vmulas.s16.accx  q0, q4
    l8ui    a13, a5, 1
    ee.srs.accx a11, a9, 0
    ee.vld.128.ip   q6, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx.ld.ip.qup   q0, a2, 16, q0, q5, q2, q3
    l8ui    a15, a6, 1
    ee.srs.accx a11, a10, 0
    ee.vld.128.ip   q7, a4, 16
    ee.zero.accx
    add     a14, a14, a11

    beqz    a8, .L_ch1d_maxtap
    addi    a8, a8, -1

    // Remainder batch 1: q1 x (q6, q7)
    ee.vmulas.s16.accx  q1, q6
    l8ui    a9, a5, 2
    ee.srs.accx a11, a13, 0
    ee.vld.128.ip   q4, a3, 16
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx  q1, q7
    l8ui    a10, a6, 2
    ee.srs.accx a11, a15, 0
    ee.vld.128.ip   q5, a4, 16
    ee.zero.accx
    add     a14, a14, a11

    beqz    a8, .L_ch1d_maxtap

    // Remainder batch 2: q2 x (q4, q5)
    ee.vmulas.s16.accx  q2, q4
    ee.srs.accx a11, a9, 0
    ee.zero.accx
    add     a12, a12, a11
    ee.vmulas.s16.accx  q2, q5
    ee.srs.accx a11, a10, 0
    ee.zero.accx
    add     a14, a14, a11

.L_ch1d_maxtap:
    // Fold in each row's pre-extracted max taps (see the single-output
    // kernel); row A accumulates into a12, row B into a14, each saturated once
    l32i    a4, sp, 0
    extui   a15, a4, 16, 4      // a15 = max tap count (0-6)
    beqz    a15, .L_ch1d_store
    l32i    a2, sp, 24          // Delay base
    l32i    a3, a7, 8           // Row A entries
    mov.n   a5, a12             // a5 = row A dot product before the max taps
    loopnez a15, .L_ch1d_maxtap_a_end
.L_ch1d_maxtap_a:
    l16ui   a9, a3, 4           // index_reversed
    l32i    a10, a3, 0          // Q31 coefficient
    addx2   a9, a9, a2          // &delay_line[index]
    addi.n  a3, a3, 8
    l16si   a9, a9, 0           // sample
    slli    a9, a9, 16          // promote to Q31
    mulsh   a9, a10, a9         // high 32 bits of coeff * sample_q31
    add.n   a12, a12, a9
.L_ch1d_maxtap_a_end:
    MAXTAP_SATURATE a12, a5, a9, a10, a11
    l32i    a3, a7, 12          // Row B entries
    mov.n   a5, a14             // a5 = row B dot product before the max taps
    loopnez a15, .L_ch1d_maxtap_b_end
.L_ch1d_maxtap_b:
    l16ui   a9, a3, 4
    l32i    a10, a3, 0
    addx2   a9, a9, a2
    addi.n  a3, a3, 8
    l16si   a9, a9, 0
    slli    a9, a9, 16
    mulsh   a9, a10, a9
    add.n   a14, a14, a9
.L_ch1d_maxtap_b_end:
    MAXTAP_SATURATE a14, a5, a9, a10, a11

.L_ch1d_store:
    l32i    a8, sp, 16
    l32i    a9, sp, 20
    s32i    a12, a8, 0
    s32i    a14, a8, 4
    addi.n  a8, a8, 8
    addi.n  a9, a9, -1
    s32i    a8, sp, 16
    s32i    a9, sp, 20
    beqz    a9, .L_ch1d_done

    l32i    a10, sp, 4
    addi.n  a10, a10, 4
    s32i    a10, sp, 4
    j       .L_ch1d_output

.L_ch1d_done:
    retw.n

    .size   xtensa_dotprod_s16_q15_shift_simd_ch1_dual, . - xtensa_dotprod_s16_q15_shift_simd_ch1_dual

#endif
