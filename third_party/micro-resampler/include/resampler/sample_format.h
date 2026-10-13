// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file sample_format.h
/// @brief Runtime sample format enumeration for the type-erased resampler interface
///
/// Defines the SampleFormat enum (Q15, Q23, Q23_PACKED, Q31, FLOAT32, FLOAT64)
/// used with ResamplerConfig::set_input_format() / set_output_format(), plus helpers to query
/// bytes per sample, nominal bit depth, and whether a format uses packed storage.
///
/// Dithering is controlled orthogonally via ResamplerConfig::set_dither(); a request
/// only has effect when a source carrying detail below 16 bits is quantized to
/// Q15 somewhere in the pipeline (see MicroResampler::dither_is_effective()).

#ifndef SAMPLE_FORMAT_H
#define SAMPLE_FORMAT_H

#include <cstdint>

namespace micro_resampler {

enum class SampleFormat : uint8_t {
    Q15,         ///< 16-bit signed integer (Q15 fixed-point)
    Q23,         ///< 24-bit signed integer in 32-bit container (Q23 fixed-point)
    Q23_PACKED,  ///< 24-bit signed integer in packed 3-byte format (little-endian)
    Q31,         ///< 32-bit signed integer (Q31 fixed-point)
    FLOAT32,     ///< 32-bit IEEE 754 floating-point
    FLOAT64      ///< 64-bit IEEE 754 floating-point (double precision)
};

// ============================================================================
// Helper Functions
// ============================================================================

/// Returns the number of bytes per sample for a given format
inline constexpr int get_bytes_per_sample(SampleFormat format) {
    switch (format) {
        case SampleFormat::Q15:
            return 2;
        case SampleFormat::Q23_PACKED:
            return 3;
        case SampleFormat::Q23:
        case SampleFormat::Q31:
        case SampleFormat::FLOAT32:
            return 4;
        case SampleFormat::FLOAT64:
            return 8;
    }
    return 0;  // Should never reach here
}

/// Returns the nominal bit depth of the samples a format carries.
///
/// Used to decide whether a format holds detail below 16 bits and thus whether a
/// reduction to Q15 (16-bit) is worth dithering. Float formats report the width of
/// their mantissa, since that bounds the meaningful precision they can represent.
inline constexpr int format_precision_bits(SampleFormat format) {
    constexpr int K_Q15_BITS = 16;
    constexpr int K_Q23_BITS = 24;  // also the Float32 mantissa width
    constexpr int K_Q31_BITS = 32;
    constexpr int K_FLOAT64_MANTISSA_BITS = 53;
    switch (format) {
        case SampleFormat::Q15:
            return K_Q15_BITS;
        case SampleFormat::Q23:
        case SampleFormat::Q23_PACKED:
        case SampleFormat::FLOAT32:
            return K_Q23_BITS;
        case SampleFormat::Q31:
            return K_Q31_BITS;
        case SampleFormat::FLOAT64:
            return K_FLOAT64_MANTISSA_BITS;
    }
    return K_Q15_BITS;  // Should never reach here
}

/// Returns true if the format uses packed byte storage
inline constexpr bool format_is_packed(SampleFormat format) {
    return format == SampleFormat::Q23_PACKED;
}

}  // namespace micro_resampler

#endif  // SAMPLE_FORMAT_H
