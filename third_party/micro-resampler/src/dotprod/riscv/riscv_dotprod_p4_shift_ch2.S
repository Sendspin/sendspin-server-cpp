// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 PIE implementation for 2-channel interleaved audio FIR filter
// Supports any length that is a multiple of 8
//
// Register notes: PIE instructions encode GPR operands in a 4-bit field that
// only reaches s0/s1, a0-a5, and s8-s11/t3-t6; t0-t2 and a6/a7 are reserved
// for scalar-only work throughout.

#if defined(__riscv) && \
    (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31) || \
     defined(ESP32S31))

    .text
    .align 4

    // Saturate a folded max-tap sum once, matching apply_max_taps(): \acc holds the wrapped
    // \base + tap_sum, and the tap sum itself cannot wrap (FilterDesign caps the tap count; see
    // dotprod_maxtap.h), so the add overflowed iff \base and the tap sum share a sign that
    // \acc does not. Branchless mask select (no conditional move in RV32IM). Clobbers \t0-\t2
    .macro MAXTAP_SATURATE acc, base, t0, t1, t2
    sub     \t0, \acc, \base       // tap sum
    xor     \t1, \base, \acc
    xor     \t0, \t0, \acc
    and     \t1, \t1, \t0
    srai    \t1, \t1, 31            // all ones iff the add overflowed
    srai    \t0, \base, 31          // 0 or -1 by the sign of base
    li      \t2, 0x7fffffff
    xor     \t2, \t2, \t0          // INT32_MAX or INT32_MIN
    xor     \t2, \t2, \acc
    and     \t2, \t2, \t1
    xor     \acc, \acc, \t2        // saturated value on overflow, else unchanged
    .endm

    // Function: riscv_dotprod_s16_q15_shift_pie_ch2
    // Computes dot products of interleaved stereo int16 samples and Q15
    // coefficients using PIE SIMD: each 8-tap block loads 8 LR frames (two
    // 128-bit vectors), deinterleaves them with vunzip, and runs one wide
    // XACC accumulation per channel, shifting each block sum right once by
    // the block's shift; then folds in the pre-extracted max-tap MACs
    // Parameters:
    //   a0: delay_line (int16_t*) - interleaved LR frames, any alignment
    //   a1: coeffs (q15_t*) - must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19)
    //   a3: shifts (uint8_t*) - shifts for each block of 8 coefficients
    //   a4: output (int32_t*) - [L, R] int32 pair
    //   a5: max_tap_entries - 8-byte {int32 coeff, uint16 index_reversed, pad}
    //       records, ignored when the packed tap count is 0

    .global riscv_dotprod_s16_q15_shift_pie_ch2
    .type   riscv_dotprod_s16_q15_shift_pie_ch2, @function
riscv_dotprod_s16_q15_shift_pie_ch2:
    // Enable unaligned 128-bit vector access (the delay line is only
    // 2-byte aligned); the bit is left set, matching esp-dsp's kernels
    esp.movx.r.cfg t3
    ori     t3, t3, 2
    esp.movx.w.cfg t3

    // Unpack args; keep the sample base for max-tap indexing (a0 advances)
    mv      a6, a0              // a6 = delay_line base
    srli    t0, a2, 16
    andi    t0, t0, 15          // t0 = max tap count (0-6)
    slli    t3, a2, 16
    srli    t3, t3, 19          // t3 = length / 8 = block count
    li      a2, 0               // a2 = L accumulator (packed args dead)
    li      a7, 0               // a7 = R accumulator

    // Preload block 0: 8 LR frames, deinterleaved, its coefficients and shift
    esp.vld.128.ip q0, a0, 16   // Frames 0-3 (LRLR)
    esp.vld.128.ip q1, a0, 16   // Frames 4-7
    esp.vld.128.ip q2, a1, 16   // 8 coefficients
    esp.vunzip.16 q0, q1        // q0 = 8 L samples, q1 = 8 R samples
    lbu     t4, 0(a3)           // Block shift
    addi    a3, a3, 1
    addi    t3, t3, -1          // Loop blocks (last block peeled: no loads past the row)
    beqz    t3, .Lch2_last

    // Main loop: one 8-tap block (8 LR frames) per iteration,
    // software-pipelined: the next block's shift, frames, and coefficients
    // load between each MAC and its shift-out to cover the XACC latency.
    // Each block's arithmetic and the summation order are unchanged by the
    // pipelining
    esp.lp.setup 0, t3, .Lch2_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2  // L
    lbu     t6, 0(a3)           // Next block shift
    addi    a3, a3, 1
    esp.srs.s.xacc t5, t4       // L block sum >> shift
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q2  // R
    esp.vld.128.ip q0, a0, 16   // Next frames 0-3
    esp.vld.128.ip q1, a0, 16   // Next frames 4-7
    add     a2, a2, t5
    esp.srs.s.xacc t5, t4       // R block sum >> shift
    esp.vld.128.ip q2, a1, 16   // Next coefficients
    esp.vunzip.16 q0, q1
    mv      t4, t6
