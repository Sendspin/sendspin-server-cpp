// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#if defined(__ARM_NEON) || defined(__ARM_NEON__)

#include "../../detail/fixed_point.h"
#include "../dotprod.h"  // Full definitions (includes neon.h via dotprod_dispatch.h)
#include "formats.h"
#include <arm_neon.h>

#include <cstdint>

namespace micro_resampler {
namespace dotprod {
namespace neon {

// ============================================================================
// Block Adaptive Q - 1 Channel
// ============================================================================

void compute_block_shift_ch1(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output) {
    *output = 0;
    const int num_blocks = config.length / 8;

    int block = 0;

    // Process 4 blocks at a time for better instruction-level parallelism
    for (; block + 3 < num_blocks; block += 4) {
        // Block 0: Load 8 samples/coeffs as two int16x4_t each
        int16x4_t s0_0 = vld1_s16(&config.samples[static_cast<size_t>(block + 0) * 8]);
        int16x4_t s0_1 = vld1_s16(&config.samples[(block + 0) * 8 + 4]);
        int16x4_t c0_0 = vld1_s16(&config.coeffs[static_cast<size_t>(block + 0) * 8]);
        int16x4_t c0_1 = vld1_s16(&config.coeffs[(block + 0) * 8 + 4]);

        // Block 1
        int16x4_t s1_0 = vld1_s16(&config.samples[static_cast<size_t>(block + 1) * 8]);
        int16x4_t s1_1 = vld1_s16(&config.samples[(block + 1) * 8 + 4]);
        int16x4_t c1_0 = vld1_s16(&config.coeffs[static_cast<size_t>(block + 1) * 8]);
        int16x4_t c1_1 = vld1_s16(&config.coeffs[(block + 1) * 8 + 4]);

        // Block 2
        int16x4_t s2_0 = vld1_s16(&config.samples[static_cast<size_t>(block + 2) * 8]);
        int16x4_t s2_1 = vld1_s16(&config.samples[(block + 2) * 8 + 4]);
        int16x4_t c2_0 = vld1_s16(&config.coeffs[static_cast<size_t>(block + 2) * 8]);
        int16x4_t c2_1 = vld1_s16(&config.coeffs[(block + 2) * 8 + 4]);

        // Block 3
        int16x4_t s3_0 = vld1_s16(&config.samples[static_cast<size_t>(block + 3) * 8]);
        int16x4_t s3_1 = vld1_s16(&config.samples[(block + 3) * 8 + 4]);
        int16x4_t c3_0 = vld1_s16(&config.coeffs[static_cast<size_t>(block + 3) * 8]);
        int16x4_t c3_1 = vld1_s16(&config.coeffs[(block + 3) * 8 + 4]);

        // Multiply all blocks (separate accumulators allow parallelism)
        int32x4_t a0_0 = vmull_s16(s0_0, c0_0);
        int32x4_t a0_1 = vmull_s16(s0_1, c0_1);
        int32x4_t a1_0 = vmull_s16(s1_0, c1_0);
        int32x4_t a1_1 = vmull_s16(s1_1, c1_1);
        int32x4_t a2_0 = vmull_s16(s2_0, c2_0);
        int32x4_t a2_1 = vmull_s16(s2_1, c2_1);
        int32x4_t a3_0 = vmull_s16(s3_0, c3_0);
        int32x4_t a3_1 = vmull_s16(s3_1, c3_1);

        // Combine halves and reduce
        int64_t sum0 = vaddlvq_s32(vaddq_s32(a0_0, a0_1));
        int64_t sum1 = vaddlvq_s32(vaddq_s32(a1_0, a1_1));
        int64_t sum2 = vaddlvq_s32(vaddq_s32(a2_0, a2_1));
        int64_t sum3 = vaddlvq_s32(vaddq_s32(a3_0, a3_1));

        // Apply per-block shifts and accumulate
        *output += static_cast<int32_t>(sum0 >> config.block_shifts[block + 0]);
        *output += static_cast<int32_t>(sum1 >> config.block_shifts[block + 1]);
        *output += static_cast<int32_t>(sum2 >> config.block_shifts[block + 2]);
        *output += static_cast<int32_t>(sum3 >> config.block_shifts[block + 3]);
    }

    // Handle remaining blocks (0-3)
    for (; block < num_blocks; block++) {
        int16x4_t s0 = vld1_s16(&config.samples[static_cast<size_t>(block) * 8]);
        int16x4_t s1 = vld1_s16(&config.samples[block * 8 + 4]);
        int16x4_t c0 = vld1_s16(&config.coeffs[static_cast<size_t>(block) * 8]);
        int16x4_t c1 = vld1_s16(&config.coeffs[block * 8 + 4]);

        int32x4_t acc0 = vmull_s16(s0, c0);
        int32x4_t acc1 = vmull_s16(s1, c1);

        int64_t block_sum = vaddlvq_s32(vaddq_s32(acc0, acc1));
        *output += static_cast<int32_t>(block_sum >> config.block_shifts[block]);
    }
}

// ============================================================================
// Block Adaptive Q - 2 Channels
// ============================================================================

void compute_block_shift_ch2(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;
    const int num_blocks = config.length / 8;

    for (int block = 0; block < num_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 16];  // 8 taps x 2 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 8];

        // Load all 8 coefficients (two vectors of 4)
        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        // Load and deinterleave all 8 stereo pairs
        int16x4x2_t stereo0 = vld2_s16(&s_ptr[0]);
        int16x4x2_t stereo1 = vld2_s16(&s_ptr[8]);

        // Multiply first 4 pairs
        int32x4_t acc_l_vec = vmull_s16(stereo0.val[0], c0);
        int32x4_t acc_r_vec = vmull_s16(stereo0.val[1], c0);

        // Fused multiply-accumulate for second 4 pairs
        acc_l_vec = vmlal_s16(acc_l_vec, stereo1.val[0], c1);
        acc_r_vec = vmlal_s16(acc_r_vec, stereo1.val[1], c1);

        // Horizontal reduction to int64 for the shift operation
        int64_t block_sum_l = vaddlvq_s32(acc_l_vec);
        int64_t block_sum_r = vaddlvq_s32(acc_r_vec);

        // Apply per-block shift and accumulate directly to output
        // Safe due to max tap extraction removing largest coefficients
        output[0] += static_cast<int32_t>(block_sum_l >> config.block_shifts[block]);
        output[1] += static_cast<int32_t>(block_sum_r >> config.block_shifts[block]);
    }
}

// ============================================================================
// Block Adaptive Q - 2 Channels (Wide SIMD - 16 taps per iteration)
// ============================================================================
// Processes 16 coefficient taps per iteration for improved throughput.
// Requires filter_length >= 32 for optimal use (dispatch threshold).

void compute_block_shift_ch2_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;

