// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt

#include "sinc_table.h"

#include "detail/coefficient_ops.h"
#include "detail/fixed_generic.h"
#include "detail/fixed_point.h"
#include "detail/precision_strategy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#ifdef ESP_PLATFORM
#endif

namespace micro_resampler {

// Helper struct to track top N coefficients by magnitude
struct TopN {
    // Direct generate_filter_bank() callers asking for more get the extra entries zeroed, so the
    // extracted taps never exceed the bound apply_max_taps() relies on
    static constexpr int MAX_CAPACITY = FilterDesign::MAX_TAPS_PER_PHASE;
    float values[MAX_CAPACITY]{};
    int indices[MAX_CAPACITY]{};
    int capacity;
    int count = 0;
    int min_idx = 0;

    explicit TopN(int n) : capacity(std::min(n, MAX_CAPACITY)) {
        // Initialize arrays to zero
        for (int i = 0; i < MAX_CAPACITY; ++i) {
            this->values[i] = 0.0F;
            this->indices[i] = 0;
        }
    }

    void maybe_insert(float val, int idx) {
        if (this->count < this->capacity) {
            // Still filling up, just add it
            this->values[this->count] = val;
            this->indices[this->count] = idx;
            if (val < this->values[this->min_idx]) {
                this->min_idx = this->count;
            }
            this->count++;
        } else if (val > this->values[this->min_idx]) {
            // Replace minimum with new value
            this->values[this->min_idx] = val;
            this->indices[this->min_idx] = idx;

            // Find new minimum (O(N) scan)
            this->min_idx = 0;
            for (int i = 1; i < this->capacity; ++i) {
                if (this->values[i] < this->values[this->min_idx]) {
                    this->min_idx = i;
                }
            }
        }
    }
};

// ============================================================================
// Helper Functions for Per-Phase Coefficient Generation
// ============================================================================

// Generate all coefficients for one phase
// Uses PrecisionStrategy::calc_t for all calculations and output
// Returns coefficients in FORWARD tap order (caller handles reversal at storage)
template <typename PrecisionStrategy>
static void generate_phase_coefficients(typename PrecisionStrategy::calc_t* output, int phase,
                                        int num_phases, int filter_length, double cutoff,
                                        double kaiser_beta, double bessel_denominator) {
    using calc_t = typename PrecisionStrategy::calc_t;
    const int total_length = num_phases * filter_length;
    const calc_t half_length = static_cast<calc_t>(total_length) / static_cast<calc_t>(2.0);

    for (int tap = 0; tap < filter_length; ++tap) {
        int n = phase + tap * num_phases;
        if (n < total_length) {
            calc_t x = (static_cast<calc_t>(n) - half_length) / static_cast<calc_t>(num_phases);
            calc_t window_val = PrecisionStrategy::evaluate_window(
                n, total_length, static_cast<calc_t>(kaiser_beta),
                static_cast<calc_t>(bessel_denominator));
            output[tap] = static_cast<calc_t>(2.0) * static_cast<calc_t>(cutoff) *
                          PrecisionStrategy::sinc(static_cast<calc_t>(2.0) *
                                                  static_cast<calc_t>(cutoff) * x) *
                          window_val;
        } else {
            output[tap] = static_cast<calc_t>(0.0);
        }
    }
}

// Extract the N largest coefficients by magnitude
// Zeros them out in the working array and returns their info
template <typename CalcT, typename CoeffFormat>
static void extract_max_taps(CalcT* coeffs, int filter_length, MaxTapEntry<CoeffFormat>* entries,
                             int max_taps_count) {
    TopN tracker(max_taps_count);
    for (int tap = 0; tap < filter_length; ++tap) {
        tracker.maybe_insert(std::abs(static_cast<float>(coeffs[tap])), tap);
    }

    for (int i = 0; i < tracker.count; ++i) {
        int tap_idx = tracker.indices[i];
        entries[i].coeff = FLOAT_TO_Q31(coeffs[tap_idx]);
        entries[i].index_reversed = static_cast<uint16_t>(filter_length - 1 - tap_idx);
        coeffs[tap_idx] = static_cast<CalcT>(0);  // Zero out for block shift calculation
    }

    // Zero unused slots
    for (int i = tracker.count; i < max_taps_count; ++i) {
        entries[i].coeff = 0;
        entries[i].index_reversed = 0;
    }
}

// Convert phase coefficients to Q15 with block-adaptive shifts
// Coefficients are in forward order, output is in REVERSED order
template <typename CalcT, typename CoeffFormat>
static void convert_phase_to_q15_block_adaptive(const CalcT* coeffs,
                                                typename CoeffFormat::coefficient_t* output,
                                                uint8_t* block_shifts, int filter_length) {
    constexpr int BLOCK_SIZE = 8;
    const int num_blocks = filter_length / BLOCK_SIZE;

    // Precision-bit sentinels used to select per-format conversion behavior below.
    constexpr int Q31_PRECISION_BITS = 31;  // Full-precision Q31: no adaptive shift needed
    constexpr int Q15_PRECISION_BITS = 15;  // Q15: convert with 16-bit saturation

    for (int block = 0; block < num_blocks; ++block) {
        // Block is in REVERSED storage order
        int reversed_start = block * BLOCK_SIZE;

        // Collect coefficients for this block (convert reversed index to forward)
        CalcT block_coeffs[BLOCK_SIZE];
        for (int i = 0; i < BLOCK_SIZE; ++i) {
            int reversed_tap = reversed_start + i;
            int forward_tap = filter_length - 1 - reversed_tap;
            block_coeffs[i] = coeffs[forward_tap];
        }

        // Calculate block shift
        uint8_t shift = 0;
        if constexpr (CoeffFormat::PRECISION_BITS == Q31_PRECISION_BITS) {
            shift = 0;  // Q31: no adaptive shift possible
        } else {
            shift = calculate_block_shift<CoeffFormat::PRECISION_BITS>(block_coeffs, BLOCK_SIZE);
        }
        block_shifts[block] =
            (Q31_PRECISION_BITS - CoeffFormat::PRECISION_BITS) - shift;  // Store processing shift

        // Convert and store
        for (int i = 0; i < BLOCK_SIZE; ++i) {
            int32_t q31 = FLOAT_TO_Q31(block_coeffs[i]);
            if constexpr (CoeffFormat::PRECISION_BITS == Q15_PRECISION_BITS) {
                output[reversed_start + i] = static_cast<typename CoeffFormat::coefficient_t>(
                    SATURATE16(PSHR32_SAFE(q31, shift)));
            } else {
                output[reversed_start + i] =
                    static_cast<typename CoeffFormat::coefficient_t>(PSHR32_SAFE(q31, shift));
            }
        }
    }
}

// Convert phase coefficients to fixed-point format (Q31/Q31Lite)
// Coefficients are in forward order, output is in REVERSED order
template <typename CalcT, typename CoeffFormat>
static void convert_phase_to_fixed(const CalcT* coeffs, typename CoeffFormat::coefficient_t* output,
                                   int filter_length) {
    for (int tap = 0; tap < filter_length; ++tap) {
        int reversed_tap = filter_length - 1 - tap;
        output[reversed_tap] = detail::FormatOps<CoeffFormat>::float_to_coefficient(coeffs[tap]);
    }
}

// Convert phase coefficients to floating-point format (Float32/Float64)
// Coefficients are in forward order, output is in REVERSED order
template <typename CalcT, typename CoeffFormat>
static void convert_phase_to_float(const CalcT* coeffs, typename CoeffFormat::coefficient_t* output,
                                   int filter_length) {
    for (int tap = 0; tap < filter_length; ++tap) {
        int reversed_tap = filter_length - 1 - tap;
        output[reversed_tap] = static_cast<typename CoeffFormat::coefficient_t>(coeffs[tap]);
    }
}

// ============================================================================
// Main Filter Bank Generation
// ============================================================================

template <typename CoeffFormat, typename PrecisionStrategy>
void generate_filter_bank(typename CoeffFormat::coefficient_t* filter_bank, int num_phases,
                          const FilterDesign& design, double input_rate, double output_rate,
                          // coefficient_shifts is unused here but kept as uint8_t* to match the
                          // virtual CoefficientGenerator interface signature.
                          // NOLINTNEXTLINE(readability-non-const-parameter)
                          uint8_t* coefficient_shifts, uint8_t* coefficient_shift_blocks,
                          MaxTapEntry<CoeffFormat>* max_tap_entries, int max_taps_per_phase) {
    (void)coefficient_shifts;  // Kept for API compatibility with calculate_filter_bank_dc_gain
    using calc_t = typename PrecisionStrategy::calc_t;

    const int filter_length = design.get_filter_length();
    const double kaiser_beta = design.get_kaiser_beta();

    // Simplified cutoff calculation: directly based on Nyquist frequencies
    // For downsampling: Use configured % of output Nyquist to prevent aliasing
    // For upsampling: Use configured % of input Nyquist to preserve signal
    // For equal rates: Use the downsampling percentage (more conservative)
    double cutoff = 0.0;  // Will be set in all branches below
    if (output_rate < input_rate) {
        // Downsampling: cutoff at X% of output Nyquist (as fraction of input rate)
        const double nyquist_hz = 0.5 * output_rate;
        cutoff = design.get_downsampling_cutoff_percent() * nyquist_hz / input_rate;
    } else if (output_rate > input_rate) {
        // Upsampling: cutoff at X% of input Nyquist (as fraction of input rate)
        const double nyquist_hz = 0.5 * input_rate;
        cutoff = design.get_upsampling_cutoff_percent() * nyquist_hz / input_rate;
    } else {
        // Equal rates: use downsampling percentage, normalized to Nyquist
        cutoff = design.get_downsampling_cutoff_percent() * 0.5;
    }

    // Precompute bessel denominator once for efficiency
    const calc_t bessel_denominator =
        PrecisionStrategy::bessel_i0(static_cast<calc_t>(kaiser_beta));

    // Allocate working buffer for one phase using the precision strategy's calc_t
    std::vector<calc_t> phase_coeffs(filter_length);

    const bool use_block_adaptive_q = (coefficient_shift_blocks != nullptr);

    for (int phase = 0; phase < num_phases; ++phase) {
        // STEP 1: Generate all coefficients for this phase in calc_t precision
        generate_phase_coefficients<PrecisionStrategy>(phase_coeffs.data(), phase, num_phases,
                                                       filter_length, cutoff, kaiser_beta,
                                                       static_cast<double>(bessel_denominator));

        // STEP 2: Format-specific conversion
        auto* phase_output = &filter_bank[static_cast<size_t>(phase) * filter_length];

        if constexpr (!CoeffFormat::USE_FLOAT) {
            if (use_block_adaptive_q) {
                // Q15 with block adaptive shifts

                // Extract max taps if enabled
                if (max_tap_entries != nullptr) {
                    extract_max_taps<calc_t, CoeffFormat>(
                        phase_coeffs.data(), filter_length,
                        &max_tap_entries[static_cast<size_t>(phase) * max_taps_per_phase],
                        max_taps_per_phase);
                }

                // Convert with block shifts
                convert_phase_to_q15_block_adaptive<calc_t, CoeffFormat>(
                    phase_coeffs.data(), phase_output,
                    &coefficient_shift_blocks[static_cast<size_t>(phase) * (filter_length / 8)],
                    filter_length);
            } else {
                // Q31/Q31Lite - standard fixed-point
                convert_phase_to_fixed<calc_t, CoeffFormat>(phase_coeffs.data(), phase_output,
                                                            filter_length);
            }
        } else {
            // Float32/Float64
            convert_phase_to_float<calc_t, CoeffFormat>(phase_coeffs.data(), phase_output,
                                                        filter_length);
        }
    }
}

template <typename CoeffFormat, typename PrecisionStrategy>
FilterBank<CoeffFormat> create_filter_bank(double input_rate, double output_rate,
                                           const FilterDesign& design, int num_phases) {
    FilterBank<CoeffFormat> bank;

    // Use provided phase count or default from design
    if (num_phases == 0) {
        num_phases = design.get_default_phase_count();
    }

    bank.filter_length = design.get_filter_length();
    bank.kaiser_beta = design.get_kaiser_beta();

    bank.num_phases = num_phases + 1;  // +1 for interpolation continuity
    bank.input_rate = input_rate;
    bank.output_rate = output_rate;

    const size_t total_coeffs = bank.num_phases * bank.filter_length;
    bank.coefficients.resize(total_coeffs);

    generate_filter_bank<CoeffFormat, PrecisionStrategy>(bank.coefficients.data(), num_phases,
                                                         design, input_rate, output_rate,
                                                         nullptr  // Per-tap shifts unused
    );

    return bank;
}

template <typename CoeffFormat>
double calculate_filter_bank_dc_gain(const typename CoeffFormat::coefficient_t* filter_bank,
                                     int filter_length, const uint8_t* coefficient_shifts) {
    (void)coefficient_shifts;  // Per-tap shifts unused
    if constexpr (!CoeffFormat::USE_FLOAT) {
        // Standard fixed-point format
        typename CoeffFormat::accumulator_t sum = 0;
        for (int i = 0; i < filter_length; ++i) {
            sum += filter_bank[i];
        }
        return Q15_TO_FLOAT(SATURATE16(sum));
    } else {
        // Floating-point format: direct summation
        double sum = 0.0;
        for (int i = 0; i < filter_length; ++i) {
            sum += filter_bank[i];
        }
        return sum;
    }
}

// ============================================================================
// Explicit template instantiations
// ============================================================================
// Instantiate with DefaultPrecisionStrategy for backward compatibility

template void generate_filter_bank<Q15Format, DefaultPrecisionStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, DefaultPrecisionStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, DefaultPrecisionStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float64Format, DefaultPrecisionStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);
template void generate_filter_bank<Float32Format, DefaultPrecisionStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);