.Lch2_loop_end:
    add     a7, a7, t5

.Lch2_last:
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2
    esp.srs.s.xacc t5, t4
    add     a2, a2, t5
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q2
    esp.srs.s.xacc t5, t4
    add     a7, a7, t5

    // Fold in the pre-extracted max taps for both channels: each entry's
    // Q31 coefficient multiplies the L and R samples of its frame, then each
    // channel saturates once, matching apply_max_taps() exactly
    beqz    t0, .Lch2_store
    mv      a0, a2              // a0/a1 = dot products before the max taps
    mv      a1, a7
.Lch2_maxtap:
    lhu     t1, 4(a5)           // index_reversed (frame index)
    lw      t2, 0(a5)           // Q31 coefficient
    addi    a5, a5, 8
    slli    t1, t1, 2           // 4 bytes per LR frame
    add     t1, t1, a6          // &delay_line[2 * index]
    lh      t6, 0(t1)           // L sample
    lh      t1, 2(t1)           // R sample
    slli    t6, t6, 16          // promote to Q31
    slli    t1, t1, 16
    mulh    t6, t2, t6          // high 32 bits of coeff * sample_q31
    mulh    t1, t2, t1
    add     a2, a2, t6
    add     a7, a7, t1
    addi    t0, t0, -1
    bnez    t0, .Lch2_maxtap
    MAXTAP_SATURATE a2, a0, t3, t4, t5
    MAXTAP_SATURATE a7, a1, t3, t4, t5

.Lch2_store:
    sw      a2, 0(a4)
    sw      a7, 4(a4)
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch2, . - riscv_dotprod_s16_q15_shift_pie_ch2

    // Function: riscv_dotprod_s16_q15_shift_pie_ch2_dual
    // Stereo variant of the ch1 dual kernel: computes both blend rows' dot
    // products for up to 4 outputs, deinterleaving each output's delay line
    // once per block and multiplying the L and R vectors against both
    // coefficient rows. Outputs are written as [row A L, row A R, row B L,
    // row B R] quads per output.
    // Parameters:
    //   a0: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a1: coeffs_a (q15_t*) - row A coefficients, must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4)
    //   a3: shifts_a (uint8_t*) - row A shifts
    //   a4: output (int32_t*) - [A L, A R, B L, B R] quad per output
    //   a5: extra (const Q15DualRowExtra*) - {coeffs_b, shifts_b, entries_a,
    //       entries_b}; the entries pointers are ignored when the packed tap
    //       count is 0
    // Frame: sp+0 sample_ptrs cursor, sp+4 output cursor, sp+8 outputs
    // remaining, sp+12 packed args; sp+16.. saved s0/s1/s8-s11

    .align  4
    .global riscv_dotprod_s16_q15_shift_pie_ch2_dual
    .type   riscv_dotprod_s16_q15_shift_pie_ch2_dual, @function
