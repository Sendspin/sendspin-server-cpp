// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 PIE implementation for 1-channel audio FIR filter
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

    // Function: riscv_dotprod_s16_q15_shift_pie_ch1
    // Computes dot product of int16 samples and Q15 coefficients using PIE
    // SIMD (1 channel): each 8-tap block is accumulated in the wide XACC,
    // shifted right once by the block's shift, and added to a 32-bit
    // accumulator; then folds in the pre-extracted max-tap MACs while the
    // accumulator is still live
    // Parameters:
    //   a0: delay_line (int16_t*) - any alignment supported
    //   a1: coeffs (q15_t*) - must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19)
    //   a3: shifts (uint8_t*) - shifts for each block of 8 samples
    //   a4: output (int32_t*) - pointer to single int32 output
    //   a5: max_tap_entries - 8-byte {int32 coeff, uint16 index_reversed, pad}
    //       records, ignored when the packed tap count is 0

    .global riscv_dotprod_s16_q15_shift_pie_ch1
    .type   riscv_dotprod_s16_q15_shift_pie_ch1, @function
riscv_dotprod_s16_q15_shift_pie_ch1:
    // Enable unaligned 128-bit vector access (the delay line is only
    // 2-byte aligned); the bit is left set, matching esp-dsp's kernels
    esp.movx.r.cfg t3
    ori     t3, t3, 2
    esp.movx.w.cfg t3

    // Unpack args; keep the sample base for max-tap indexing (a0 advances)
    mv      a6, a0              // a6 = delay_line base
    srli    t0, a2, 16
    andi    t0, t0, 15          // t0 = max tap count (0-6)
    slli    t1, a2, 16
    srli    t1, t1, 19          // t1 = length / 8 = block count
    addi    t3, t1, -1          // t3 = hw-loop blocks (last block peeled)

    // Preload block 0's samples, coefficients, and shift
    esp.vld.128.ip q0, a0, 16
    esp.vld.128.ip q1, a1, 16
    lbu     t4, 0(a3)
    addi    a3, a3, 1
    li      a2, 0               // a2 = accumulator (packed args dead)

    beqz    t3, .Lch1_last

    // Main loop: one 8-tap block per iteration, software-pipelined: the next
    // block's coefficients and shift load between the MAC and the shift-out
    // to cover the XACC latency, and the previous block's sum is added while
    // the current one drains. The fused MAC also loads the next block's
    // samples, so the last block is peeled below to avoid reading past the
    // delay line. Each block's arithmetic and the summation order are unchanged
    // by the pipelining
    li      t5, 0               // No previous block sum yet
    esp.lp.setup 0, t3, .Lch1_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q0, a0, 16, q0, q1  // 8 MACs; load next samples
    esp.vld.128.ip q1, a1, 16   // Next coefficient block
    lbu     t6, 0(a3)           // Next block shift
    addi    a3, a3, 1
    add     a2, a2, t5          // Previous block sum
    esp.srs.s.xacc t5, t4       // Block sum >> shift
.Lch1_loop_end:
    mv      t4, t6
    add     a2, a2, t5

.Lch1_last:
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q1
    esp.srs.s.xacc t5, t4
    add     a2, a2, t5

    // Fold in the pre-extracted max taps: acc += (coeff * (sample << 16)) >> 32
    // per entry, then saturate once, matching apply_max_taps() exactly (sample
    // low bits are zero, so the single mulh is bit-exact)
    beqz    t0, .Lch1_store
    mv      a7, a2              // a7 = dot product before the max taps
.Lch1_maxtap:
    lhu     t1, 4(a5)           // index_reversed
    lw      t2, 0(a5)           // Q31 coefficient
    addi    a5, a5, 8
    slli    t1, t1, 1
    add     t1, t1, a6          // &delay_line[index]
    lh      t1, 0(t1)           // sample
    slli    t1, t1, 16          // promote to Q31
    mulh    t1, t2, t1          // high 32 bits of coeff * sample_q31
    add     a2, a2, t1
    addi    t0, t0, -1
    bnez    t0, .Lch1_maxtap
    MAXTAP_SATURATE a2, a7, t1, t2, t3