template FilterBank<Q15Format> create_filter_bank<Q15Format, DefaultPrecisionStrategy>(
    double, double, const FilterDesign&, int);
template FilterBank<Q31Format> create_filter_bank<Q31Format, DefaultPrecisionStrategy>(
    double, double, const FilterDesign&, int);
template FilterBank<Q31LiteFormat> create_filter_bank<Q31LiteFormat, DefaultPrecisionStrategy>(
    double, double, const FilterDesign&, int);
template FilterBank<Float64Format> create_filter_bank<Float64Format, DefaultPrecisionStrategy>(
    double, double, const FilterDesign&, int);
template FilterBank<Float32Format> create_filter_bank<Float32Format, DefaultPrecisionStrategy>(
    double, double, const FilterDesign&, int);

// ============================================================================
// Precision Strategy Instantiations for CoefficientGeneratorImpl
// ============================================================================
// The CoefficientGeneratorImpl template calls generate_filter_bank, so we need
// to explicitly instantiate the combinations used by the factory method.
// These are minimal instantiations - only for the base CoeffFormats that users
// can instantiate MicroResamplerImpl with.

// DoublePrecisionStrategy - used by factory
// Only instantiate explicitly when it's not the default (i.e., on ESP32)
#if defined(ESP_PLATFORM) || defined(ESP32) || defined(ESP32S3)
template void generate_filter_bank<Q15Format, DoublePrecisionStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, DoublePrecisionStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, DoublePrecisionStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float32Format, DoublePrecisionStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);
template void generate_filter_bank<Float64Format, DoublePrecisionStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);
#endif