riscv_dotprod_s16_q15_shift_pie_ch2_dual:
    addi    sp, sp, -48
    sw      s0, 16(sp)
    sw      s1, 20(sp)
    sw      s8, 24(sp)
    sw      s9, 28(sp)
    sw      s10, 32(sp)
    sw      s11, 36(sp)

    // Enable unaligned 128-bit vector access
    esp.movx.r.cfg t3
    ori     t3, t3, 2
    esp.movx.w.cfg t3

    // Unpack fixed state (blocks and taps re-derived per phase from the
    // spilled packed word: all four temporaries carry accumulators here)
    lw      s0, 0(a5)           // s0 = coeffs_b base
    lw      s1, 4(a5)           // s1 = shifts_b base
    lw      s10, 8(a5)          // s10 = row A max-tap entries
    lw      s11, 12(a5)         // s11 = row B max-tap entries
    srli    t2, a2, 20
    andi    t2, t2, 7           // t2 = output count
    sw      a0, 0(sp)           // sample_ptrs cursor
    sw      a4, 4(sp)           // Output cursor
    sw      t2, 8(sp)           // Outputs remaining
    sw      a2, 12(sp)          // Packed args

.Ld2_output:
    lw      a6, 0(sp)
    lw      t4, 0(a6)           // t4 = delay line for this output
    mv      a5, t4              // a5 = delay base (max-tap indexing)
    mv      t5, a1              // t5 = row A coefficient cursor
    mv      t6, s0              // t6 = row B coefficient cursor
    mv      a2, a3              // a2 = row A shift cursor
    mv      a4, s1              // a4 = row B shift cursor
    li      a6, 0               // a6 = row A L accumulator
    li      a7, 0               // a7 = row A R accumulator
    li      t0, 0               // t0 = row B L accumulator
    li      t1, 0               // t1 = row B R accumulator

    // Preload block 0: both rows' coefficients, 8 LR frames deinterleaved,
    // and both rows' shifts
    esp.vld.128.ip q2, t5, 16
    esp.vld.128.ip q3, t6, 16
    esp.vld.128.ip q0, t4, 16   // Frames 0-3 (LRLR)
    esp.vld.128.ip q1, t4, 16   // Frames 4-7
    esp.vunzip.16 q0, q1        // q0 = 8 L samples, q1 = 8 R samples
    lbu     t3, 0(a2)           // t3 = row A block shift
    addi    a2, a2, 1
    lbu     s8, 0(a4)           // s8 = row B block shift
    addi    a4, a4, 1
    li      s9, 0               // No pending row B R block sum yet

    lw      a0, 12(sp)
    slli    a0, a0, 16
    srli    a0, a0, 19          // a0 = block count
    addi    a0, a0, -1          // Hw-loop blocks (last block peeled: the
                                // loop loads the next block's data, which
                                // must not read past the row)
    beqz    a0, .Ld2_last

    // Software-pipelined like the single-output kernel: each shift-out
    // waits behind the previous sum's add and the next block's loads,
    // covering the XACC latency. Same per-block arithmetic and order
    esp.lp.setup 0, a0, .Ld2_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2  // Row A x L
    add     t1, t1, s9          // Previous block's row B R sum
    lbu     a0, 0(a2)           // Next row A shift
    addi    a2, a2, 1
    esp.srs.s.xacc s9, t3
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q2, t5, 16, q1, q2  // Row A x R; next A coeffs
    add     a6, a6, s9
    lbu     t2, 0(a4)           // Next row B shift
    addi    a4, a4, 1
    esp.srs.s.xacc s9, t3
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q3  // Row B x L
    add     a7, a7, s9
    esp.vld.128.ip q0, t4, 16   // Next frames 0-3
    esp.srs.s.xacc s9, s8
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q3, t6, 16, q1, q3  // Row B x R; next B coeffs
    add     t0, t0, s9
    esp.vld.128.ip q1, t4, 16   // Next frames 4-7
    esp.srs.s.xacc s9, s8
    esp.vunzip.16 q0, q1
    mv      t3, a0
.Ld2_loop_end:
    mv      s8, t2

.Ld2_last:
    add     t1, t1, s9          // Pending row B R sum (0 if the loop was skipped)
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2
    esp.srs.s.xacc s9, t3
    add     a6, a6, s9
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q2
    esp.srs.s.xacc s9, t3
    add     a7, a7, s9
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q3
    esp.srs.s.xacc s9, s8
    add     t0, t0, s9
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q3
    esp.srs.s.xacc s9, s8
    add     t1, t1, s9

    // Fold in each row's pre-extracted max taps for both channels, each
    // saturated once
    lw      t3, 12(sp)
    srli    t3, t3, 16
    andi    t3, t3, 15          // Max tap count (0-6)
    beqz    t3, .Ld2_store
    mv      a2, t3
    mv      t3, s10             // Row A entries cursor
    mv      t4, a6              // t4/t5 = row A dot products before the max taps
    mv      t5, a7
