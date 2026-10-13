// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// ESP32-P4 scalar-FPU implementation for 2-channel interleaved Float32 FIR
// filter. Supports any length that is a multiple of 8
//
// The P4 has no floating-point SIMD, so this is software-pipelined scalar
// fmadd.s code: two accumulators per channel (even/odd frames) break the
// FPU's fused-MAC latency chain. The loops are plain branch loops: live FPU
// state across an esp.lp.setup hardware loop hits the P4's hwloop state-save
// erratum (see the loop comments).

#if defined(__riscv) && \
    (defined(CONFIG_IDF_TARGET_ESP32P4) || defined(ESP32P4) || defined(CONFIG_IDF_TARGET_ESP32S31) || \
     defined(ESP32S31))

    .text
    .align 4

    // Function: riscv_dotprod_f32_pie_ch2
    // Computes dot products of interleaved stereo float samples and
    // coefficients. Even frames accumulate into acc pair (L0, R0), odd
    // frames into (L1, R1); each channel reduces as even + odd. Any
    // dual/batched variant must replicate this exact association to stay
    // bit-identical.
    // Parameters:
    //   a0: samples (float*) - interleaved LR frames, any 4-byte alignment
    //   a1: coeffs (float*) - any 4-byte alignment
    //   a2: length (must be multiple of 8)
    //   a3: output (float*) - [L, R] float pair

    .global riscv_dotprod_f32_pie_ch2
    .type   riscv_dotprod_f32_pie_ch2, @function
riscv_dotprod_f32_pie_ch2:
    fmv.w.x ft8, zero           // L even
    fmv.w.x ft9, zero           // R even
    fmv.w.x ft10, zero          // L odd
    fmv.w.x ft11, zero          // R odd
    slli    t4, a2, 3
    add     t4, a0, t4          // t4 = sample end (loop exit address)

    // 8 frames (16 MACs) per iteration as four 2-frame pairs alternating
    // between the ft0-ft5 and fa0-fa5 register sets, so each pair's loads
    // land a full pair ahead of its fmadds; the exit test compares the
    // sample pointer against its end, so no counter is kept. Plain branch
    // loop: holding live FPU accumulators across an esp.lp.setup hardware
    // loop produced run-to-run nondeterminism under FreeRTOS (the P4
    // hardware-loop state-save erratum), so the f32 kernels avoid hwloops.
.Lf32ch2_loop:
    flw     ft0, 0(a1)          // coeff j
    flw     ft1, 0(a0)          // L j
    flw     ft2, 4(a0)          // R j
    flw     ft3, 4(a1)          // coeff j+1
    flw     ft4, 8(a0)          // L j+1
    flw     ft5, 12(a0)         // R j+1
    flw     fa0, 8(a1)
    flw     fa1, 16(a0)
    flw     fa2, 20(a0)
    flw     fa3, 12(a1)
    flw     fa4, 24(a0)
    flw     fa5, 28(a0)
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft0, ft2, ft9
    fmadd.s ft10, ft3, ft4, ft10
    fmadd.s ft11, ft3, ft5, ft11
    flw     ft0, 16(a1)
    flw     ft1, 32(a0)
    flw     ft2, 36(a0)
    flw     ft3, 20(a1)
    flw     ft4, 40(a0)
    flw     ft5, 44(a0)
    fmadd.s ft8, fa0, fa1, ft8
    fmadd.s ft9, fa0, fa2, ft9
    fmadd.s ft10, fa3, fa4, ft10
    fmadd.s ft11, fa3, fa5, ft11
    flw     fa0, 24(a1)
    flw     fa1, 48(a0)
    flw     fa2, 52(a0)
    flw     fa3, 28(a1)
    flw     fa4, 56(a0)
    flw     fa5, 60(a0)
    fmadd.s ft8, ft0, ft1, ft8
    fmadd.s ft9, ft0, ft2, ft9
    fmadd.s ft10, ft3, ft4, ft10
    fmadd.s ft11, ft3, ft5, ft11
    addi    a1, a1, 32
    addi    a0, a0, 64
    fmadd.s ft8, fa0, fa1, ft8
    fmadd.s ft9, fa0, fa2, ft9
    fmadd.s ft10, fa3, fa4, ft10
    fmadd.s ft11, fa3, fa5, ft11
    bne     a0, t4, .Lf32ch2_loop

    // Reduce each channel: even + odd
    fadd.s  ft8, ft8, ft10
    fadd.s  ft9, ft9, ft11
    fsw     ft8, 0(a3)
    fsw     ft9, 4(a3)
    ret

    .size   riscv_dotprod_f32_pie_ch2, . - riscv_dotprod_f32_pie_ch2

    // Function: riscv_dotprod_f32_pie_ch2_dual
    // Stereo variant of the ch1 dual kernel: computes both blend rows' dot
    // products for up to 4 outputs, loading each LR frame once. Row
    // accumulation is even/odd frame pairs reduced even + odd per channel -
    // the same association as the single-output kernel, so results are
    // bit-identical to it. Outputs are written as [row A L, row A R,
    // row B L, row B R] quads per output.
    // Parameters:
    //   a0: sample_ptrs (const float* const*) - one delay-line pointer per output
    //   a1: coeffs_a (float*) - row A coefficients
    //   a2: coeffs_b (float*) - row B coefficients
    //   a3: length (bits 0-15, must be multiple of 8) | output count (bits 20-22)
    //   a4: output (float*) - [A L, A R, B L, B R] quad per output

    .align  4
    .global riscv_dotprod_f32_pie_ch2_dual
    .type   riscv_dotprod_f32_pie_ch2_dual, @function
