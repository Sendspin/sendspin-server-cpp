// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include "../dotprod.h"  // Full definitions (includes neon.h via dotprod_dispatch.h)
#include "formats.h"
#include <arm_neon.h>

#include <cstdint>

namespace micro_resampler {
namespace dotprod {
namespace neon {

// ============================================================================
// Q31 Format - Fixed Precision Support
// ============================================================================
// Q31Format uses int32_t samples and coefficients with int64_t accumulators.
// Q31 does NOT use adaptive Q / block shifts - it uses fixed precision path.
// NEON can process 2-4 samples at a time using vmull_s32.

// ============================================================================
// Fixed Precision - 1 Channel (Q31)
// ============================================================================

static void compute_fixed_ch1_q31(const DotProdConfig<Q31Format>& config,
                                  Q31Format::accumulator_t* output) {
    int64x2_t acc = vdupq_n_s64(0);

    int i = 0;

    // Process 4 samples at a time
    for (; i + 3 < config.length; i += 4) {
        // Load 4 samples and 4 coefficients
        int32x4_t s = vld1q_s32(&config.samples[i]);
        int32x4_t c = vld1q_s32(&config.coeffs[i]);

        // Multiply: 4 x (int32 * int32) -> 4 x int64
        int64x2_t prod_lo = vmull_s32(vget_low_s32(s), vget_low_s32(c));
        int64x2_t prod_hi = vmull_high_s32(s, c);

        // Accumulate
        acc = vaddq_s64(acc, prod_lo);
        acc = vaddq_s64(acc, prod_hi);
    }

    // Horizontal sum
    int64_t result = vaddvq_s64(acc);

    // Handle remaining samples
    for (; i < config.length; i++) {
        result += static_cast<int64_t>(config.samples[i]) * static_cast<int64_t>(config.coeffs[i]);
    }

    *output = result;
}

// ============================================================================
// Fixed Precision - 2 Channels (Q31)
// ============================================================================

static void compute_fixed_ch2_q31(const DotProdConfig<Q31Format>& config,
                                  Q31Format::accumulator_t* output) {
    int64x2_t acc_l = vdupq_n_s64(0);
    int64x2_t acc_r = vdupq_n_s64(0);

    int i = 0;

    // Process 2 stereo pairs at a time
    for (; i + 1 < config.length; i += 2) {
        // Load 2 coefficients
        int32x2_t c = vld1_s32(&config.coeffs[i]);

        // Load and deinterleave 2 stereo pairs (4 samples total)
        int32x2x2_t stereo = vld2_s32(&config.samples[static_cast<size_t>(i) * 2]);

        // Multiply: 2 x (int32 * int32) -> 2 x int64
        int64x2_t prod_l = vmull_s32(stereo.val[0], c);
        int64x2_t prod_r = vmull_s32(stereo.val[1], c);

        // Accumulate
        acc_l = vaddq_s64(acc_l, prod_l);
        acc_r = vaddq_s64(acc_r, prod_r);
    }

    // Horizontal sums
    int64_t result_l = vaddvq_s64(acc_l);
    int64_t result_r = vaddvq_s64(acc_r);

    // Handle remaining sample
    for (; i < config.length; i++) {
        result_l += static_cast<int64_t>(config.samples[static_cast<size_t>(i) * 2]) *
                    static_cast<int64_t>(config.coeffs[i]);
        result_r += static_cast<int64_t>(config.samples[i * 2 + 1]) *
                    static_cast<int64_t>(config.coeffs[i]);
    }

    output[0] = result_l;
    output[1] = result_r;
}

// ============================================================================
// Fixed Precision - 3 Channels (Q31)
// ============================================================================

static void compute_fixed_3ch_q31(const DotProdConfig<Q31Format>& config,
                                  Q31Format::accumulator_t* output) {
    int64x2_t acc[3] = {vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0)};

    int i = 0;

    // Process 2 frames at a time
    for (; i + 1 < config.length; i += 2) {
        int32x2_t c = vld1_s32(&config.coeffs[i]);
        int32x2x3_t samples = vld3_s32(&config.samples[static_cast<size_t>(i) * 3]);

        acc[0] = vaddq_s64(acc[0], vmull_s32(samples.val[0], c));
        acc[1] = vaddq_s64(acc[1], vmull_s32(samples.val[1], c));
        acc[2] = vaddq_s64(acc[2], vmull_s32(samples.val[2], c));
    }

