// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

/// @file coefficient_generator.h
/// @brief Abstract, type-erased interface for polyphase filter bank coefficient generation
///
/// Declares CoefficientGenerator, an abstract base class whose generate_filter_bank()
/// virtual method allows runtime selection of coefficient precision strategies without a
/// combinatorial explosion of template parameters at call sites. Also declares the
/// CoefficientGeneratorImpl class template that pairs a concrete CoeffFormat with a
/// PrecisionStrategy; its member function definition lives in coefficient_generator_impl.h.

#ifndef COEFFICIENT_GENERATOR_H
#define COEFFICIENT_GENERATOR_H

#include <cstdint>

namespace micro_resampler {

// Forward declarations
class FilterDesign;
template <typename CoeffFormat>
struct MaxTapEntry;

// ============================================================================
// Abstract Base Class for Coefficient Generation (Type-Erased Interface)
// ============================================================================
// This base class enables runtime selection of precision strategies without
// template parameter explosion. Concrete implementations are instantiated
// on-demand via the factory pattern.
class CoefficientGenerator {
public:
    virtual ~CoefficientGenerator() = default;

    /// @brief Generate filter bank coefficients using the strategy's precision
    /// @param filter_bank Type-erased pointer to coefficient array (must match CoeffFormat)
    /// @param num_phases Number of polyphase filters
    /// @param design Filter design parameters (length, beta, cutoffs)
    /// @param input_rate Input sample rate
    /// @param output_rate Output sample rate
    /// @param coefficient_shifts Legacy parameter (unused, for API compatibility)
    /// @param coefficient_shift_blocks Per-block shift values for Q15 adaptive precision
    /// @param max_tap_entries Type-erased pointer to MaxTapEntry array (must match CoeffFormat)
    /// @param max_taps_per_phase Number of largest taps to extract per phase
    virtual void generate_filter_bank(void* filter_bank, int num_phases, const FilterDesign& design,
                                      double input_rate, double output_rate,
                                      uint8_t* coefficient_shifts,
                                      uint8_t* coefficient_shift_blocks, void* max_tap_entries,
                                      int max_taps_per_phase) = 0;
};

// ============================================================================
// Templated Implementation (Instantiated On-Demand)
// ============================================================================
// This templated class wraps the existing generate_filter_bank template function.
// Only the combinations actually used in your code will be instantiated by the
// compiler, eliminating the need for explicit instantiations of all strategy
// combinations.
//
// Template Parameters:
//   CoeffFormat - The coefficient format (Q15Format, Q31Format, Float64Format, etc.)
//   PrecisionStrategy - The precision strategy (SinglePrecisionStrategy, etc.)
//
// NOTE: Implementation is provided in coefficient_generator_impl.h to avoid
// circular dependencies with sinc_table.h
template <typename CoeffFormat, typename PrecisionStrategy>
class CoefficientGeneratorImpl : public CoefficientGenerator {
public:
    void generate_filter_bank(void* filter_bank, int num_phases, const FilterDesign& design,
                              double input_rate, double output_rate, uint8_t* coefficient_shifts,
                              uint8_t* coefficient_shift_blocks, void* max_tap_entries,
                              int max_taps_per_phase) override;
};

}  // namespace micro_resampler

#endif  // COEFFICIENT_GENERATOR_H