// SinglePrecisionStrategy - used by factory
template void generate_filter_bank<Q15Format, SinglePrecisionStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, SinglePrecisionStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, SinglePrecisionStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float32Format, SinglePrecisionStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);
template void generate_filter_bank<Float64Format, SinglePrecisionStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);

// TableFloatStrategy - used by factory
// Only instantiate explicitly when it's not the default (i.e., not on ESP32)
#if !defined(ESP_PLATFORM) && !defined(ESP32) && !defined(ESP32S3)
template void generate_filter_bank<Q15Format, TableFloatStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, TableFloatStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, TableFloatStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float32Format, TableFloatStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);
template void generate_filter_bank<Float64Format, TableFloatStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);
#endif

// TableDoubleStrategy - used by factory
template void generate_filter_bank<Q15Format, TableDoubleStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, TableDoubleStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, TableDoubleStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float32Format, TableDoubleStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);
template void generate_filter_bank<Float64Format, TableDoubleStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);
// ============================================================================

// QuadPrecisionStrategy - used by factory when quad support is compiled in
#ifdef RESAMPLER_ENABLE_QUAD_PRECISION
template void generate_filter_bank<Q15Format, QuadPrecisionStrategy>(
    typename Q15Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q15Format>*, int);
