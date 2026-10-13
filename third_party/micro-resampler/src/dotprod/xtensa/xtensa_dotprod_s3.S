// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025 Kevin Ahrendt

// ESP32-S3 SIMD implementation of dot product functions
// using LX7 vector instructions (EE.* instructions)

#if defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))

    .text
    .align 4

    // Function: xtensa_dotprod_s16_q15_simd
    // Computes dot product of int16 samples and Q15 coefficients using SIMD
    // Parameters:
    //   a2: delay_line (int16_t*) - any alignment supported
    //   a3: coeffs (q15_t*) - must be 16-byte aligned
    //   a4: length (int) - must be multiple of 8
    // Returns:
    //   a2: dot product result (int32_t)

    .global xtensa_dotprod_s16_q15_simd
    .type   xtensa_dotprod_s16_q15_simd, @function
xtensa_dotprod_s16_q15_simd:
    entry   sp, 32

    // Zero accumulator
    ee.zero.accx

    // Calculate loop count for 24-sample batches (magic constant for /24)
    movi        a11, 178956971
    muluh       a5, a4, a11

    // Preload first samples from potentially misaligned delay_line
    // QUP handles misalignment by combining two aligned loads with shifting
    ee.ld.128.usar.ip  q0, a2, 16       // Load first aligned 16 bytes
    ee.ld.128.usar.ip  q1, a2, 16       // Load second aligned 16 bytes
    ee.src.q.ld.ip     q2, a2, 16, q0, q1   // Shift and align to correct position

    ee.vld.128.ip       q3, a3, 16      // Preload first 8 coefficients

    movi.n                  a6, 24
    mul16s                  a6, a6, a5    // Total samples in main loop
    sub                     a6, a4, a6    // Calculate remainder

    // Main loop: 24 samples per iteration (3x QUP instructions, 8 samples each)
    loopnez a5, .L_simd_loop_end
.L_simd_loop:
    // Each iteration processes 24 samples using 3 fused operations
    // vmulas.qup: multiply-accumulate + load next samples + align for misalignment
    ee.vld.128.ip       q4, a3, 16      // Load next 8 coefficients
    ee.vmulas.s16.accx.ld.ip.qup  q0, a2, 16, q0, q3, q1, q2    // MAC + align 8 samples

    ee.vld.128.ip       q5, a3, 16
    ee.vmulas.s16.accx.ld.ip.qup  q1, a2, 16, q1, q4, q2, q0    // MAC + align 8 samples

    ee.vld.128.ip       q3, a3, 16
    ee.vmulas.s16.accx.ld.ip.qup  q2, a2, 16, q2, q5, q0, q1    // MAC + align 8 samples
.L_simd_loop_end:

.L_simd_remainder:
    beqz    a6, .L_done

    // Process remainder: 8 or 16 samples (filter length must be multiple of 8)
    movi.n  a7, 8
    beq     a6, a7, .L_remainder_8

.L_remainder_16:
    // Handle 16-sample remainder
    ee.vld.128.ip                    q4, a3, 16
    ee.vmulas.s16.accx.ld.ip.qup     q0, a2, 16, q0, q3, q1, q2

    ee.vmulas.s16.accx    q1, q4
    j       .L_done

.L_remainder_8:
    // Handle 8-sample remainder
    ee.vmulas.s16.accx    q0, q3

.L_done:
    // Extract result from ACCX
    rur.accx_0  a2
    retw.n

    .size   xtensa_dotprod_s16_q15_simd, . - xtensa_dotprod_s16_q15_simd

#endif // defined(__XTENSA__) && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ESP32S3))
