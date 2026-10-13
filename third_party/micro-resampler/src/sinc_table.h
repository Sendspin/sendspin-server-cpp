// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

/// @file sinc_table.h
/// @brief Filter bank types and generation functions for the polyphase sinc table
///
/// Defines FilterBank and MaxTapEntry, and declares functions to generate and
/// query a coefficient table from a FilterDesign, including per-phase DC gain
/// calculation

#ifndef SINC_TABLE_H
#define SINC_TABLE_H

#include "detail/precision_strategy.h"
#include "resampler/filter_design.h"

#include <cstdint>
#include <vector>

namespace micro_resampler {

template <typename CoeffFormat>
struct FilterBank {
    using coefficient_t = typename CoeffFormat::coefficient_t;

    std::vector<coefficient_t> coefficients;
    int num_phases = 0;
    int filter_length = 0;
    double input_rate = 0.0;
    double output_rate = 0.0;
    double kaiser_beta = 0.0;
};

// Interleaved max tap data for better cache locality
template <typename CoeffFormat>
struct MaxTapEntry {
    using processing_t = typename CoeffFormat::processing_t;

    processing_t coeff{};       // Extracted max coefficient in Q31 format
    uint16_t index_reversed{};  // Tap index (pre-reversed for sample array access)
                                // 16-bit: filter lengths can exceed 256 taps
    uint8_t padding[2]{};       // Align to 8 bytes for efficient access
};

template <typename CoeffFormat, typename PrecisionStrategy = DefaultPrecisionStrategy>
void generate_filter_bank(
    typename CoeffFormat::coefficient_t* filter_bank, int num_phases, const FilterDesign& design,
    double input_rate, double output_rate,
    uint8_t* coefficient_shifts =
        nullptr,  // Optional: per-coefficient shift values for adaptive Q-format
    uint8_t* coefficient_shift_blocks =
        nullptr,  // Optional: per-block-coefficient shift values for adaptive Q-format
    MaxTapEntry<CoeffFormat>* max_tap_entries =
        nullptr,                // Optional: interleaved max tap data per phase
    int max_taps_per_phase = 4  // Number of largest coefficients to extract per phase (default: 4)
);

template <typename CoeffFormat, typename PrecisionStrategy = DefaultPrecisionStrategy>
FilterBank<CoeffFormat> create_filter_bank(double input_rate, double output_rate,
                                           const FilterDesign& design, int num_phases = 0);

template <typename CoeffFormat>
double calculate_filter_bank_dc_gain(const typename CoeffFormat::coefficient_t* filter_bank,
                                     int filter_length,
                                     const uint8_t* coefficient_shifts = nullptr);

}  // namespace micro_resampler

#endif