.Lch1_store:
    sw      a2, 0(a4)
    mv      a0, a2
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch1, . - riscv_dotprod_s16_q15_shift_pie_ch1

    // Function: riscv_dotprod_s16_q15_shift_pie_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs in one
    // call: each output's delay line is loaded once per block and multiplied
    // against two coefficient rows (the ASRC blend pair), with each row's
    // block shifts and max-tap records applied to its own accumulator.
    // The row pair is FIXED across the batch (the near-unity ASRC case);
    // only the sample pointer differs per output.
    // Parameters:
    //   a0: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a1: coeffs_a (q15_t*) - row A coefficients, must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4)
    //   a3: shifts_a (uint8_t*) - row A shifts
    //   a4: output (int32_t*) - [row A, row B] accumulator pair per output
    //   a5: extra (const Q15DualRowExtra*) - {coeffs_b, shifts_b, entries_a,
    //       entries_b}; the entries pointers are ignored when the packed tap
    //       count is 0
    // Frame: sp+0 sample_ptrs cursor, sp+4 output cursor, sp+8 outputs
    // remaining; sp+16.. saved s0/s1/s8-s11

    .align  4
    .global riscv_dotprod_s16_q15_shift_pie_ch1_dual
    .type   riscv_dotprod_s16_q15_shift_pie_ch1_dual, @function
riscv_dotprod_s16_q15_shift_pie_ch1_dual:
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

    // Unpack fixed state
    lw      s0, 0(a5)           // s0 = coeffs_b base
    lw      s1, 4(a5)           // s1 = shifts_b base
    lw      s10, 8(a5)          // s10 = row A max-tap entries
    lw      s11, 12(a5)         // s11 = row B max-tap entries
    srli    t1, a2, 16
    andi    t1, t1, 15          // t1 = max tap count (0-6)
    srli    t2, a2, 20
    andi    t2, t2, 7           // t2 = output count
    slli    t0, a2, 16
    srli    t0, t0, 19          // t0 = length / 8 = block count
    sw      a0, 0(sp)           // sample_ptrs cursor
    sw      a4, 4(sp)           // Output cursor
    sw      t2, 8(sp)           // Outputs remaining

.Ld1_output:
    lw      a6, 0(sp)
    lw      t4, 0(a6)           // t4 = delay line for this output
    mv      a5, t4              // a5 = delay base (max-tap indexing)
    mv      t5, a1              // t5 = row A coefficient cursor
    mv      t6, s0              // t6 = row B coefficient cursor
    mv      a2, a3              // a2 = row A shift cursor
    mv      a4, s1              // a4 = row B shift cursor
    li      a6, 0               // a6 = row A accumulator
    li      a7, 0               // a7 = row B accumulator

    // Preload samples, both rows' first coefficient blocks, and their shifts
    esp.vld.128.ip q0, t4, 16
    esp.vld.128.ip q1, t5, 16
    esp.vld.128.ip q2, t6, 16
    lbu     t3, 0(a2)           // t3 = row A block shift
    addi    a2, a2, 1
    lbu     s8, 0(a4)           // s8 = row B block shift
    addi    a4, a4, 1
    li      s9, 0               // No pending row B block sum yet

    addi    a0, t0, -1          // Hw-loop blocks (last block peeled: the
                                // fused coefficient loads must not read
                                // past either row)
    beqz    a0, .Ld1_last

    // Software-pipelined like the single-output kernel: each row's
    // shift-out waits behind the previous sum's add and the next block's
    // loads, covering the XACC latency. Same per-block arithmetic and order
    esp.lp.setup 0, a0, .Ld1_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q1, t5, 16, q0, q1  // Row A MACs; next A coeffs
    add     a7, a7, s9          // Previous block's row B sum
    lbu     a0, 0(a2)           // Next row A shift
    addi    a2, a2, 1
    esp.srs.s.xacc s9, t3
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q2, t6, 16, q0, q2  // Row B MACs; next B coeffs
    esp.vld.128.ip q0, t4, 16                     // Next sample block
    add     a6, a6, s9
    lbu     t2, 0(a4)           // Next row B shift
    addi    a4, a4, 1
    esp.srs.s.xacc s9, s8
    mv      t3, a0
