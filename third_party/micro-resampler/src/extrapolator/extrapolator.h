// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file extrapolator.h
/// @brief Extrapolation framework for filling resampler history at stream boundaries
///
/// Declares a polymorphic Extrapolator interface plus three implementations
/// selectable through ExtrapolationMode:
/// - ZerosExtrapolator: zero padding (fastest, no extrapolation)
/// - MirrorExtrapolator: FFmpeg-style signal reflection (fast, default)
/// - BurgLPCExtrapolator: Burg's method for optimal reflection coefficients (stable filters)

#ifndef EXTRAPOLATOR_H
#define EXTRAPOLATOR_H

#include "resampler/resampler_config.h"  // For ExtrapolationMode

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace micro_resampler {

// ============================================================================
// Extrapolation Format Traits
// ============================================================================
// These traits define the types used for extrapolation arithmetic.
// Used internally by extrapolator implementations.

// Float extrapolation format (for float/double samples)
struct FloatExtrapolation {
    using sample_t = float;  // Input/output samples
    using coeff_t = double;  // LPC coefficients
    using accum_t = double;  // Accumulator for predictions

    static constexpr int MAX_LPC_ORDER = 8;
    static constexpr int DEFAULT_LPC_ORDER = 4;

    // Bandwidth expansion chirp factor
    static constexpr double CHIRP_FACTOR = 0.998;
};

// Fixed-point extrapolation format (for Q15 samples)
struct FixedExtrapolation {
    using sample_t = int16_t;  // Q15 samples
    using coeff_t = int32_t;   // Q28 LPC coefficients
    using accum_t = int64_t;   // Q43 prediction accumulator

    static constexpr int MAX_LPC_ORDER = 8;
    // Also caps the fixed-point Burg path (matches the CHIRP_POWERS length).
    // Orders above 2 are safe because burg_lpc_fixed() stops adding stages
    // once the residual reaches the Q15 quantization noise floor.
    static constexpr int DEFAULT_LPC_ORDER = 4;

    // Pre-computed chirp powers in Q15: 0.998^1, 0.998^2, 0.998^3, 0.998^4
    // Defined inline to avoid ODR issues with constexpr arrays in C++14/17
    static constexpr int16_t CHIRP_POWERS[4] = {32702, 32637, 32572, 32507};
};

// Out-of-line definition for C++14/17 compatibility (needed for ODR-use)

// ============================================================================
// Virtual Base Class for Extrapolators
// ============================================================================
// Template on CoeffFormat (same as MicroResamplerImpl) to match sample types.
// Each concrete extrapolator handles multi-channel data and format conversion
// internally.

template <typename CoeffFormat>
class Extrapolator {
public:
    using sample_t = typename CoeffFormat::sample_t;

    virtual ~Extrapolator() = default;

    // Allocate any scratch space needed to prime/flush with the given filter length.
    // MicroResamplerImpl calls this from initialize() so prime/flush never allocate while
    // streaming. Idempotent; prime_buffer()/prepare_flush() call it too, so standalone use
    // without an explicit allocate() still works.
    //
    // Returns false on allocation failure.
    virtual bool allocate(int filter_length) {
        (void)filter_length;
        return true;
    }

    // Prime the history buffer with extrapolated samples for smooth startup.
    // Extrapolates backward from the initial samples to fill the history buffer.
    //
    // Parameters:
    //   history_buffer: The resampler's history buffer to fill
    //                   Size: at least 2 * filter_length * num_channels samples
    //                   After priming, the first filter_length * num_channels samples
    //                   contain extrapolated data.
    //   num_channels: Number of interleaved channels
    //   initial_samples: First available input samples (interleaved)
    //   available_frames: Number of frames available in initial_samples
    //   filter_length: The filter length (determines how many samples to extrapolate)
    virtual void prime_buffer(sample_t* history_buffer, int num_channels,
                              const sample_t* initial_samples, int available_frames,
                              int filter_length) = 0;

    // Prepare the history buffer for flush by extrapolating forward.
    // Extends the signal beyond the last sample for smooth end-of-stream output.
    //
    // Parameters:
    //   history_buffer: The resampler's history buffer containing final samples
    //                   Size: at least 2 * filter_length * num_channels samples
    //                   The first filter_length * num_channels samples contain the
    //                   final input data; exactly filter_length / 2 + 1 extrapolated
    //                   frames are written after them (MicroResamplerImpl's flush consumes
    //                   that many, and a phase-wrap blend reads one frame further).
    //   num_channels: Number of interleaved channels
    //   filter_length: The filter length (determines extrapolation length)
    virtual void prepare_flush(sample_t* history_buffer, int num_channels, int filter_length) = 0;

    // Optional configuration
    virtual void set_lpc_order(int order) {
        (void)order;
    }
    virtual int get_lpc_order() const {
        return 0;
    }
};

// ============================================================================
// Zeros Extrapolator
// ============================================================================
// Simplest extrapolation: fills with zeros.
// Fastest but may cause discontinuities at boundaries.

template <typename CoeffFormat>
class ZerosExtrapolator : public Extrapolator<CoeffFormat> {
public:
    using sample_t = typename CoeffFormat::sample_t;

    void prime_buffer(sample_t* history_buffer, int num_channels, const sample_t* initial_samples,
                      int available_frames, int filter_length) override;

    void prepare_flush(sample_t* history_buffer, int num_channels, int filter_length) override;
};

// ============================================================================
// Mirror Extrapolator
// ============================================================================
// Simple signal reflection for fast boundary handling.
// Mirrors samples around the boundary point. Needs no scratch space.

template <typename CoeffFormat>
class MirrorExtrapolator : public Extrapolator<CoeffFormat> {
public:
    using sample_t = typename CoeffFormat::sample_t;

    void prime_buffer(sample_t* history_buffer, int num_channels, const sample_t* initial_samples,
                      int available_frames, int filter_length) override;

    void prepare_flush(sample_t* history_buffer, int num_channels, int filter_length) override;
};

// ============================================================================
// Burg LPC Extrapolator
// ============================================================================
// Uses Burg's method to find optimal reflection coefficients.
// Guarantees stable filter coefficients and provides high-quality extrapolation.

template <typename CoeffFormat>
class BurgLPCExtrapolator : public Extrapolator<CoeffFormat> {
public:
    using sample_t = typename CoeffFormat::sample_t;

    BurgLPCExtrapolator() = default;
    ~BurgLPCExtrapolator() override;

    // Owns raw scratch buffers
    BurgLPCExtrapolator(const BurgLPCExtrapolator&) = delete;
    BurgLPCExtrapolator& operator=(const BurgLPCExtrapolator&) = delete;

    bool allocate(int filter_length) override;

    void prime_buffer(sample_t* history_buffer, int num_channels, const sample_t* initial_samples,
                      int available_frames, int filter_length) override;

    void prepare_flush(sample_t* history_buffer, int num_channels, int filter_length) override;

    // Fixed-point sample formats cap the applied order at
    // FixedExtrapolation::DEFAULT_LPC_ORDER.
    void set_lpc_order(int order) override {
        lpc_order_ = std::clamp(order, 1, FloatExtrapolation::MAX_LPC_ORDER);
    }
    int get_lpc_order() const override {
        return this->lpc_order_;
    }

private:
    void free_buffers();

    int lpc_order_ = FloatExtrapolation::DEFAULT_LPC_ORDER;
    int allocated_filter_length_ = 0;  // Filter length the scratch buffers are sized for

    // Scratch buffers, each 2 * allocated_filter_length_ entries (prime needs at most
    // filter_length input + filter_length extrapolated samples per channel)
    sample_t* channel_buffer_ = nullptr;
    float* conversion_buffer_ = nullptr;  // Float formats only: samples converted to float
    double* error_buffer_ = nullptr;      // Float formats only: Burg forward/backward errors
};

// ============================================================================
// Factory Function
// ============================================================================
// Creates an extrapolator of the specified type

template <typename CoeffFormat>
std::unique_ptr<Extrapolator<CoeffFormat>> create_extrapolator(ExtrapolationMode mode) {
    switch (mode) {
        case ExtrapolationMode::ZEROS:
            return std::make_unique<ZerosExtrapolator<CoeffFormat>>();
        case ExtrapolationMode::MIRROR:
            return std::make_unique<MirrorExtrapolator<CoeffFormat>>();
        case ExtrapolationMode::BURG_LPC:
            return std::make_unique<BurgLPCExtrapolator<CoeffFormat>>();
    }
    // Should never reach here, but needed for compiler
    return std::make_unique<MirrorExtrapolator<CoeffFormat>>();
}

}  // namespace micro_resampler

#endif  // EXTRAPOLATOR_H