    // Process 16-tap blocks
    const int num_wide_blocks = config.length / 16;

    for (int block = 0; block < num_wide_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 32];  // 16 taps x 2 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 16];

        // Note: In block-shift mode, we have two consecutive 8-tap blocks
        // with potentially different shifts. We need to process them separately.
        const int block_idx_0 = block * 2;
        const int block_idx_1 = block * 2 + 1;

        // First 8-tap sub-block
        {
            int16x4_t c0 = vld1_s16(&c_ptr[0]);
            int16x4_t c1 = vld1_s16(&c_ptr[4]);

            int16x4x2_t stereo0 = vld2_s16(&s_ptr[0]);
            int16x4x2_t stereo1 = vld2_s16(&s_ptr[8]);

            int32x4_t acc_l_vec = vmull_s16(stereo0.val[0], c0);
            int32x4_t acc_r_vec = vmull_s16(stereo0.val[1], c0);
            acc_l_vec = vmlal_s16(acc_l_vec, stereo1.val[0], c1);
            acc_r_vec = vmlal_s16(acc_r_vec, stereo1.val[1], c1);

            int64_t block_sum_l = vaddlvq_s32(acc_l_vec);
            int64_t block_sum_r = vaddlvq_s32(acc_r_vec);

            output[0] += static_cast<int32_t>(block_sum_l >> config.block_shifts[block_idx_0]);
            output[1] += static_cast<int32_t>(block_sum_r >> config.block_shifts[block_idx_0]);
        }