.Ld1_loop_end:
    mv      s8, t2

.Ld1_last:
    add     a7, a7, s9          // Pending row B sum (0 if the loop was skipped)
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q1
    esp.srs.s.xacc s9, t3
    add     a6, a6, s9
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q2
    esp.srs.s.xacc s9, s8
    add     a7, a7, s9

    // Fold in each row's pre-extracted max taps (see the single-output
    // kernel); row A accumulates into a6, row B into a7, each saturated once
    beqz    t1, .Ld1_store
    mv      t3, s10             // Row A entries cursor
    mv      a2, t1
    mv      t4, a6              // t4 = row A dot product before the max taps
.Ld1_maxtap_a:
    lhu     s8, 4(t3)           // index_reversed
    lw      s9, 0(t3)           // Q31 coefficient
    addi    t3, t3, 8
    slli    s8, s8, 1
    add     s8, s8, a5
    lh      s8, 0(s8)           // sample
    slli    s8, s8, 16          // promote to Q31
    mulh    s8, s9, s8
    add     a6, a6, s8
    addi    a2, a2, -1
    bnez    a2, .Ld1_maxtap_a
    MAXTAP_SATURATE a6, t4, t5, t6, a4
    mv      t3, s11             // Row B entries cursor
    mv      a2, t1
    mv      t4, a7              // t4 = row B dot product before the max taps
.Ld1_maxtap_b:
    lhu     s8, 4(t3)
    lw      s9, 0(t3)
    addi    t3, t3, 8
    slli    s8, s8, 1
    add     s8, s8, a5
    lh      s8, 0(s8)
    slli    s8, s8, 16
    mulh    s8, s9, s8
    add     a7, a7, s8
    addi    a2, a2, -1
    bnez    a2, .Ld1_maxtap_b
    MAXTAP_SATURATE a7, t4, t5, t6, a4

.Ld1_store:
    lw      t3, 4(sp)
    sw      a6, 0(t3)
    sw      a7, 4(t3)
    addi    t3, t3, 8
    sw      t3, 4(sp)
    lw      s8, 8(sp)
    addi    s8, s8, -1
    sw      s8, 8(sp)
    beqz    s8, .Ld1_done
    lw      s9, 0(sp)
    addi    s9, s9, 4
    sw      s9, 0(sp)
    j       .Ld1_output

.Ld1_done:
    lw      s0, 16(sp)
    lw      s1, 20(sp)
    lw      s8, 24(sp)
    lw      s9, 28(sp)
    lw      s10, 32(sp)
    lw      s11, 36(sp)
    addi    sp, sp, 48
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch1_dual, . - riscv_dotprod_s16_q15_shift_pie_ch1_dual

    // Function: riscv_dotprod_s16_q15_shift_pie_ch1_batch
    // Batched single-output kernel for the aligned fixed-ratio path: runs the
    // single-output body above for up to 4 outputs in one call, amortizing
    // the call, configuration, and dispatch overhead. Each output has its
    // own delay-line pointer; the coefficient, shift, and max-tap rows
    // advance one bank row per output (the cursors end each output at the
    // next row's start), or rewind to the same row when fixed_row is set
    // (single-phase banks). Per-output arithmetic is identical to the
    // single-output kernel.
    // Parameters:
    //   a0: sample_ptrs (const int16_t* const*) - one delay-line pointer per output
    //   a1: coeffs (q15_t*) - first row, must be 16-byte aligned
    //   a2: length (bits 0-15, must be multiple of 8) | max tap count (bits 16-19) |
    //       output count (bits 20-22, 1-4) | fixed_row (bit 23)
    //   a3: shifts (uint8_t*) - first row's block shifts
    //   a4: output (int32_t*) - one accumulator per output
    //   a5: max_tap_entries - first row's 8-byte records (row stride = tap
    //       count), ignored when the packed tap count is 0
    // Frame: sp+0 saved s8, sp+4 saved s9

    .align  4
    .global riscv_dotprod_s16_q15_shift_pie_ch1_batch
    .type   riscv_dotprod_s16_q15_shift_pie_ch1_batch, @function