    int64_t result[3] = {vaddvq_s64(acc[0]), vaddvq_s64(acc[1]), vaddvq_s64(acc[2])};

    for (; i < config.length; i++) {
        result[0] += static_cast<int64_t>(config.samples[i * 3 + 0]) *
                     static_cast<int64_t>(config.coeffs[i]);
        result[1] += static_cast<int64_t>(config.samples[i * 3 + 1]) *
                     static_cast<int64_t>(config.coeffs[i]);
        result[2] += static_cast<int64_t>(config.samples[i * 3 + 2]) *
                     static_cast<int64_t>(config.coeffs[i]);
    }

    output[0] = result[0];
    output[1] = result[1];
    output[2] = result[2];
}

// ============================================================================
// Fixed Precision - 4 Channels (Q31)
// ============================================================================

static void compute_fixed_4ch_q31(const DotProdConfig<Q31Format>& config,
                                  Q31Format::accumulator_t* output) {
    int64x2_t acc[4] = {vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0), vdupq_n_s64(0)};

    int i = 0;

    for (; i + 1 < config.length; i += 2) {
        int32x2_t c = vld1_s32(&config.coeffs[i]);
        int32x2x4_t samples = vld4_s32(&config.samples[static_cast<size_t>(i) * 4]);

        acc[0] = vaddq_s64(acc[0], vmull_s32(samples.val[0], c));
        acc[1] = vaddq_s64(acc[1], vmull_s32(samples.val[1], c));
        acc[2] = vaddq_s64(acc[2], vmull_s32(samples.val[2], c));
        acc[3] = vaddq_s64(acc[3], vmull_s32(samples.val[3], c));
    }

    int64_t result[4];
    for (int ch = 0; ch < 4; ch++) {
        result[ch] = vaddvq_s64(acc[ch]);
    }

    for (; i < config.length; i++) {
        for (int ch = 0; ch < 4; ch++) {
            result[ch] += static_cast<int64_t>(config.samples[i * 4 + ch]) *
                          static_cast<int64_t>(config.coeffs[i]);
        }
    }

    for (int ch = 0; ch < 4; ch++) {
        output[ch] = result[ch];
    }
}

// ============================================================================
// Fixed Precision - 5-8 Channels (Q31)
// ============================================================================

static void compute_fixed_multichannel_q31(const DotProdConfig<Q31Format>& config,
                                           Q31Format::accumulator_t* output) {
    const int num_channels = config.num_channels;
    int64_t acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    for (int i = 0; i < config.length; i++) {
        const int32_t coeff = config.coeffs[i];
        const int32_t* s_ptr = &config.samples[static_cast<size_t>(i) * num_channels];
        const int32x2_t c_vec = vdup_n_s32(coeff);

        int ch = 0;
        for (; ch + 1 < num_channels; ch += 2) {
            int32x2_t s = vld1_s32(&s_ptr[ch]);
            int64x2_t prod = vmull_s32(s, c_vec);
            acc[ch + 0] += vgetq_lane_s64(prod, 0);
            acc[ch + 1] += vgetq_lane_s64(prod, 1);
        }

        for (; ch < num_channels; ch++) {
            acc[ch] += static_cast<int64_t>(s_ptr[ch]) * static_cast<int64_t>(coeff);
        }
    }

    for (int ch = 0; ch < num_channels; ch++) {
        output[ch] = acc[ch];
    }
}

// ============================================================================
// Fixed Precision - Dispatch (Q31)
// ============================================================================

void compute_fixed_q31(const DotProdConfig<Q31Format>& config, Q31Format::accumulator_t* output) {
    switch (config.num_channels) {
        case 1:
            compute_fixed_ch1_q31(config, output);
            break;
        case 2:
            compute_fixed_ch2_q31(config, output);
            break;
        case 3:
            compute_fixed_3ch_q31(config, output);
            break;
        case 4:
            compute_fixed_4ch_q31(config, output);
            break;
        case 5:
        case 6:
        case 7:
        case 8:
            compute_fixed_multichannel_q31(config, output);
            break;
        default:
            break;  // >8 channels handled by C++ fallback
    }
}

}  // namespace neon
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // __ARM_NEON || __ARM_NEON__
