// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#include "resampler/filter_design.h"

namespace micro_resampler {

// Default polyphase bank size for the LOW quality preset. Chosen empirically
// (see CLAUDE.md quality table) rather than derived from the other constants.
constexpr int LOW_QUALITY_DEFAULT_PHASE_COUNT = 80;

FilterDesign FilterDesign::from_quality(ResamplerQuality quality) {
    FilterDesign design;

    switch (quality) {
        case ResamplerQuality::VERY_LOW:
            design.filter_length_ = 16;
            design.kaiser_beta_ = 6.0;
            design.downsampling_cutoff_percent_ = 0.85;  // 85% of output Nyquist
            design.upsampling_cutoff_percent_ = 1.0;     // 100% of input Nyquist
            design.default_phase_count_ = 1024;
            break;

        case ResamplerQuality::LOW:
            design.filter_length_ = 32;
            design.kaiser_beta_ = 6.0;
            design.downsampling_cutoff_percent_ = 0.882;  // 88.2% of output Nyquist
            design.upsampling_cutoff_percent_ = 0.9;      // 90% of input Nyquist
            design.default_phase_count_ = LOW_QUALITY_DEFAULT_PHASE_COUNT;
            break;

        case ResamplerQuality::MEDIUM:
            design.filter_length_ = 64;
            design.kaiser_beta_ = 8.0;
            design.downsampling_cutoff_percent_ = 0.921;  // 92.1% of output Nyquist
            design.upsampling_cutoff_percent_ = 1.0;      // 100% of input Nyquist
            design.default_phase_count_ = 1024;
            break;

        case ResamplerQuality::HIGH:
            design.filter_length_ = 128;
            design.kaiser_beta_ = 10.0;
            design.downsampling_cutoff_percent_ = 0.95;  // 95% of output Nyquist
            design.upsampling_cutoff_percent_ = 1.0;     // 100% of input Nyquist
            design.default_phase_count_ = 1024;
            break;

        case ResamplerQuality::VERY_HIGH:
            design.filter_length_ = 256;
            design.kaiser_beta_ = 12.0;
            design.downsampling_cutoff_percent_ = 0.95;  // 95% of output Nyquist
            design.upsampling_cutoff_percent_ = 1.0;     // 100% of input Nyquist
            design.default_phase_count_ = 1024;
            break;

        case ResamplerQuality::LUDICROUS:
            // Maximum quality - 2048 taps, 1024 phases
            // Warning: ~4MB filter bank (Q15), not suitable for memory-constrained embedded
            design.filter_length_ = 2048;
            design.kaiser_beta_ = 33.0;                 // High beta for excellent stopband
            design.downsampling_cutoff_percent_ = 1.0;  // 100% of output Nyquist
            design.upsampling_cutoff_percent_ = 1.0;    // 100% of input Nyquist
            design.default_phase_count_ = 1024;
            break;
    }

    return design;
}

}  // namespace micro_resampler