riscv_dotprod_s16_q15_shift_pie_ch1_batch:
    addi    sp, sp, -16
    sw      s8, 0(sp)
    sw      s9, 4(sp)

    // Enable unaligned 128-bit vector access
    esp.movx.r.cfg t3
    ori     t3, t3, 2
    esp.movx.w.cfg t3

    srli    t0, a2, 16
    andi    t0, t0, 15          // t0 = max tap count (0-6)
    slli    t1, a2, 16
    srli    t1, t1, 19          // t1 = length / 8 = block count
    srli    a7, a2, 20
    andi    a7, a7, 7           // a7 = outputs remaining
    srli    t2, a2, 23
    andi    t2, t2, 1           // t2 = fixed_row

.Lb1_output:
    lw      t3, 0(a0)           // t3 = delay line for this output
    addi    a0, a0, 4
    mv      a6, t3              // a6 = delay base (max-tap indexing)

    // Block loop: the single-output kernel's, software-pipelined the same way
    esp.vld.128.ip q0, t3, 16
    esp.vld.128.ip q1, a1, 16
    lbu     t4, 0(a3)
    addi    a3, a3, 1
    li      a2, 0               // a2 = accumulator
    li      t5, 0               // No previous block sum yet
    addi    s8, t1, -1          // Hw-loop blocks (last block peeled)
    beqz    s8, .Lb1_last

    esp.lp.setup 0, s8, .Lb1_loop_end
    esp.zero.xacc
    esp.vmulas.s16.xacc.ld.ip q0, t3, 16, q0, q1  // 8 MACs; load next samples
    esp.vld.128.ip q1, a1, 16   // Next coefficient block
    lbu     t6, 0(a3)           // Next block shift
    addi    a3, a3, 1
    add     a2, a2, t5          // Previous block sum
    esp.srs.s.xacc t5, t4       // Block sum >> shift
.Lb1_loop_end:
    mv      t4, t6
    add     a2, a2, t5

.Lb1_last:
    esp.zero.xacc
    esp.vmulas.s16.xacc q0, q1
    esp.srs.s.xacc t5, t4
    add     a2, a2, t5

    // Max taps, exactly as the single-output kernel (the entries cursor
    // advances one row)
    beqz    t0, .Lb1_store
    mv      t6, t0              // t6 = tap counter
    mv      t5, a2              // t5 = dot product before the max taps
.Lb1_maxtap:
    lhu     t3, 4(a5)           // index_reversed
    lw      t4, 0(a5)           // Q31 coefficient
    addi    a5, a5, 8
    slli    t3, t3, 1
    add     t3, t3, a6          // &delay_line[index]
    lh      t3, 0(t3)           // sample
    slli    t3, t3, 16          // promote to Q31
    mulh    t3, t4, t3
    add     a2, a2, t3
    addi    t6, t6, -1
    bnez    t6, .Lb1_maxtap
    MAXTAP_SATURATE a2, t5, t3, t4, s9

.Lb1_store:
    sw      a2, 0(a4)
    addi    a4, a4, 4
    beqz    t2, .Lb1_next
    // fixed_row: rewind the row cursors to the start of the same row
    slli    t3, t1, 4
    sub     a1, a1, t3          // length * 2 bytes
    sub     a3, a3, t1          // one shift per block
    slli    t3, t0, 3
    sub     a5, a5, t3          // 8 bytes per max-tap record
.Lb1_next:
    addi    a7, a7, -1
    bnez    a7, .Lb1_output

    lw      s8, 0(sp)
    lw      s9, 4(sp)
    addi    sp, sp, 16
    ret

    .size   riscv_dotprod_s16_q15_shift_pie_ch1_batch, . - riscv_dotprod_s16_q15_shift_pie_ch1_batch

#endif