template void generate_filter_bank<Q31Format, QuadPrecisionStrategy>(
    typename Q31Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31Format>*, int);
template void generate_filter_bank<Q31LiteFormat, QuadPrecisionStrategy>(
    typename Q31LiteFormat::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Q31LiteFormat>*, int);
template void generate_filter_bank<Float32Format, QuadPrecisionStrategy>(
    typename Float32Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float32Format>*, int);
template void generate_filter_bank<Float64Format, QuadPrecisionStrategy>(
    typename Float64Format::coefficient_t*, int, const FilterDesign&, double, double, uint8_t*,
    uint8_t*, MaxTapEntry<Float64Format>*, int);
#endif

template double calculate_filter_bank_dc_gain<Q15Format>(const Q15Format::coefficient_t*, int,
                                                         const uint8_t*);
template double calculate_filter_bank_dc_gain<Q31Format>(const Q31Format::coefficient_t*, int,
                                                         const uint8_t*);
template double calculate_filter_bank_dc_gain<Q31LiteFormat>(const Q31LiteFormat::coefficient_t*,
                                                             int, const uint8_t*);
template double calculate_filter_bank_dc_gain<Float64Format>(const Float64Format::coefficient_t*,
                                                             int, const uint8_t*);
template double calculate_filter_bank_dc_gain<Float32Format>(const Float32Format::coefficient_t*,
                                                             int, const uint8_t*);

}  // namespace micro_resampler