        // Second 8-tap sub-block
        {
            int16x4_t c2 = vld1_s16(&c_ptr[8]);
            int16x4_t c3 = vld1_s16(&c_ptr[12]);

            int16x4x2_t stereo2 = vld2_s16(&s_ptr[16]);
            int16x4x2_t stereo3 = vld2_s16(&s_ptr[24]);

            int32x4_t acc_l_vec = vmull_s16(stereo2.val[0], c2);
            int32x4_t acc_r_vec = vmull_s16(stereo2.val[1], c2);
            acc_l_vec = vmlal_s16(acc_l_vec, stereo3.val[0], c3);
            acc_r_vec = vmlal_s16(acc_r_vec, stereo3.val[1], c3);

            int64_t block_sum_l = vaddlvq_s32(acc_l_vec);
            int64_t block_sum_r = vaddlvq_s32(acc_r_vec);

            output[0] += static_cast<int32_t>(block_sum_l >> config.block_shifts[block_idx_1]);
            output[1] += static_cast<int32_t>(block_sum_r >> config.block_shifts[block_idx_1]);
        }
    }

    // Handle remaining 8-tap block (if any)
    const int remaining_8tap_blocks = (config.length % 16) / 8;
    if (remaining_8tap_blocks > 0) {
        const int block_idx = num_wide_blocks * 2;
        const int16_t* s_ptr = &config.samples[static_cast<size_t>(num_wide_blocks) * 32];
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(num_wide_blocks) * 16];

        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        int16x4x2_t stereo0 = vld2_s16(&s_ptr[0]);
        int16x4x2_t stereo1 = vld2_s16(&s_ptr[8]);

        int32x4_t acc_l_vec = vmull_s16(stereo0.val[0], c0);
        int32x4_t acc_r_vec = vmull_s16(stereo0.val[1], c0);
        acc_l_vec = vmlal_s16(acc_l_vec, stereo1.val[0], c1);
        acc_r_vec = vmlal_s16(acc_r_vec, stereo1.val[1], c1);

        int64_t block_sum_l = vaddlvq_s32(acc_l_vec);
        int64_t block_sum_r = vaddlvq_s32(acc_r_vec);

        output[0] += static_cast<int32_t>(block_sum_l >> config.block_shifts[block_idx]);
        output[1] += static_cast<int32_t>(block_sum_r >> config.block_shifts[block_idx]);
    }
}

// ============================================================================
// Block Adaptive Q - 3 Channels (e.g., LCR)
// ============================================================================
// Uses vld3_s16 to deinterleave 3-channel audio while loading.

void compute_block_shift_3ch(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;
    const int num_blocks = config.length / 8;

    for (int block = 0; block < num_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 24];  // 8 taps x 3 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 8];

        // Load all 8 coefficients (two vectors of 4)
        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        // Load and deinterleave all 8 frames of 3-channel audio
        int16x4x3_t samples0 = vld3_s16(&s_ptr[0]);
        int16x4x3_t samples1 = vld3_s16(&s_ptr[12]);

        // Multiply first 4 frames
        int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
        int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
        int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);

        // Fused multiply-accumulate for second 4 frames
        acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
        acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
        acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);

        // Horizontal reduction to int64 for the shift operation
        int64_t block_sum_0 = vaddlvq_s32(acc_0);
        int64_t block_sum_1 = vaddlvq_s32(acc_1);
        int64_t block_sum_2 = vaddlvq_s32(acc_2);

        // Apply per-block shift and accumulate directly to output
        output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block]);
        output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block]);
        output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block]);
    }
}

// ============================================================================
// Block Adaptive Q - 3 Channels (Wide SIMD - 16 taps per iteration)
// ============================================================================
// Processes 16 coefficient taps per iteration for improved throughput.
// Requires filter_length >= 32 for optimal use (dispatch threshold).

