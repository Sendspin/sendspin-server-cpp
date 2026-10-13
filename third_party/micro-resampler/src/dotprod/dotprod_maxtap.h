// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file dotprod_maxtap.h
/// @brief Max tap extraction for improved precision with large filter coefficients
///
/// The largest coefficients in each phase are pre-computed in Q31 precision and
/// applied separately, after the main dot product, to avoid truncation loss from
/// per-coefficient normalization

#ifndef DOTPROD_MAXTAP_H
#define DOTPROD_MAXTAP_H

#include "detail/coefficient_ops.h"
#include "detail/fixed_generic.h"
#include "dotprod.h"

#include <climits>
#include <type_traits>

namespace micro_resampler {
namespace dotprod {
namespace detail {

// ============================================================================
// Max Tap Extraction
// ============================================================================
// Applied AFTER the main dot product when max_tap_entries is enabled.
// Adds contribution from pre-extracted high-precision coefficients.
//
// Only Q15 uses max tap extraction - it enhances precision by computing
// the largest coefficients in Q31. Q31 already has full precision, and
// Float doesn't need fixed-point enhancement.

template <typename CoeffFormat>
void apply_max_taps(const DotProdConfig<CoeffFormat>& config,
                    typename CoeffFormat::accumulator_t* output) {
    // Only Q15 benefits from max tap extraction
    // Q31 already has full precision, Float doesn't need it
    if constexpr (!std::is_same_v<CoeffFormat, Q15Format>) {
        return;
    } else {
        using Ops = ::micro_resampler::detail::FormatOps<CoeffFormat>;

        const MaxTapEntry<CoeffFormat>* entries = config.max_tap_entries;
        const typename CoeffFormat::sample_t* samples = config.samples;
        const int count = config.max_taps_count;

        // Sum the extracted taps in 32 bits, then add that sum to the dot product with one
        // saturating add, so a total past +/-2.0 clips instead of wrapping to the opposite
        // sign. The tap sum itself cannot wrap: a Q31 coeff times a Q15 sample gives at most
        // |coeff| / 2 + 1 in Q30, and the 6 largest coefficients of one windowed-sinc phase
        // with a cutoff at or below Nyquist sum to at most 1.96 (FilterDesign caps the tap
        // count at MAX_TAPS_PER_PHASE and the cutoffs to [0, 1.0]). This matches accumulating
        // in 64 bits and saturating once, without the per-tap 64-bit adds. It assumes
        // output[ch], the main dot product's partial sum, did not wrap. The non-max taps' L1
        // norm stays below 2.0 for VERY_LOW/LOW, but on long downsampling filters (MEDIUM and
        // up) sign-aligned full-scale input across nearly every tap could still wrap it;
        // fixing that needs wider accumulators in the main kernels.
        //
        // Mono and stereo keep the tap sums in registers so each entry costs only the
        // entry/sample loads and the MAC itself, instead of a read-modify-write of output[]
        // per tap
        if (config.num_channels == 1) {
            int32_t tap_sum = 0;
            for (int extract_idx = 0; extract_idx < count; ++extract_idx) {
                tap_sum = ADD32(tap_sum, Ops::multiply_sample_processing(
                                             entries[extract_idx].coeff,
                                             samples[entries[extract_idx].index_reversed]));
            }
            output[0] = add_sat32_formats(output[0], tap_sum);
            return;
        }

        if (config.num_channels == 2) {
            int32_t tap_sum_left = 0;
            int32_t tap_sum_right = 0;
            for (int extract_idx = 0; extract_idx < count; ++extract_idx) {
                const int32_t max_coeff = entries[extract_idx].coeff;
                const int sample_base_idx = 2 * entries[extract_idx].index_reversed;
                tap_sum_left = ADD32(tap_sum_left, Ops::multiply_sample_processing(
                                                       max_coeff, samples[sample_base_idx]));
                tap_sum_right = ADD32(tap_sum_right, Ops::multiply_sample_processing(
                                                         max_coeff, samples[sample_base_idx + 1]));
            }
            output[0] = add_sat32_formats(output[0], tap_sum_left);
            output[1] = add_sat32_formats(output[1], tap_sum_right);
            return;
        }

        for (int ch = 0; ch < config.num_channels; ++ch) {
            int32_t tap_sum = 0;
            for (int extract_idx = 0; extract_idx < count; ++extract_idx) {
                const int sample_idx =
                    config.num_channels * entries[extract_idx].index_reversed + ch;
                tap_sum = ADD32(tap_sum, Ops::multiply_sample_processing(entries[extract_idx].coeff,
                                                                         samples[sample_idx]));
            }
            output[ch] = add_sat32_formats(output[ch], tap_sum);
        }
    }
}

}  // namespace detail
}  // namespace dotprod
}  // namespace micro_resampler

#endif  // DOTPROD_MAXTAP_H
