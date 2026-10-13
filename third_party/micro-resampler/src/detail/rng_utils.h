// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file rng_utils.h
/// @brief RNG state seeding utilities for TPDF dithering
///
/// Provides deterministic, non-zero per-channel seed derivation only; the
/// actual dither generation (Xorshift32 + TPDF) lives in
/// resampler/formats.h alongside the sample converters that use it.

#ifndef MICRO_RESAMPLER_RNG_UTILS_H
#define MICRO_RESAMPLER_RNG_UTILS_H

#include <cstdint>

namespace micro_resampler {

// Knuth's multiplicative hash constant: 2^32 * (golden ratio conjugate),
// rounded to the nearest odd number. Used to decorrelate sequential indices
// into well-spread pseudo-random seeds.
static constexpr uint32_t KNUTH_HASH_MULTIPLIER = 2654435761U;

// Arbitrary fixed offset applied to the input RNG stream only, so it never
// produces the same seed sequence as the output RNG stream for the same index.
static constexpr uint32_t INPUT_RNG_SEED_OFFSET = 0x12345678U;

/**
 * @brief Initialize RNG state with a deterministic seed
 *
 * Generates a non-zero seed from an index value. The RNG state is never
 * initialized to zero (which would break the xorshift32 generator
 * in resampler/formats.h).
 *
 * @param index Seed index (e.g., channel number, cascade index)
 * @return Initialized RNG state value
 */
static inline uint32_t initialize_rng_state(uint32_t index) {
    // Use a simple hash to generate different seeds for different indices
    // Never returns 0 (which would break xorshift32)
    uint32_t seed = (index * KNUTH_HASH_MULTIPLIER) + 1;
    return seed;
}

/**
 * @brief Initialize input RNG state with a deterministic seed (independent from output RNG)
 *
 * Uses a different offset than initialize_rng_state() to ensure input and output
 * dithering use completely independent RNG streams.
 *
 * @param index Seed index (e.g., channel number)
 * @return Initialized RNG state value
 */
static inline uint32_t initialize_input_rng_state(uint32_t index) {
    // Use different offset (0x12345678) to ensure independence from output RNG
    uint32_t seed = (index * KNUTH_HASH_MULTIPLIER) + INPUT_RNG_SEED_OFFSET;
    // Ensure we never return 0 (which would break xorshift32)
    return seed != 0 ? seed : 1;
}

}  // namespace micro_resampler

#endif  // MICRO_RESAMPLER_RNG_UTILS_H
