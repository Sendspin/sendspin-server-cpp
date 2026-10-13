// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt
//
// Zeros extrapolation: fills boundary regions with zeros.
// This is the simplest and fastest extrapolation method.

#include "extrapolator/extrapolator.h"
#include "formats.h"

#include <cstring>

namespace micro_resampler {

// ============================================================================
// ZerosExtrapolator Implementation
// ============================================================================

template <typename CoeffFormat>
void ZerosExtrapolator<CoeffFormat>::prime_buffer(sample_t* history_buffer, int num_channels,
                                                  const sample_t* initial_samples,
                                                  int available_frames, int filter_length) {
    (void)initial_samples;
    (void)available_frames;

    // Fill the entire history buffer with zeros
    std::memset(history_buffer, 0,
                static_cast<size_t>(filter_length) * num_channels * sizeof(sample_t));
}

template <typename CoeffFormat>
void ZerosExtrapolator<CoeffFormat>::prepare_flush(sample_t* history_buffer, int num_channels,
                                                   int filter_length) {
    int extrapolate_frames = filter_length / 2 + 1;

    // Fill the second half of the history buffer with zeros
    sample_t* dst = history_buffer + filter_length * num_channels;
    std::memset(dst, 0, static_cast<size_t>(extrapolate_frames) * num_channels * sizeof(sample_t));
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================

template class ZerosExtrapolator<Q15Format>;
template class ZerosExtrapolator<Q31Format>;
template class ZerosExtrapolator<Q31LiteFormat>;
template class ZerosExtrapolator<Float64Format>;
template class ZerosExtrapolator<Float32Format>;

}  // namespace micro_resampler
