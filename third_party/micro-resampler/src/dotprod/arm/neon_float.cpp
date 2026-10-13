// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include "../dotprod.h"  // Full definitions (includes neon.h via dotprod_dispatch.h)
#include "formats.h"
#include <arm_neon.h>

namespace micro_resampler {
namespace dotprod {
namespace neon {

// ============================================================================
// Float64Format - Fixed Precision Support (Double Precision)
// ============================================================================
// Float64Format uses double for samples, coefficients, and accumulators.
// Float does NOT use adaptive Q - it uses fixed precision path.
// NEON on AArch64 can process 2 doubles at a time using float64x2_t.

// ============================================================================
// Fixed Precision - 1 Channel (Float64Format)
// ============================================================================

static void compute_fixed_ch1_float(const DotProdConfig<Float64Format>& config,
                                    Float64Format::accumulator_t* output) {
    float64x2_t acc0 = vdupq_n_f64(0.0);
    float64x2_t acc1 = vdupq_n_f64(0.0);
    float64x2_t acc2 = vdupq_n_f64(0.0);
    float64x2_t acc3 = vdupq_n_f64(0.0);
    float64x2_t acc4 = vdupq_n_f64(0.0);
    float64x2_t acc5 = vdupq_n_f64(0.0);
    float64x2_t acc6 = vdupq_n_f64(0.0);
    float64x2_t acc7 = vdupq_n_f64(0.0);

    int i = 0;

    // Process 16 doubles per iteration (8 vectors x 2 doubles)
    constexpr int UNROLL_STEP = 16;
    for (; i + UNROLL_STEP - 1 < config.length; i += UNROLL_STEP) {
        float64x2_t s0 = vld1q_f64(&config.samples[i]);
        float64x2_t s1 = vld1q_f64(&config.samples[i + 2]);
        float64x2_t s2 = vld1q_f64(&config.samples[i + 4]);
        float64x2_t s3 = vld1q_f64(&config.samples[i + 6]);
        float64x2_t s4 = vld1q_f64(&config.samples[i + 8]);
        float64x2_t s5 = vld1q_f64(&config.samples[i + 10]);
        float64x2_t s6 = vld1q_f64(&config.samples[i + 12]);
        float64x2_t s7 = vld1q_f64(&config.samples[i + 14]);

        float64x2_t c0 = vld1q_f64(&config.coeffs[i]);
        float64x2_t c1 = vld1q_f64(&config.coeffs[i + 2]);
        float64x2_t c2 = vld1q_f64(&config.coeffs[i + 4]);
        float64x2_t c3 = vld1q_f64(&config.coeffs[i + 6]);
        float64x2_t c4 = vld1q_f64(&config.coeffs[i + 8]);
        float64x2_t c5 = vld1q_f64(&config.coeffs[i + 10]);
        float64x2_t c6 = vld1q_f64(&config.coeffs[i + 12]);
        float64x2_t c7 = vld1q_f64(&config.coeffs[i + 14]);

        acc0 = vfmaq_f64(acc0, s0, c0);
        acc1 = vfmaq_f64(acc1, s1, c1);
        acc2 = vfmaq_f64(acc2, s2, c2);
        acc3 = vfmaq_f64(acc3, s3, c3);
        acc4 = vfmaq_f64(acc4, s4, c4);
        acc5 = vfmaq_f64(acc5, s5, c5);
        acc6 = vfmaq_f64(acc6, s6, c6);
        acc7 = vfmaq_f64(acc7, s7, c7);
    }

    // Combine accumulators (tree reduction)
    acc0 = vaddq_f64(acc0, acc1);
    acc2 = vaddq_f64(acc2, acc3);
    acc4 = vaddq_f64(acc4, acc5);
    acc6 = vaddq_f64(acc6, acc7);

    acc0 = vaddq_f64(acc0, acc2);
    acc4 = vaddq_f64(acc4, acc6);

    acc0 = vaddq_f64(acc0, acc4);

    double result = vaddvq_f64(acc0);

    // Handle remainder (0-15 elements) - could vectorize this too
    for (; i < config.length; i++) {
        result += config.samples[i] * config.coeffs[i];
    }

    *output = result;
}

// ============================================================================
// Fixed Precision - 2 Channels (Float64Format)
// ============================================================================

static void compute_fixed_ch2_float(const DotProdConfig<Float64Format>& config,
                                    Float64Format::accumulator_t* output) {
    // 4 accumulators per channel for better instruction-level parallelism
    float64x2_t acc_l0 = vdupq_n_f64(0.0);
    float64x2_t acc_l1 = vdupq_n_f64(0.0);
    float64x2_t acc_l2 = vdupq_n_f64(0.0);
    float64x2_t acc_l3 = vdupq_n_f64(0.0);
    float64x2_t acc_r0 = vdupq_n_f64(0.0);
    float64x2_t acc_r1 = vdupq_n_f64(0.0);
    float64x2_t acc_r2 = vdupq_n_f64(0.0);
    float64x2_t acc_r3 = vdupq_n_f64(0.0);

    int i = 0;

    // Process 8 stereo pairs at a time (8 coefficients, 16 samples)
    for (; i + 7 < config.length; i += 8) {
        // Load 8 coefficients (4 vectors x 2 doubles)
        float64x2_t c0 = vld1q_f64(&config.coeffs[i]);
        float64x2_t c1 = vld1q_f64(&config.coeffs[i + 2]);
        float64x2_t c2 = vld1q_f64(&config.coeffs[i + 4]);
        float64x2_t c3 = vld1q_f64(&config.coeffs[i + 6]);

        // Load and deinterleave 8 stereo pairs
        float64x2x2_t stereo0 = vld2q_f64(&config.samples[static_cast<size_t>(i) * 2]);
        float64x2x2_t stereo1 = vld2q_f64(&config.samples[static_cast<size_t>(i + 2) * 2]);
        float64x2x2_t stereo2 = vld2q_f64(&config.samples[static_cast<size_t>(i + 4) * 2]);
        float64x2x2_t stereo3 = vld2q_f64(&config.samples[static_cast<size_t>(i + 6) * 2]);

        // Fused multiply-add to separate accumulators
        acc_l0 = vfmaq_f64(acc_l0, stereo0.val[0], c0);
        acc_r0 = vfmaq_f64(acc_r0, stereo0.val[1], c0);
        acc_l1 = vfmaq_f64(acc_l1, stereo1.val[0], c1);
        acc_r1 = vfmaq_f64(acc_r1, stereo1.val[1], c1);
        acc_l2 = vfmaq_f64(acc_l2, stereo2.val[0], c2);
        acc_r2 = vfmaq_f64(acc_r2, stereo2.val[1], c2);
        acc_l3 = vfmaq_f64(acc_l3, stereo3.val[0], c3);
        acc_r3 = vfmaq_f64(acc_r3, stereo3.val[1], c3);
    }

    // Tree reduction of accumulators
    acc_l0 = vaddq_f64(acc_l0, acc_l1);
    acc_l2 = vaddq_f64(acc_l2, acc_l3);
    acc_l0 = vaddq_f64(acc_l0, acc_l2);

    acc_r0 = vaddq_f64(acc_r0, acc_r1);
    acc_r2 = vaddq_f64(acc_r2, acc_r3);
    acc_r0 = vaddq_f64(acc_r0, acc_r2);

    // Horizontal sums
    double result_l = vaddvq_f64(acc_l0);
    double result_r = vaddvq_f64(acc_r0);

    // Handle remainder (0-7 samples)
    for (; i < config.length; i++) {
        result_l += config.samples[static_cast<size_t>(i) * 2] * config.coeffs[i];
        result_r += config.samples[i * 2 + 1] * config.coeffs[i];
    }

    output[0] = result_l;
    output[1] = result_r;
}

// ============================================================================
// Fixed Precision - 3 Channels (Float64Format)
// ============================================================================

static void compute_fixed_3ch_float(const DotProdConfig<Float64Format>& config,
                                    Float64Format::accumulator_t* output) {
    // 4 accumulators per channel for better instruction-level parallelism
    float64x2_t acc0_0 = vdupq_n_f64(0.0), acc0_1 = vdupq_n_f64(0.0);
    float64x2_t acc0_2 = vdupq_n_f64(0.0), acc0_3 = vdupq_n_f64(0.0);
    float64x2_t acc1_0 = vdupq_n_f64(0.0), acc1_1 = vdupq_n_f64(0.0);
    float64x2_t acc1_2 = vdupq_n_f64(0.0), acc1_3 = vdupq_n_f64(0.0);
    float64x2_t acc2_0 = vdupq_n_f64(0.0), acc2_1 = vdupq_n_f64(0.0);
    float64x2_t acc2_2 = vdupq_n_f64(0.0), acc2_3 = vdupq_n_f64(0.0);

    int i = 0;

    // Process 8 frames at a time (8 coefficients, 24 samples)
    for (; i + 7 < config.length; i += 8) {
        // Load 8 coefficients
        float64x2_t c0 = vld1q_f64(&config.coeffs[i]);
        float64x2_t c1 = vld1q_f64(&config.coeffs[i + 2]);
        float64x2_t c2 = vld1q_f64(&config.coeffs[i + 4]);
        float64x2_t c3 = vld1q_f64(&config.coeffs[i + 6]);

        // Load and deinterleave 8 frames of 3-channel audio
        float64x2x3_t s0 = vld3q_f64(&config.samples[static_cast<size_t>(i) * 3]);
        float64x2x3_t s1 = vld3q_f64(&config.samples[static_cast<size_t>(i + 2) * 3]);
        float64x2x3_t s2 = vld3q_f64(&config.samples[static_cast<size_t>(i + 4) * 3]);
        float64x2x3_t s3 = vld3q_f64(&config.samples[static_cast<size_t>(i + 6) * 3]);

        // Fused multiply-add to separate accumulators
        acc0_0 = vfmaq_f64(acc0_0, s0.val[0], c0);
        acc1_0 = vfmaq_f64(acc1_0, s0.val[1], c0);
        acc2_0 = vfmaq_f64(acc2_0, s0.val[2], c0);

        acc0_1 = vfmaq_f64(acc0_1, s1.val[0], c1);
        acc1_1 = vfmaq_f64(acc1_1, s1.val[1], c1);
        acc2_1 = vfmaq_f64(acc2_1, s1.val[2], c1);

        acc0_2 = vfmaq_f64(acc0_2, s2.val[0], c2);
        acc1_2 = vfmaq_f64(acc1_2, s2.val[1], c2);
        acc2_2 = vfmaq_f64(acc2_2, s2.val[2], c2);

        acc0_3 = vfmaq_f64(acc0_3, s3.val[0], c3);
        acc1_3 = vfmaq_f64(acc1_3, s3.val[1], c3);
        acc2_3 = vfmaq_f64(acc2_3, s3.val[2], c3);
    }

    // Tree reduction of accumulators
    acc0_0 = vaddq_f64(acc0_0, acc0_1);
    acc0_2 = vaddq_f64(acc0_2, acc0_3);
    acc0_0 = vaddq_f64(acc0_0, acc0_2);

    acc1_0 = vaddq_f64(acc1_0, acc1_1);
    acc1_2 = vaddq_f64(acc1_2, acc1_3);
    acc1_0 = vaddq_f64(acc1_0, acc1_2);

    acc2_0 = vaddq_f64(acc2_0, acc2_1);
    acc2_2 = vaddq_f64(acc2_2, acc2_3);
    acc2_0 = vaddq_f64(acc2_0, acc2_2);

    double result[3] = {vaddvq_f64(acc0_0), vaddvq_f64(acc1_0), vaddvq_f64(acc2_0)};

    // Handle remainder (0-7 frames)
    for (; i < config.length; i++) {
        result[0] += config.samples[i * 3 + 0] * config.coeffs[i];
        result[1] += config.samples[i * 3 + 1] * config.coeffs[i];
        result[2] += config.samples[i * 3 + 2] * config.coeffs[i];
    }

    output[0] = result[0];
    output[1] = result[1];
    output[2] = result[2];
}

// ============================================================================
// Fixed Precision - 4 Channels (Float64Format)
// ============================================================================

static void compute_fixed_4ch_float(const DotProdConfig<Float64Format>& config,
                                    Float64Format::accumulator_t* output) {
    // 4 accumulators per channel for better instruction-level parallelism
    float64x2_t acc0_0 = vdupq_n_f64(0.0), acc0_1 = vdupq_n_f64(0.0);
    float64x2_t acc0_2 = vdupq_n_f64(0.0), acc0_3 = vdupq_n_f64(0.0);
    float64x2_t acc1_0 = vdupq_n_f64(0.0), acc1_1 = vdupq_n_f64(0.0);
    float64x2_t acc1_2 = vdupq_n_f64(0.0), acc1_3 = vdupq_n_f64(0.0);
    float64x2_t acc2_0 = vdupq_n_f64(0.0), acc2_1 = vdupq_n_f64(0.0);
    float64x2_t acc2_2 = vdupq_n_f64(0.0), acc2_3 = vdupq_n_f64(0.0);
    float64x2_t acc3_0 = vdupq_n_f64(0.0), acc3_1 = vdupq_n_f64(0.0);
    float64x2_t acc3_2 = vdupq_n_f64(0.0), acc3_3 = vdupq_n_f64(0.0);

    int i = 0;

    // Process 8 frames at a time (8 coefficients, 32 samples)
    for (; i + 7 < config.length; i += 8) {
        // Load 8 coefficients
        float64x2_t c0 = vld1q_f64(&config.coeffs[i]);
        float64x2_t c1 = vld1q_f64(&config.coeffs[i + 2]);
        float64x2_t c2 = vld1q_f64(&config.coeffs[i + 4]);
        float64x2_t c3 = vld1q_f64(&config.coeffs[i + 6]);

        // Load and deinterleave 8 frames of 4-channel audio
        float64x2x4_t s0 = vld4q_f64(&config.samples[static_cast<size_t>(i) * 4]);
        float64x2x4_t s1 = vld4q_f64(&config.samples[static_cast<size_t>(i + 2) * 4]);
        float64x2x4_t s2 = vld4q_f64(&config.samples[static_cast<size_t>(i + 4) * 4]);
        float64x2x4_t s3 = vld4q_f64(&config.samples[static_cast<size_t>(i + 6) * 4]);

        // Fused multiply-add to separate accumulators
        acc0_0 = vfmaq_f64(acc0_0, s0.val[0], c0);
        acc1_0 = vfmaq_f64(acc1_0, s0.val[1], c0);
        acc2_0 = vfmaq_f64(acc2_0, s0.val[2], c0);
        acc3_0 = vfmaq_f64(acc3_0, s0.val[3], c0);

        acc0_1 = vfmaq_f64(acc0_1, s1.val[0], c1);
        acc1_1 = vfmaq_f64(acc1_1, s1.val[1], c1);
        acc2_1 = vfmaq_f64(acc2_1, s1.val[2], c1);
        acc3_1 = vfmaq_f64(acc3_1, s1.val[3], c1);

        acc0_2 = vfmaq_f64(acc0_2, s2.val[0], c2);
        acc1_2 = vfmaq_f64(acc1_2, s2.val[1], c2);
        acc2_2 = vfmaq_f64(acc2_2, s2.val[2], c2);
        acc3_2 = vfmaq_f64(acc3_2, s2.val[3], c2);

        acc0_3 = vfmaq_f64(acc0_3, s3.val[0], c3);
        acc1_3 = vfmaq_f64(acc1_3, s3.val[1], c3);
        acc2_3 = vfmaq_f64(acc2_3, s3.val[2], c3);
        acc3_3 = vfmaq_f64(acc3_3, s3.val[3], c3);
    }

    // Tree reduction of accumulators
    acc0_0 = vaddq_f64(acc0_0, acc0_1);
    acc0_2 = vaddq_f64(acc0_2, acc0_3);
    acc0_0 = vaddq_f64(acc0_0, acc0_2);

    acc1_0 = vaddq_f64(acc1_0, acc1_1);
    acc1_2 = vaddq_f64(acc1_2, acc1_3);
    acc1_0 = vaddq_f64(acc1_0, acc1_2);

    acc2_0 = vaddq_f64(acc2_0, acc2_1);
    acc2_2 = vaddq_f64(acc2_2, acc2_3);
    acc2_0 = vaddq_f64(acc2_0, acc2_2);

    acc3_0 = vaddq_f64(acc3_0, acc3_1);
    acc3_2 = vaddq_f64(acc3_2, acc3_3);
    acc3_0 = vaddq_f64(acc3_0, acc3_2);

    double result[4] = {vaddvq_f64(acc0_0), vaddvq_f64(acc1_0), vaddvq_f64(acc2_0),
                        vaddvq_f64(acc3_0)};

    // Handle remainder (0-7 frames)
    for (; i < config.length; i++) {
        result[0] += config.samples[i * 4 + 0] * config.coeffs[i];
        result[1] += config.samples[i * 4 + 1] * config.coeffs[i];
        result[2] += config.samples[i * 4 + 2] * config.coeffs[i];
        result[3] += config.samples[i * 4 + 3] * config.coeffs[i];
    }

    output[0] = result[0];
    output[1] = result[1];
    output[2] = result[2];
    output[3] = result[3];
}

// ============================================================================
// Fixed Precision - 5-8 Channels (Float64Format)
// ============================================================================

static void compute_fixed_multichannel_float(const DotProdConfig<Float64Format>& config,
                                             Float64Format::accumulator_t* output) {
    const int num_channels = config.num_channels;

    // Use 4 sets of vector accumulators for instruction-level parallelism
    // Each set holds up to 8 channels (4 x float64x2_t = 8 doubles)
    float64x2_t acc0[4] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0)};
    float64x2_t acc1[4] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0)};
    float64x2_t acc2[4] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0)};
    float64x2_t acc3[4] = {vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0), vdupq_n_f64(0.0)};

    int i = 0;

    // Process 4 taps at a time
    for (; i + 3 < config.length; i += 4) {
        const float64x2_t c0 = vdupq_n_f64(config.coeffs[i]);
        const float64x2_t c1 = vdupq_n_f64(config.coeffs[i + 1]);
        const float64x2_t c2 = vdupq_n_f64(config.coeffs[i + 2]);
        const float64x2_t c3 = vdupq_n_f64(config.coeffs[i + 3]);

        const double* s_ptr0 = &config.samples[static_cast<size_t>(i) * num_channels];
        const double* s_ptr1 = &config.samples[static_cast<size_t>(i + 1) * num_channels];
        const double* s_ptr2 = &config.samples[static_cast<size_t>(i + 2) * num_channels];
        const double* s_ptr3 = &config.samples[static_cast<size_t>(i + 3) * num_channels];

        // Process channels in pairs
        for (int ch = 0; ch + 1 < num_channels; ch += 2) {
            const int vec_idx = ch / 2;

            float64x2_t s0 = vld1q_f64(&s_ptr0[ch]);
            float64x2_t s1 = vld1q_f64(&s_ptr1[ch]);
            float64x2_t s2 = vld1q_f64(&s_ptr2[ch]);
            float64x2_t s3 = vld1q_f64(&s_ptr3[ch]);

            acc0[vec_idx] = vfmaq_f64(acc0[vec_idx], s0, c0);
            acc1[vec_idx] = vfmaq_f64(acc1[vec_idx], s1, c1);
            acc2[vec_idx] = vfmaq_f64(acc2[vec_idx], s2, c2);
            acc3[vec_idx] = vfmaq_f64(acc3[vec_idx], s3, c3);
        }
    }

    // Combine the 4 accumulator sets
    for (int v = 0; v < 4; v++) {
        acc0[v] = vaddq_f64(acc0[v], acc1[v]);
        acc2[v] = vaddq_f64(acc2[v], acc3[v]);
        acc0[v] = vaddq_f64(acc0[v], acc2[v]);
    }

    // Extract results to scalar array
    double result[8];
    for (int v = 0; v < 4; v++) {
        result[static_cast<size_t>(v) * 2] = vgetq_lane_f64(acc0[v], 0);
        result[v * 2 + 1] = vgetq_lane_f64(acc0[v], 1);
    }

    // Handle remainder taps and odd channel
    for (; i < config.length; i++) {
        const double coeff = config.coeffs[i];
        const double* s_ptr = &config.samples[static_cast<size_t>(i) * num_channels];

        for (int ch = 0; ch < num_channels; ch++) {
            result[ch] += s_ptr[ch] * coeff;
        }
    }

    for (int ch = 0; ch < num_channels; ch++) {
        output[ch] = result[ch];
    }
}

// ============================================================================
// Fixed Precision - Dispatch (Float64Format)
// ============================================================================

void compute_fixed_float(const DotProdConfig<Float64Format>& config,
                         Float64Format::accumulator_t* output) {
    switch (config.num_channels) {
        case 1:
            compute_fixed_ch1_float(config, output);
            break;
        case 2:
            compute_fixed_ch2_float(config, output);
            break;
        case 3:
            compute_fixed_3ch_float(config, output);
            break;
        case 4:
            compute_fixed_4ch_float(config, output);
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            compute_fixed_multichannel_float(config, output);
            break;
        default:
            break;  // >8 channels handled by C++ fallback
    }
}

}  // namespace neon
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // __ARM_NEON || __ARM_NEON__
