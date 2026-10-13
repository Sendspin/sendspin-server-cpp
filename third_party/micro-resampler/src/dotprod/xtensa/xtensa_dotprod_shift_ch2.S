// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

// Xtensa assembly implementation of the stereo (interleaved) Q15 dot product
// with per-block shifting, using MAC16 with fused ldinc instructions (non-S3
// Xtensa chips). Applies an adaptive shift per 8-tap block.
//
// MAC16 has a single 40-bit accumulator, so each 8-tap block runs twice: a
// left pass over the block's 8 frames, a pointer rewind, then a right pass.
// Each aligned 32-bit frame load holds {left, right} in its {low, high}
// halves, so the two passes execute the same loads and differ only in which
// sample half the MULA.DD opcodes select.
//
// Both pointers must be 4-byte aligned; stereo frames are 4 bytes, so any
// frame boundary in the (16-byte aligned) history buffer qualifies.

#ifdef __XTENSA__

    .text
    .align 4

// 8 taps of one stereo channel. qa multiplies the selected frame half
// against the even coefficient half, qb against the odd: the left channel
// passes (ll, lh), the right channel (hl, hh). Coefficients advance one word
// per two taps; frames advance one word per tap (8 words per block).
// Expects a2 and a3 pre-adjusted by -4; frames in m0/m1, coeffs in m2/m3.
// Fused mula.dd.*.ldinc multiplies with the pre-load register contents.
.macro mac16_block_stereo qa, qb
    ldinc   m0, a2                          // frame 0
    ldinc   m2, a3                          // coeffs [0,1]
    ldinc   m1, a2                          // frame 1
    mula.dd.\qa\().ldinc m3, a3, m0, m2     // tap 0, coeffs [2,3]
    mula.dd.\qb\().ldinc m0, a2, m1, m2     // tap 1, frame 2
    ldinc   m1, a2                          // frame 3
    mula.dd.\qa\().ldinc m2, a3, m0, m3     // tap 2, coeffs [4,5]
    mula.dd.\qb\().ldinc m0, a2, m1, m3     // tap 3, frame 4
    ldinc   m1, a2                          // frame 5
    mula.dd.\qa\().ldinc m3, a3, m0, m2     // tap 4, coeffs [6,7]
    mula.dd.\qb\().ldinc m0, a2, m1, m2     // tap 5, frame 6
    ldinc   m1, a2                          // frame 7
    mula.dd.\qa m0, m3                      // tap 6
    mula.dd.\qb m1, m3                      // tap 7
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

    // Function: xtensa_dotprod_s16_q15_shift_mac16_ch2
    // Computes dot products with per-block shifting (2 interleaved channels)
    // Parameters:
    //   a2: delay_line (int16_t*, interleaved L/R frames, 4-byte aligned)
    //   a3: coeffs (q15_t*, 4-byte aligned)
    //   a4: length (int) - taps per channel, must be multiple of 8
    //   a5: shifts (uint8_t*) - shift value for each block of 8 taps
    //   a6: output (int32_t[2]) - left then right result
    // Returns:
    //   Results stored at output pointer

    .global xtensa_dotprod_s16_q15_shift_mac16_ch2
    .type   xtensa_dotprod_s16_q15_shift_mac16_ch2, @function
xtensa_dotprod_s16_q15_shift_mac16_ch2:
    entry   sp, 32

    movi    a8, 0                           // left running total
    movi    a13, 0                          // right running total
    srli    a9, a4, 3                       // number of 8-tap blocks
    beqz    a9, .L_ch2_done

    addi    a2, a2, -4                      // arrange frame pointer
    addi    a3, a3, -4                      // arrange coefficient pointer

    loopnez a9, .L_ch2_loop_end
.L_ch2_loop:
    // Left pass over this block's 8 frames
    mac16_clear_acc a10
    mac16_block_stereo ll, lh
    l8ui    a11, a5, 0                      // shift shared by both channels
    ssr     a11
    mac16_extract_shifted a8, a10, a12

    // Rewind to the top of the block: 8 frames and 4 coefficient words
    addi    a2, a2, -32
    addi    a3, a3, -16

    // Right pass over the same frames (SAR still holds the shift)
    mac16_clear_acc a10
    mac16_block_stereo hl, hh
    mac16_extract_shifted a13, a10, a12
    addi    a5, a5, 1
.L_ch2_loop_end:

.L_ch2_done:
    s32i    a8, a6, 0
    s32i    a13, a6, 4
    retw

    .size   xtensa_dotprod_s16_q15_shift_mac16_ch2, . - xtensa_dotprod_s16_q15_shift_mac16_ch2

#endif // __XTENSA__
