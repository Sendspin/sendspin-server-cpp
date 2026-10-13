// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file coefficient_generator_impl.h
/// @brief Out-of-line definition of CoefficientGeneratorImpl::generate_filter_bank
///
/// Provides the template member function body for the CoefficientGeneratorImpl
/// class declared in coefficient_generator.h. It casts the type-erased filter
/// bank and MaxTapEntry pointers to their concrete CoeffFormat types and forwards
/// to the templated generate_filter_bank() free function. This body is kept in a
/// separate header, included only after sinc_table.h, filter_design.h, and
/// precision_strategy.h are available, so that circular dependencies are avoided.

#ifndef COEFFICIENT_GENERATOR_IMPL_H
#define COEFFICIENT_GENERATOR_IMPL_H

#include "coefficient_generator.h"
#include "detail/precision_strategy.h"  // IWYU pragma: keep (PrecisionStrategy template arg must be complete at instantiation)
#include "resampler/filter_design.h"
#include "sinc_table.h"

#include <cstdint>

namespace micro_resampler {

template <typename CoeffFormat, typename PrecisionStrategy>
void CoefficientGeneratorImpl<CoeffFormat, PrecisionStrategy>::generate_filter_bank(
    void* filter_bank, int num_phases, const FilterDesign& design, double input_rate,
    double output_rate, uint8_t* coefficient_shifts, uint8_t* coefficient_shift_blocks,
    void* max_tap_entries, int max_taps_per_phase) {
    // Cast type-erased pointers back to their concrete types
    auto* typed_bank = static_cast<typename CoeffFormat::coefficient_t*>(filter_bank);
    auto* typed_max_taps = static_cast<MaxTapEntry<CoeffFormat>*>(max_tap_entries);

    // Call the existing templated function with concrete types
    micro_resampler::generate_filter_bank<CoeffFormat, PrecisionStrategy>(
        typed_bank, num_phases, design, input_rate, output_rate, coefficient_shifts,
        coefficient_shift_blocks, typed_max_taps, max_taps_per_phase);
}

}  // namespace micro_resampler

#endif  // COEFFICIENT_GENERATOR_IMPL_H
