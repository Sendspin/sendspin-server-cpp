// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 scalar-FPU implementation for 1-channel Float32 FIR filter
// Supports any length that is a multiple of 8
//
// The P4 has no floating-point SIMD, so this is software-pipelined scalar
// fmadd.s code: four independent accumulators break the FPU's fused-MAC
// latency chain (the compiled C++ loop is serial on one accumulator). The
// loops are plain branch loops: live FPU state across an esp.lp.setup hardware
// loop hits the P4's hwloop state-save erratum (see the loop comments).

#if defined(__riscv) && \
    (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31) || \
     defined(ESP32S31))

    .text
    .align 4

    // Function: riscv_dotprod_f32_pie_ch1
    // Computes dot product of float samples and coefficients (1 channel).
    // Tap j accumulates into accumulator j % 4; the four accumulators are
    // reduced as (acc0 + acc1) + (acc2 + acc3). Any dual/batched variant
    // must replicate this exact association to stay bit-identical.
    // Parameters:
    //   a0: samples (float*) - any 4-byte alignment
    //   a1: coeffs (float*) - any 4-byte alignment
    //   a2: length (must be multiple of 8)
    //   a3: output (float*) - pointer to single float output

    .global riscv_dotprod_f32_pie_ch1
    .type   riscv_dotprod_f32_pie_ch1, @function
riscv_dotprod_f32_pie_ch1:
    fmv.w.x ft8, zero           // acc0 (taps 0, 4, 8, ...)
    fmv.w.x ft9, zero           // acc1
    fmv.w.x ft10, zero          // acc2
    fmv.w.x ft11, zero          // acc3
    slli    t4, a2, 2
    add     t4, a0, t4          // t4 = sample end (loop exit address)
    andi    t3, a2, 8           // t3 != 0 when length / 8 is odd
    beqz    t3, .Lf32ch1_loop

    // Odd number of 8-tap blocks: rewind both pointers one block and enter
    // the loop at its second half. Every tap keeps its accumulator (j % 4)
    // and order, with no remainder path and no read outside either row.
    addi    a0, a0, -32
    addi    a1, a1, -32
    j       .Lf32ch1_half

    // 16 taps per iteration: four 4-tap groups, each group's loads issued
    // well before its fmadds so load-use stalls stay hidden; the exit test
    // compares the sample pointer against its end, so no counter is kept.
    // Plain branch loop: holding live FPU accumulators across an esp.lp.setup
    // hardware loop produced run-to-run nondeterminism under FreeRTOS (the P4
    // hardware-loop state-save erratum), so the f32 kernels avoid hwloops.
.Lf32ch1_loop:
    flw     ft0, 0(a0)
    flw     ft1, 0(a1)
    flw     ft2, 4(a0)
    flw     ft3, 4(a1)
    flw     ft4, 8(a0)
    flw     ft5, 8(a1)
    flw     ft6, 12(a0)
    flw     ft7, 12(a1)
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft2, ft3, ft9
    fmadd.s ft10, ft4, ft5, ft10
    fmadd.s ft11, ft6, ft7, ft11
    flw     ft0, 16(a0)
    flw     ft1, 16(a1)
    flw     ft2, 20(a0)
    flw     ft3, 20(a1)
    flw     ft4, 24(a0)
    flw     ft5, 24(a1)
    flw     ft6, 28(a0)
    flw     ft7, 28(a1)
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft2, ft3, ft9
    fmadd.s ft10, ft4, ft5, ft10
    fmadd.s ft11, ft6, ft7, ft11
.Lf32ch1_half:
    flw     ft0, 32(a0)
    flw     ft1, 32(a1)
    flw     ft2, 36(a0)
    flw     ft3, 36(a1)
    flw     ft4, 40(a0)
    flw     ft5, 40(a1)
    flw     ft6, 44(a0)
    flw     ft7, 44(a1)
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft2, ft3, ft9
    fmadd.s ft10, ft4, ft5, ft10
    fmadd.s ft11, ft6, ft7, ft11
    flw     ft0, 48(a0)
    flw     ft1, 48(a1)
    flw     ft2, 52(a0)
    flw     ft3, 52(a1)
    flw     ft4, 56(a0)
    flw     ft5, 56(a1)
    flw     ft6, 60(a0)
    flw     ft7, 60(a1)
    addi    a0, a0, 64
    addi    a1, a1, 64
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft2, ft3, ft9
    fmadd.s ft10, ft4, ft5, ft10
    fmadd.s ft11, ft6, ft7, ft11
    bne     a0, t4, .Lf32ch1_loop

    // Reduce (acc0 + acc1) + (acc2 + acc3)
    fadd.s  ft8, ft8, ft9
    fadd.s  ft10, ft10, ft11
    fadd.s  ft8, ft8, ft10
    fsw     ft8, 0(a3)
    ret

    .size   riscv_dotprod_f32_pie_ch1, . - riscv_dotprod_f32_pie_ch1

    // Function: riscv_dotprod_f32_pie_ch1_dual
    // Computes BOTH blend rows' dot products for up to 4 outputs, loading
    // each sample once and feeding two fmadd chains. Row accumulation is
    // tap j into accumulator j % 4 reduced (a0 + a1) + (a2 + a3) - the same
    // association as the single-output kernel, so results are bit-identical
    // to it despite float addition being non-associative. Outputs are
    // written as [row A, row B] pairs per output.
    // Parameters:
    //   a0: sample_ptrs (const float* const*) - one delay-line pointer per output
    //   a1: coeffs_a (float*) - row A coefficients
    //   a2: coeffs_b (float*) - row B coefficients
    //   a3: length (bits 0-15, must be multiple of 8) | output count (bits 20-22)
    //   a4: output (float*) - [row A, row B] pair per output

    .align  4
    .global riscv_dotprod_f32_pie_ch1_dual
    .type   riscv_dotprod_f32_pie_ch1_dual, @function
