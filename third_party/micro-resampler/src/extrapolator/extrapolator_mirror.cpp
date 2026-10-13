// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt
//
// Mirror extrapolation: reflects samples around the boundary point.
// This is the simplest and fastest extrapolation method.

#include "extrapolator/extrapolator.h"
#include "formats.h"

#include <algorithm>

// ============================================================================
// MirrorExtrapolator Implementation
// ============================================================================
// Reflection maps every output sample straight to a source index, so both methods
// work directly on the interleaved buffers with no per-channel scratch.

namespace micro_resampler {

template <typename CoeffFormat>
void MirrorExtrapolator<CoeffFormat>::prime_buffer(sample_t* history_buffer, int num_channels,
                                                   const sample_t* initial_samples,
                                                   int available_frames, int filter_length) {
    int frames_to_use = std::min(available_frames, filter_length);

    // Handle empty (or invalid negative-length) input
    if (frames_to_use <= 0) {
        return;
    }

    // Fill the entire pre-history region [0, filter_length) so the filter window has no
    // zero holes when the input is shorter than the filter. Reflecting around the start
    // of the input: the frame just before input frame 0 is input frame 0, the one before
    // that is input frame 1, and so on, wrapping every frames_to_use frames.
    for (int j = 0; j < filter_length; ++j) {
        int src_frame = (filter_length - 1 - j) % frames_to_use;
        for (int ch = 0; ch < num_channels; ++ch) {
            history_buffer[j * num_channels + ch] = initial_samples[src_frame * num_channels + ch];
        }
    }
}

template <typename CoeffFormat>
void MirrorExtrapolator<CoeffFormat>::prepare_flush(sample_t* history_buffer, int num_channels,
                                                    int filter_length) {
    int extrapolate_frames = filter_length / 2 + 1;

    // Reflect only the LAST filter_length/2 frames around the end of the history
    int samples_for_extrapolation = filter_length / 2;

    if (samples_for_extrapolation <= 0) {
        return;
    }

    // Write [filter_length, filter_length + extrapolate_frames) from the reflection source
    // [filter_length - samples_for_extrapolation, filter_length); the ranges never overlap
    for (int j = 0; j < extrapolate_frames; ++j) {
        int src_frame = filter_length - 1 - (j % samples_for_extrapolation);
        int dst_frame = filter_length + j;
        for (int ch = 0; ch < num_channels; ++ch) {
            history_buffer[dst_frame * num_channels + ch] =
                history_buffer[src_frame * num_channels + ch];
        }
    }
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================

template class Extrapolator<Q15Format>;
template class Extrapolator<Q31Format>;
template class Extrapolator<Q31LiteFormat>;
template class Extrapolator<Float64Format>;
template class Extrapolator<Float32Format>;

template class MirrorExtrapolator<Q15Format>;
template class MirrorExtrapolator<Q31Format>;
template class MirrorExtrapolator<Q31LiteFormat>;
template class MirrorExtrapolator<Float64Format>;
template class MirrorExtrapolator<Float32Format>;

}  // namespace micro_resampler
