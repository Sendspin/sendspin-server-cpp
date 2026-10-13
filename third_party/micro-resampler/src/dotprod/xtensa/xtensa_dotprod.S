// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

// Xtensa assembly implementation of the plain (no block shift) Q15 dot
// product using MAC16 with fused ldinc instructions, following the
// software-pipelined schedule of esp-dsp's dsps_dotprod_s16_ae32.
//
// Both pointers must be 4-byte aligned: ldinc performs 32-bit loads and the
// low two address bits are ignored (or trap) on unaligned accesses.

#ifdef __XTENSA__

    .text
    .align 4

    // Function: xtensa_dotprod_s16_q15_mac16
    // Parameters:
    //   a2: delay_line (int16_t*, 4-byte aligned)
    //   a3: coeffs (q15_t*, 4-byte aligned)
    //   a4: length (int, any value >= 0)
    // Returns:
    //   a2: low 32 bits of the 40-bit accumulator

    .global xtensa_dotprod_s16_q15_mac16
    .type   xtensa_dotprod_s16_q15_mac16, @function
xtensa_dotprod_s16_q15_mac16:
    entry   sp, 32

    // Clear accumulator
    movi    a5, 0
    wsr     a5, acclo
    wsr     a5, acchi

    // Arrange pointers for ldinc pre-increment
    addi    a2, a2, -4
    addi    a3, a3, -4

    blti    a4, 4, .L_remainder     // fewer than 4 taps: remainder only

    // Pipelined main body covers 4*(count+1) taps; each fused
    // mula.dd.*.ldinc multiplies with the pre-load register contents
    srli    a5, a4, 2
    addi    a5, a5, -1              // count = length/4 - 1

    ldinc   m0, a2                  // samples [0,1]
    ldinc   m2, a3                  // coeffs [0,1]
    ldinc   m1, a2                  // samples [2,3]
    mula.dd.ll.ldinc m3, a3, m0, m2 // s0*c0, coeffs [2,3]

    loopnez a5, .L_main_end
.L_main:
    mula.dd.hh.ldinc m0, a2, m0, m2 // odd tap, next samples
    mula.dd.ll.ldinc m2, a3, m1, m3 // even tap, next coeffs
    mula.dd.hh.ldinc m1, a2, m1, m3 // odd tap, next samples
    mula.dd.ll.ldinc m3, a3, m0, m2 // even tap, next coeffs
.L_main_end:

    mula.dd.hh m0, m2
    mula.dd.ll m1, m3
    mula.dd.hh m1, m3

.L_remainder:
    // 2 leftover taps
    bbci    a4, 1, .L_check_one
    ldinc   m0, a2
    ldinc   m2, a3
    mula.dd.ll m0, m2
    mula.dd.hh m0, m2

.L_check_one:
    // 1 leftover tap (reads one halfword past both arrays; harmless)
    bbci    a4, 0, .L_done
    ldinc   m0, a2
    ldinc   m2, a3
    mula.dd.ll m0, m2

.L_done:
    rsr     a2, acclo               // Get accumulator result
    retw

    .size   xtensa_dotprod_s16_q15_mac16, . - xtensa_dotprod_s16_q15_mac16

#endif // __XTENSA__