void compute_block_shift_3ch_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;

    // Process 16-tap blocks (two 8-tap sub-blocks)
    const int num_wide_blocks = config.length / 16;

    for (int block = 0; block < num_wide_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 48];  // 16 taps x 3 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 16];

        // Two consecutive 8-tap blocks with potentially different shifts
        const int block_idx_0 = block * 2;
        const int block_idx_1 = block * 2 + 1;

        // First 8-tap sub-block
        {
            int16x4_t c0 = vld1_s16(&c_ptr[0]);
            int16x4_t c1 = vld1_s16(&c_ptr[4]);

            int16x4x3_t samples0 = vld3_s16(&s_ptr[0]);
            int16x4x3_t samples1 = vld3_s16(&s_ptr[12]);

            int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
            int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
            int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);
            acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
            acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
            acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);

            int64_t block_sum_0 = vaddlvq_s32(acc_0);
            int64_t block_sum_1 = vaddlvq_s32(acc_1);
            int64_t block_sum_2 = vaddlvq_s32(acc_2);

            output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx_0]);
            output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx_0]);
            output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx_0]);
        }

        // Second 8-tap sub-block
        {
            int16x4_t c2 = vld1_s16(&c_ptr[8]);
            int16x4_t c3 = vld1_s16(&c_ptr[12]);

            int16x4x3_t samples2 = vld3_s16(&s_ptr[24]);
            int16x4x3_t samples3 = vld3_s16(&s_ptr[36]);

            int32x4_t acc_0 = vmull_s16(samples2.val[0], c2);
            int32x4_t acc_1 = vmull_s16(samples2.val[1], c2);
            int32x4_t acc_2 = vmull_s16(samples2.val[2], c2);
            acc_0 = vmlal_s16(acc_0, samples3.val[0], c3);
            acc_1 = vmlal_s16(acc_1, samples3.val[1], c3);
            acc_2 = vmlal_s16(acc_2, samples3.val[2], c3);

            int64_t block_sum_0 = vaddlvq_s32(acc_0);
            int64_t block_sum_1 = vaddlvq_s32(acc_1);
            int64_t block_sum_2 = vaddlvq_s32(acc_2);

            output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx_1]);
            output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx_1]);
            output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx_1]);
        }
    }

    // Handle remaining 8-tap block (if any)
    const int remaining_8tap_blocks = (config.length % 16) / 8;
    if (remaining_8tap_blocks > 0) {
        const int block_idx = num_wide_blocks * 2;
        const int16_t* s_ptr = &config.samples[static_cast<size_t>(num_wide_blocks) * 48];
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(num_wide_blocks) * 16];

        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        int16x4x3_t samples0 = vld3_s16(&s_ptr[0]);
        int16x4x3_t samples1 = vld3_s16(&s_ptr[12]);

        int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
        int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
        int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);
        acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
        acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
        acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);

        int64_t block_sum_0 = vaddlvq_s32(acc_0);
        int64_t block_sum_1 = vaddlvq_s32(acc_1);
        int64_t block_sum_2 = vaddlvq_s32(acc_2);

        output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx]);
        output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx]);
        output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx]);
    }
}

// ============================================================================
// Block Adaptive Q - 4 Channels (e.g., Quadraphonic, LRCS)
// ============================================================================
// Uses vld4_s16 to deinterleave 4-channel audio while loading.

void compute_block_shift_4ch(const DotProdConfig<Q15Format>& config,
                             Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;
    output[3] = 0;
    const int num_blocks = config.length / 8;

    for (int block = 0; block < num_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 32];  // 8 taps x 4 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 8];

        // Load all 8 coefficients (two vectors of 4)
        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        // Load and deinterleave all 8 frames of 4-channel audio
        int16x4x4_t samples0 = vld4_s16(&s_ptr[0]);
        int16x4x4_t samples1 = vld4_s16(&s_ptr[16]);

        // Multiply first 4 frames
        int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
        int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
        int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);
        int32x4_t acc_3 = vmull_s16(samples0.val[3], c0);

        // Fused multiply-accumulate for second 4 frames
        acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
        acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
        acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);
        acc_3 = vmlal_s16(acc_3, samples1.val[3], c1);

        // Horizontal reduction to int64 for the shift operation
        int64_t block_sum_0 = vaddlvq_s32(acc_0);
        int64_t block_sum_1 = vaddlvq_s32(acc_1);
        int64_t block_sum_2 = vaddlvq_s32(acc_2);
        int64_t block_sum_3 = vaddlvq_s32(acc_3);

        // Apply per-block shift and accumulate directly to output
        output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block]);
        output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block]);
        output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block]);
        output[3] += static_cast<int32_t>(block_sum_3 >> config.block_shifts[block]);
    }
}

// ============================================================================
// Block Adaptive Q - 4 Channels (Wide SIMD - 16 taps per iteration)
// ============================================================================
// Processes 16 coefficient taps per iteration for improved throughput.
// Requires filter_length >= 32 for optimal use (dispatch threshold).