.Ld2_maxtap_a:
    lhu     s8, 4(t3)           // index_reversed (frame index)
    lw      s9, 0(t3)           // Q31 coefficient
    addi    t3, t3, 8
    slli    s8, s8, 2           // 4 bytes per LR frame
    add     s8, s8, a5
    lh      a4, 0(s8)           // L sample
    lh      s8, 2(s8)           // R sample
    slli    a4, a4, 16
    slli    s8, s8, 16
    mulh    a4, s9, a4
    mulh    s8, s9, s8
    add     a6, a6, a4
    add     a7, a7, s8
    addi    a2, a2, -1
    bnez    a2, .Ld2_maxtap_a
    MAXTAP_SATURATE a6, t4, t6, s8, s9
    MAXTAP_SATURATE a7, t5, t6, s8, s9
    lw      a2, 12(sp)
    srli    a2, a2, 16
    andi    a2, a2, 15
    mv      t3, s11             // Row B entries cursor
    mv      t4, t0              // t4/t5 = row B dot products before the max taps
    mv      t5, t1
.Ld2_maxtap_b:
    lhu     s8, 4(t3)
    lw      s9, 0(t3)
    addi    t3, t3, 8
    slli    s8, s8, 2
    add     s8, s8, a5
    lh      a4, 0(s8)
    lh      s8, 2(s8)
    slli    a4, a4, 16
    slli    s8, s8, 16
    mulh    a4, s9, a4
    mulh    s8, s9, s8
    add     t0, t0, a4
    add     t1, t1, s8
    addi    a2, a2, -1
    bnez    a2, .Ld2_maxtap_b
    MAXTAP_SATURATE t0, t4, t6, s8, s9
    MAXTAP_SATURATE t1, t5, t6, s8, s9

.Ld2_store:
    lw      t3, 4(sp)
    sw      a6, 0(t3)
    sw      a7, 4(t3)
    sw      t0, 8(t3)
    sw      t1, 12(t3)
    addi    t3, t3, 16
    sw      t3, 4(sp)
    lw      s8, 8(sp)
    addi    s8, s8, -1
    sw      s8, 8(sp)
    beqz    s8, .Ld2_done
    lw      s9, 0(sp)
    addi    s9, s9, 4
    sw      s9, 0(sp)
    j       .Ld2_output

.Ld2_done:
    lw      s0, 16(sp)
    lw      s1, 20(sp)
    lw      s8, 24(sp)
    lw      s9, 28(sp)
    lw      s10, 32(sp)
    lw      s11, 36(sp)
    addi    sp, sp, 48
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch2_dual, . - riscv_dotprod_s16_q15_shift_pie_ch2_dual

    // Function: riscv_dotprod_s16_q15_shift_pie_ch2_batch
    // Stereo variant of the ch1 batch kernel: the single-output stereo body
    // for up to 4 outputs per call, with per-output delay-line pointers and
    // sequential (or, with fixed_row, repeated) bank rows. Outputs are
    // written as [L, R] pairs.
    // Parameters:
    //   a0: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a1: coeffs (q15_t*) - first row, must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4) | fixed_row (bit 23)
    //   a3: shifts (uint8_t*) - first row's block shifts
    //   a4: output (int32_t*) - [L, R] pair per output
    //   a5: max_tap_entries - first row's 8-byte records (row stride = tap
    //       count), ignored when the packed tap count is 0
    // Frame: sp+0.. saved s8-s11

    .align  4
    .global riscv_dotprod_s16_q15_shift_pie_ch2_batch
    .type   riscv_dotprod_s16_q15_shift_pie_ch2_batch, @function