riscv_dotprod_f32_pie_ch2_dual:
    srli    a6, a3, 20
    andi    a6, a6, 7           // a6 = output count
    slli    a7, a3, 16
    srli    a7, a7, 13          // a7 = length * 8 (LR row bytes)

.Lf32d2_output:
    lw      a5, 0(a0)           // a5 = samples for this output
    mv      t5, a1              // t5 = row A coefficient cursor
    mv      t6, a2              // t6 = row B coefficient cursor
    fmv.w.x fa0, zero           // A L even
    fmv.w.x fa1, zero           // A R even
    fmv.w.x fa2, zero           // A L odd
    fmv.w.x fa3, zero           // A R odd
    fmv.w.x fa4, zero           // B L even
    fmv.w.x fa5, zero           // B R even
    fmv.w.x fa6, zero           // B L odd
    fmv.w.x fa7, zero           // B R odd
    add     t3, a5, a7          // t3 = sample end (loop exit address)

    // 8 frames (32 MACs) per iteration as four 2-frame pairs: each frame's
    // L/R feed both rows (plain branch loop; see the single-output kernel's
    // hwloop note)
.Lf32d2_loop:
    flw     ft0, 0(a5)          // L j
    flw     ft1, 4(a5)          // R j
    flw     ft2, 0(t5)          // row A coeff j
    flw     ft3, 0(t6)          // row B coeff j
    flw     ft4, 8(a5)          // L j+1
    flw     ft5, 12(a5)         // R j+1
    flw     ft6, 4(t5)          // row A coeff j+1
    flw     ft7, 4(t6)          // row B coeff j+1
    fmadd.s fa0, ft2, ft0, fa0
    fmadd.s fa1, ft2, ft1, fa1
    fmadd.s fa4, ft3, ft0, fa4
    fmadd.s fa5, ft3, ft1, fa5
    fmadd.s fa2, ft6, ft4, fa2
    fmadd.s fa3, ft6, ft5, fa3
    fmadd.s fa6, ft7, ft4, fa6
    fmadd.s fa7, ft7, ft5, fa7
    flw     ft0, 16(a5)
    flw     ft1, 20(a5)
    flw     ft2, 8(t5)
    flw     ft3, 8(t6)
    flw     ft4, 24(a5)
    flw     ft5, 28(a5)
    flw     ft6, 12(t5)
    flw     ft7, 12(t6)
    fmadd.s fa0, ft2, ft0, fa0
    fmadd.s fa1, ft2, ft1, fa1
    fmadd.s fa4, ft3, ft0, fa4
    fmadd.s fa5, ft3, ft1, fa5
    fmadd.s fa2, ft6, ft4, fa2
    fmadd.s fa3, ft6, ft5, fa3
    fmadd.s fa6, ft7, ft4, fa6
    fmadd.s fa7, ft7, ft5, fa7
    flw     ft0, 32(a5)
    flw     ft1, 36(a5)
    flw     ft2, 16(t5)
    flw     ft3, 16(t6)
    flw     ft4, 40(a5)
    flw     ft5, 44(a5)
    flw     ft6, 20(t5)
    flw     ft7, 20(t6)
    fmadd.s fa0, ft2, ft0, fa0
    fmadd.s fa1, ft2, ft1, fa1
    fmadd.s fa4, ft3, ft0, fa4
    fmadd.s fa5, ft3, ft1, fa5
    fmadd.s fa2, ft6, ft4, fa2
    fmadd.s fa3, ft6, ft5, fa3
    fmadd.s fa6, ft7, ft4, fa6
    fmadd.s fa7, ft7, ft5, fa7
    flw     ft0, 48(a5)
    flw     ft1, 52(a5)
    flw     ft2, 24(t5)
    flw     ft3, 24(t6)
    flw     ft4, 56(a5)
    flw     ft5, 60(a5)
    flw     ft6, 28(t5)
    flw     ft7, 28(t6)
    addi    a5, a5, 64
    addi    t5, t5, 32
    addi    t6, t6, 32
    fmadd.s fa0, ft2, ft0, fa0
    fmadd.s fa1, ft2, ft1, fa1
    fmadd.s fa4, ft3, ft0, fa4
    fmadd.s fa5, ft3, ft1, fa5
    fmadd.s fa2, ft6, ft4, fa2
    fmadd.s fa3, ft6, ft5, fa3
    fmadd.s fa6, ft7, ft4, fa6
    fmadd.s fa7, ft7, ft5, fa7
    bne     a5, t3, .Lf32d2_loop

    // Reduce each row and channel: even + odd
    fadd.s  fa0, fa0, fa2
    fadd.s  fa1, fa1, fa3
    fadd.s  fa4, fa4, fa6
    fadd.s  fa5, fa5, fa7
    fsw     fa0, 0(a4)
    fsw     fa1, 4(a4)
    fsw     fa4, 8(a4)
    fsw     fa5, 12(a4)

    addi    a4, a4, 16
    addi    a0, a0, 4
    addi    a6, a6, -1
    bnez    a6, .Lf32d2_output
    ret

    .size   riscv_dotprod_f32_pie_ch2_dual, . - riscv_dotprod_f32_pie_ch2_dual

#endif