void compute_block_shift_4ch_wide(const DotProdConfig<Q15Format>& config,
                                  Q15Format::accumulator_t* output) {
    output[0] = 0;
    output[1] = 0;
    output[2] = 0;
    output[3] = 0;

    // Process 16-tap blocks (two 8-tap sub-blocks)
    const int num_wide_blocks = config.length / 16;

    for (int block = 0; block < num_wide_blocks; block++) {
        const int16_t* s_ptr =
            &config.samples[static_cast<size_t>(block) * 64];  // 16 taps x 4 channels
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 16];

        // Two consecutive 8-tap blocks with potentially different shifts
        const int block_idx_0 = block * 2;
        const int block_idx_1 = block * 2 + 1;

        // First 8-tap sub-block
        {
            int16x4_t c0 = vld1_s16(&c_ptr[0]);
            int16x4_t c1 = vld1_s16(&c_ptr[4]);

            int16x4x4_t samples0 = vld4_s16(&s_ptr[0]);
            int16x4x4_t samples1 = vld4_s16(&s_ptr[16]);

            int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
            int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
            int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);
            int32x4_t acc_3 = vmull_s16(samples0.val[3], c0);
            acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
            acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
            acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);
            acc_3 = vmlal_s16(acc_3, samples1.val[3], c1);

            int64_t block_sum_0 = vaddlvq_s32(acc_0);
            int64_t block_sum_1 = vaddlvq_s32(acc_1);
            int64_t block_sum_2 = vaddlvq_s32(acc_2);
            int64_t block_sum_3 = vaddlvq_s32(acc_3);

            output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx_0]);
            output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx_0]);
            output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx_0]);
            output[3] += static_cast<int32_t>(block_sum_3 >> config.block_shifts[block_idx_0]);
        }

        // Second 8-tap sub-block
        {
            int16x4_t c2 = vld1_s16(&c_ptr[8]);
            int16x4_t c3 = vld1_s16(&c_ptr[12]);

            int16x4x4_t samples2 = vld4_s16(&s_ptr[32]);
            int16x4x4_t samples3 = vld4_s16(&s_ptr[48]);

            int32x4_t acc_0 = vmull_s16(samples2.val[0], c2);
            int32x4_t acc_1 = vmull_s16(samples2.val[1], c2);
            int32x4_t acc_2 = vmull_s16(samples2.val[2], c2);
            int32x4_t acc_3 = vmull_s16(samples2.val[3], c2);
            acc_0 = vmlal_s16(acc_0, samples3.val[0], c3);
            acc_1 = vmlal_s16(acc_1, samples3.val[1], c3);
            acc_2 = vmlal_s16(acc_2, samples3.val[2], c3);
            acc_3 = vmlal_s16(acc_3, samples3.val[3], c3);

            int64_t block_sum_0 = vaddlvq_s32(acc_0);
            int64_t block_sum_1 = vaddlvq_s32(acc_1);
            int64_t block_sum_2 = vaddlvq_s32(acc_2);
            int64_t block_sum_3 = vaddlvq_s32(acc_3);

            output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx_1]);
            output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx_1]);
            output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx_1]);
            output[3] += static_cast<int32_t>(block_sum_3 >> config.block_shifts[block_idx_1]);
        }
    }

    // Handle remaining 8-tap block (if any)
    const int remaining_8tap_blocks = (config.length % 16) / 8;
    if (remaining_8tap_blocks > 0) {
        const int block_idx = num_wide_blocks * 2;
        const int16_t* s_ptr = &config.samples[static_cast<size_t>(num_wide_blocks) * 64];
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(num_wide_blocks) * 16];

        int16x4_t c0 = vld1_s16(&c_ptr[0]);
        int16x4_t c1 = vld1_s16(&c_ptr[4]);

        int16x4x4_t samples0 = vld4_s16(&s_ptr[0]);
        int16x4x4_t samples1 = vld4_s16(&s_ptr[16]);

        int32x4_t acc_0 = vmull_s16(samples0.val[0], c0);
        int32x4_t acc_1 = vmull_s16(samples0.val[1], c0);
        int32x4_t acc_2 = vmull_s16(samples0.val[2], c0);
        int32x4_t acc_3 = vmull_s16(samples0.val[3], c0);
        acc_0 = vmlal_s16(acc_0, samples1.val[0], c1);
        acc_1 = vmlal_s16(acc_1, samples1.val[1], c1);
        acc_2 = vmlal_s16(acc_2, samples1.val[2], c1);
        acc_3 = vmlal_s16(acc_3, samples1.val[3], c1);

        int64_t block_sum_0 = vaddlvq_s32(acc_0);
        int64_t block_sum_1 = vaddlvq_s32(acc_1);
        int64_t block_sum_2 = vaddlvq_s32(acc_2);
        int64_t block_sum_3 = vaddlvq_s32(acc_3);

        output[0] += static_cast<int32_t>(block_sum_0 >> config.block_shifts[block_idx]);
        output[1] += static_cast<int32_t>(block_sum_1 >> config.block_shifts[block_idx]);
        output[2] += static_cast<int32_t>(block_sum_2 >> config.block_shifts[block_idx]);
        output[3] += static_cast<int32_t>(block_sum_3 >> config.block_shifts[block_idx]);
    }
}