riscv_dotprod_s16_q15_shift_pie_ch2_batch:
    addi    sp, sp, -16
    sw      s8, 0(sp)
    sw      s9, 4(sp)
    sw      s10, 8(sp)
    sw      s11, 12(sp)

    // Enable unaligned 128-bit vector access
    esp.movx.r.cfg t3
    ori     t3, t3, 2
    esp.movx.w.cfg t3

    srli    t0, a2, 16
    andi    t0, t0, 15          // t0 = max tap count (0-6)
    slli    t1, a2, 16
    srli    t1, t1, 19          // t1 = length / 8 = block count
    srli    s10, a2, 20
    andi    s10, s10, 7         // s10 = outputs remaining
    srli    t2, a2, 23
    andi    t2, t2, 1           // t2 = fixed_row

.Lb2_output:
    lw      t3, 0(a0)           // t3 = delay line for this output
    addi    a0, a0, 4
    mv      a6, t3              // a6 = delay base (max-tap indexing)

    // Block loop: the single-output kernel's, software-pipelined the same way
    esp.vld.128.ip q0, t3, 16   // Frames 0-3 (LRLR)
    esp.vld.128.ip q1, t3, 16   // Frames 4-7
    esp.vld.128.ip q2, a1, 16   // 8 coefficients
    esp.vunzip.16 q0, q1        // q0 = 8 L samples, q1 = 8 R samples
    lbu     t4, 0(a3)
    addi    a3, a3, 1
    li      a2, 0               // a2 = L accumulator
    li      a7, 0               // a7 = R accumulator
    addi    s8, t1, -1          // Hw-loop blocks (last block peeled)
    beqz    s8, .Lb2_last

    esp.lp.setup 0, s8, .Lb2_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2  // L
    lbu     t6, 0(a3)           // Next block shift
    addi    a3, a3, 1
    esp.srs.s.xacc t5, t4       // L block sum >> shift
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q2  // R
    esp.vld.128.ip q0, t3, 16   // Next frames 0-3
    esp.vld.128.ip q1, t3, 16   // Next frames 4-7
    add     a2, a2, t5
    esp.srs.s.xacc t5, t4       // R block sum >> shift
    esp.vld.128.ip q2, a1, 16   // Next coefficients
    esp.vunzip.16 q0, q1
    mv      t4, t6
.Lb2_loop_end:
    add     a7, a7, t5

.Lb2_last:
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2
    esp.srs.s.xacc t5, t4
    add     a2, a2, t5
    esp.zero.xacc
    esp.vmulas.s16.xacc q1, q2
    esp.srs.s.xacc t5, t4
    add     a7, a7, t5

    // Max taps for both channels, exactly as the single-output kernel (the
    // entries cursor advances one row)
    beqz    t0, .Lb2_store
    mv      t6, t0              // t6 = tap counter
    mv      t5, a2              // t5/s11 = dot products before the max taps
    mv      s11, a7
.Lb2_maxtap:
    lhu     t3, 4(a5)           // index_reversed (frame index)
    lw      t4, 0(a5)           // Q31 coefficient
    addi    a5, a5, 8
    slli    t3, t3, 2           // 4 bytes per LR frame
    add     t3, t3, a6          // &delay_line[2 * index]
    lh      s8, 0(t3)           // L sample
    lh      s9, 2(t3)           // R sample
    slli    s8, s8, 16          // promote to Q31
    slli    s9, s9, 16
    mulh    s8, t4, s8
    mulh    s9, t4, s9
    add     a2, a2, s8
    add     a7, a7, s9
    addi    t6, t6, -1
    bnez    t6, .Lb2_maxtap
    MAXTAP_SATURATE a2, t5, t3, t4, s8
    MAXTAP_SATURATE a7, s11, t3, t4, s8

.Lb2_store:
    sw      a2, 0(a4)
    sw      a7, 4(a4)
    addi    a4, a4, 8
    beqz    t2, .Lb2_next
    // fixed_row: rewind the row cursors to the start of the same row
    slli    t3, t1, 4
    sub     a1, a1, t3          // length * 2 bytes
    sub     a3, a3, t1          // one shift per block
    slli    t3, t0, 3
    sub     a5, a5, t3          // 8 bytes per max-tap record
.Lb2_next:
    addi    s10, s10, -1
    bnez    s10, .Lb2_output

    lw      s8, 0(sp)
    lw      s9, 4(sp)
    lw      s10, 8(sp)
    lw      s11, 12(sp)
    addi    sp, sp, 16
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch2_batch, . - riscv_dotprod_s16_q15_shift_pie_ch2_batch

#endif