riscv_dotprod_f32_pie_ch1_dual:
    srli    a6, a3, 20
    andi    a6, a6, 7           // a6 = output count
    slli    a7, a3, 16
    srli    a7, a7, 14          // a7 = length * 4 (row bytes)

.Lf32d1_output:
    lw      a5, 0(a0)           // a5 = samples for this output
    mv      t5, a1              // t5 = row A coefficient cursor
    mv      t6, a2              // t6 = row B coefficient cursor
    fmv.w.x fa0, zero           // Row A acc0-acc3
    fmv.w.x fa1, zero
    fmv.w.x fa2, zero
    fmv.w.x fa3, zero
    fmv.w.x fa4, zero           // Row B acc0-acc3
    fmv.w.x fa5, zero
    fmv.w.x fa6, zero
    fmv.w.x fa7, zero
    add     t3, a5, a7          // t3 = sample end (loop exit address)

    // 8 taps per iteration (two 4-tap groups): each sample feeds both rows'
    // fmadds (plain branch loop; see the single-output kernel's hwloop note)
.Lf32d1_loop:
    flw     ft0, 0(a5)
    flw     ft1, 0(t5)
    flw     ft2, 0(t6)
    flw     ft3, 4(a5)
    flw     ft4, 4(t5)
    flw     ft5, 4(t6)
    flw     ft6, 8(a5)
    flw     ft7, 8(t5)
    flw     ft8, 8(t6)
    flw     ft9, 12(a5)
    flw     ft10, 12(t5)
    flw     ft11, 12(t6)
    fmadd.s fa0, ft0, ft1, fa0
    fmadd.s fa4, ft0, ft2, fa4
    fmadd.s fa1, ft3, ft4, fa1
    fmadd.s fa5, ft3, ft5, fa5
    fmadd.s fa2, ft6, ft7, fa2
    fmadd.s fa6, ft6, ft8, fa6
    fmadd.s fa3, ft9, ft10, fa3
    fmadd.s fa7, ft9, ft11, fa7
    flw     ft0, 16(a5)
    flw     ft1, 16(t5)
    flw     ft2, 16(t6)
    flw     ft3, 20(a5)
    flw     ft4, 20(t5)
    flw     ft5, 20(t6)
    flw     ft6, 24(a5)
    flw     ft7, 24(t5)
    flw     ft8, 24(t6)
    flw     ft9, 28(a5)
    flw     ft10, 28(t5)
    flw     ft11, 28(t6)
    addi    a5, a5, 32
    addi    t5, t5, 32
    addi    t6, t6, 32
    fmadd.s fa0, ft0, ft1, fa0
    fmadd.s fa4, ft0, ft2, fa4
    fmadd.s fa1, ft3, ft4, fa1
    fmadd.s fa5, ft3, ft5, fa5
    fmadd.s fa2, ft6, ft7, fa2
    fmadd.s fa6, ft6, ft8, fa6
    fmadd.s fa3, ft9, ft10, fa3
    fmadd.s fa7, ft9, ft11, fa7
    bne     a5, t3, .Lf32d1_loop

    // Reduce each row: (acc0 + acc1) + (acc2 + acc3)
    fadd.s  fa0, fa0, fa1
    fadd.s  fa2, fa2, fa3
    fadd.s  fa0, fa0, fa2
    fadd.s  fa4, fa4, fa5
    fadd.s  fa6, fa6, fa7
    fadd.s  fa4, fa4, fa6
    fsw     fa0, 0(a4)
    fsw     fa4, 4(a4)

    addi    a4, a4, 8
    addi    a0, a0, 4
    addi    a6, a6, -1
    bnez    a6, .Lf32d1_output
    ret

    .size   riscv_dotprod_f32_pie_ch1_dual, . - riscv_dotprod_f32_pie_ch1_dual

#endif