// ============================================================================
// Block Adaptive Q - 5-8 Channels (e.g., 5.1, 7.1 surround)
// ============================================================================
// Generalized version for 5-8 channels. Processes one tap at a time,
// using NEON to parallelize across channels.

void compute_block_shift_multichannel(const DotProdConfig<Q15Format>& config,
                                      Q15Format::accumulator_t* output) {
    const int num_channels = config.num_channels;
    const int num_blocks = config.length / 8;

    // Initialize output buffer
    for (int ch = 0; ch < num_channels; ch++) {
        output[ch] = 0;
    }

    for (int block = 0; block < num_blocks; block++) {
        const q15_t* c_ptr = &config.coeffs[static_cast<size_t>(block) * 8];
        const int16_t* s_base = &config.samples[static_cast<size_t>(block) * 8 * num_channels];

        int64_t block_sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};

        // Process 8 taps in this block
        for (int j = 0; j < 8; j++) {
            const int16_t coeff = c_ptr[j];
            const int16_t* s_ptr = &s_base[static_cast<size_t>(j) * num_channels];

            // Broadcast coefficient to vector
            const int16x4_t c_vec = vdup_n_s16(coeff);

            // Process channels in groups of 4 using NEON
            int ch = 0;
            for (; ch + 3 < num_channels; ch += 4) {
                // Load 4 contiguous channel samples for this tap
                int16x4_t s = vld1_s16(&s_ptr[ch]);

                // Multiply: 4 x (int16 * int16) -> 4 x int32
                int32x4_t prod = vmull_s16(s, c_vec);

                // Extract and accumulate to per-channel sums
                block_sum[ch + 0] += vgetq_lane_s32(prod, 0);
                block_sum[ch + 1] += vgetq_lane_s32(prod, 1);
                block_sum[ch + 2] += vgetq_lane_s32(prod, 2);
                block_sum[ch + 3] += vgetq_lane_s32(prod, 3);
            }

            // Handle remaining channels (1-3) with scalar operations
            for (; ch < num_channels; ch++) {
                block_sum[ch] += static_cast<int64_t>(s_ptr[ch]) * static_cast<int32_t>(coeff);
            }
        }

        // Apply per-block shift and accumulate directly to output
        // Safe due to max tap extraction removing largest coefficients
        for (int ch = 0; ch < num_channels; ch++) {
            output[ch] += static_cast<int32_t>(block_sum[ch] >> config.block_shifts[block]);
        }
    }
}

// ============================================================================
// Fixed Precision - 1 Channel (Q15 only)
// ============================================================================
// Currently unused in the resampler, but available for future use. Declared in
// neon.h and mirrored by the Xtensa backends (simd_s3.h, mac16.h) as a
// deliberately-reserved cross-backend reference API, not dead code.
// cppcheck-suppress unusedFunction

int32_t compute_simple(const int16_t* samples, const q15_t* coeffs, int length) {
    int32x4_t acc0 = vdupq_n_s32(0);
    int32x4_t acc1 = vdupq_n_s32(0);

    int i = 0;

    // Process 8 samples at a time
    for (; i + 7 < length; i += 8) {
        int16x8_t s = vld1q_s16(&samples[i]);
        int16x8_t c = vld1q_s16(&coeffs[i]);

        acc0 = vmlal_s16(acc0, vget_low_s16(s), vget_low_s16(c));
        acc1 = vmlal_high_s16(acc1, s, c);
    }

    // Combine accumulators
    int32x4_t acc = vaddq_s32(acc0, acc1);
    int32_t result = vaddvq_s32(acc);

    // Handle remaining samples
    for (; i < length; i++) {
        result += static_cast<int32_t>(samples[i]) * static_cast<int32_t>(coeffs[i]);
    }

    return result;
}

}  // namespace neon
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // __ARM_NEON || __ARM_NEON__
