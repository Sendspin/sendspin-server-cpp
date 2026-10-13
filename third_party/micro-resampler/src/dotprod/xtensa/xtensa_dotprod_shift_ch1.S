// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// Xtensa assembly implementation of the mono Q15 dot product with per-block
// shifting, using MAC16 with fused ldinc instructions (non-S3 Xtensa chips).
// Applies an adaptive shift per 8-tap block for coefficient scaling.
//
// Coefficients must be 4-byte aligned. The delay-line pointer may sit at any
// int16 boundary: an odd sample index takes the straddling-word path, which
// reads the two bytes preceding delay_line (always in-bounds for the
// resampler's history buffer, whose base is 16-byte aligned).

#ifdef __XTENSA__

    .text
    .align 4

// 8 taps, samples 4-byte aligned: two taps per sample word, halves l then h.
// Expects a2 pre-adjusted by -4, a3 by -4; samples in m0/m1, coeffs in m2/m3.
// Fused mula.dd.*.ldinc multiplies with the pre-load register contents, so
// each MAC hides one stream load.
.macro mac16_block_mono_even
    ldinc   m0, a2                          // samples [0,1]
    ldinc   m2, a3                          // coeffs [0,1]
    ldinc   m1, a2                          // samples [2,3]
    mula.dd.ll.ldinc m3, a3, m0, m2         // s0*c0, coeffs [2,3]
    mula.dd.hh.ldinc m0, a2, m0, m2         // s1*c1, samples [4,5]
    mula.dd.ll.ldinc m2, a3, m1, m3         // s2*c2, coeffs [4,5]
    mula.dd.hh.ldinc m1, a2, m1, m3         // s3*c3, samples [6,7]
    mula.dd.ll.ldinc m3, a3, m0, m2         // s4*c4, coeffs [6,7]
    mula.dd.hh m0, m2                       // s5*c5
    mula.dd.ll m1, m3                       // s6*c6
    mula.dd.hh m1, m3                       // s7*c7
.endm

// 8 taps, samples at an odd int16 index (pointer % 4 == 2): loads use the
// aligned words straddling the taps, so sample halves swap to h then l.
// Expects a2 pre-adjusted by -6 so the first ldinc hits the aligned word
// holding s0 in its high half. Ends by rewinding one word because the last
// word loaded (s7|s8) is also the first word of the next block.
.macro mac16_block_mono_odd
    ldinc   m0, a2                          // samples [-1,0]
    ldinc   m2, a3                          // coeffs [0,1]
    ldinc   m1, a2                          // samples [1,2]
    mula.dd.hl.ldinc m3, a3, m0, m2         // s0*c0, coeffs [2,3]
    mula.dd.lh.ldinc m0, a2, m1, m2         // s1*c1, samples [3,4]
    mula.dd.hl.ldinc m2, a3, m1, m3         // s2*c2, coeffs [4,5]
    mula.dd.lh.ldinc m1, a2, m0, m3         // s3*c3, samples [5,6]
    mula.dd.hl.ldinc m3, a3, m0, m2         // s4*c4, coeffs [6,7]
    mula.dd.lh.ldinc m0, a2, m1, m2         // s5*c5, samples [7,8]
    mula.dd.hl m1, m3                       // s6*c6
    mula.dd.lh m0, m3                       // s7*c7
    addi    a2, a2, -4                      // next block re-reads s7|s8 word
.endm

// Clear the 40-bit MAC16 accumulator (clobbers the given scratch register).
.macro mac16_clear_acc scratch
    movi    \scratch, 0
    wsr     \scratch, acclo
    wsr     \scratch, acchi
.endm

// Read the 40-bit accumulator, arithmetic-shift it right by SAR via funnel
// shift, and add the 32-bit result into \total (clobbers \tmp1, \tmp2).
.macro mac16_extract_shifted total, tmp1, tmp2
    rsr     \tmp1, acchi                    // high 8 bits, sign-extended
    rsr     \tmp2, acclo                    // low 32 bits
    src     \tmp1, \tmp1, \tmp2             // (acc >> SAR) as int32
    add     \total, \total, \tmp1
.endm

    // Function: xtensa_dotprod_s16_q15_shift_mac16_ch1
    // Computes dot product with per-block shifting (1 channel)
    // Parameters:
    //   a2: delay_line (int16_t*, any int16 alignment)
    //   a3: coeffs (q15_t*, 4-byte aligned)
    //   a4: length (int) - must be multiple of 8
    //   a5: shifts (uint8_t*) - shift value for each block of 8 samples
    //   a6: output (int32_t*) - pointer to single int32 output
    // Returns:
    //   Result stored at output pointer

    .global xtensa_dotprod_s16_q15_shift_mac16_ch1
    .type   xtensa_dotprod_s16_q15_shift_mac16_ch1, @function
xtensa_dotprod_s16_q15_shift_mac16_ch1:
    entry   sp, 32

    movi    a8, 0                           // running total
    srli    a9, a4, 3                       // number of 8-tap blocks
    beqz    a9, .L_ch1_done

    addi    a3, a3, -4                      // arrange coefficient pointer
    bbsi    a2, 1, .L_ch1_odd               // odd sample index?

    addi    a2, a2, -4                      // arrange aligned sample pointer
    loopnez a9, .L_ch1_even_end
.L_ch1_even_loop:
    mac16_clear_acc a10
    mac16_block_mono_even
    l8ui    a11, a5, 0                      // this block's shift
    ssr     a11
    mac16_extract_shifted a8, a10, a12
    addi    a5, a5, 1
.L_ch1_even_end:
    j       .L_ch1_done

.L_ch1_odd:
    addi    a2, a2, -6                      // first load: word holding s0 high
    loopnez a9, .L_ch1_odd_end
.L_ch1_odd_loop:
    mac16_clear_acc a10
    mac16_block_mono_odd
    l8ui    a11, a5, 0
    ssr     a11
    mac16_extract_shifted a8, a10, a12
    addi    a5, a5, 1
.L_ch1_odd_end:

.L_ch1_done:
    s32i    a8, a6, 0
    retw

    .size   xtensa_dotprod_s16_q15_shift_mac16_ch1, . - xtensa_dotprod_s16_q15_shift_mac16_ch1

#endif // __XTENSA__
