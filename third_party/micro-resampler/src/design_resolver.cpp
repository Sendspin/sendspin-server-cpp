// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Kevin Ahrendt

#include "resampler/design_resolver.h"

#include "detail/precision_strategy.h"  // For DefaultPrecisionStrategy
#include "resampler/resampler.h"        // For MAX_FILTER_LENGTH

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <type_traits>

namespace micro_resampler {

namespace {

// Kaiser window design formula constants (Oppenheim & Schafer). The filter
// length estimate is N = (A - 7.95) / (2.285 * delta_omega) with delta_omega
// the transition width in radians per input sample.
constexpr double K_KAISER_LENGTH_OFFSET_DB = 7.95;
constexpr double K_KAISER_LENGTH_SLOPE = 2.285;

// Kaiser beta piecewise formula boundaries and coefficients
constexpr double K_BETA_HIGH_ATTEN_DB = 50.0;
constexpr double K_BETA_LOW_ATTEN_DB = 21.0;
constexpr double K_BETA_HIGH_SCALE = 0.1102;
constexpr double K_BETA_HIGH_OFFSET_DB = 8.7;
constexpr double K_BETA_MID_POW_SCALE = 0.5842;
constexpr double K_BETA_MID_POW_EXPONENT = 0.4;
constexpr double K_BETA_MID_LINEAR_SCALE = 0.07886;

// Image suppression gained per doubling of the phase count when linearly
// interpolating between adjacent phases. Rule-of-thumb value, provisional
// until validated by the phase-slope sweep (docs/configuration_redesign.md)
constexpr double K_INTERP_DB_PER_OCTAVE = 12.0;

// Maximum polyphase branch count (matches the documented 1-1024 range)
constexpr uint32_t K_MAX_PHASE_COUNT = 1024;

// Filter lengths must be a multiple of 8 for the ESP32-S3 SIMD dot product
constexpr int K_TAP_MULTIPLE = 8;

// Sanity clamps for the incoming specification
constexpr double K_MIN_STOPBAND_DB = 10.0;
constexpr double K_MAX_STOPBAND_DB = 300.0;
constexpr double K_MIN_PASSBAND_FRACTION = 0.01;
constexpr double K_MAX_PASSBAND_FRACTION = 0.999;

// Kaiser beta values with precomputed window tables (detail/kaiser_tables.h).
// Table-based coefficient generation snaps to this grid
constexpr double K_TABLE_BETA_GRID[] = {6.0, 8.0, 10.0, 12.0};

double beta_for_attenuation(double attenuation_db) {
    if (attenuation_db > K_BETA_HIGH_ATTEN_DB) {
        return K_BETA_HIGH_SCALE * (attenuation_db - K_BETA_HIGH_OFFSET_DB);
    }
    if (attenuation_db >= K_BETA_LOW_ATTEN_DB) {
        const double excess = attenuation_db - K_BETA_LOW_ATTEN_DB;
        return K_BETA_MID_POW_SCALE * std::pow(excess, K_BETA_MID_POW_EXPONENT) +
               K_BETA_MID_LINEAR_SCALE * excess;
    }
    return 0.0;
}

// Attenuation a Kaiser window delivers at the given beta - the inverse of
// beta_for_attenuation's high-attenuation branch. Valid for beta > ~4.55
// (attenuation > 50 dB), which covers the whole table grid
double attenuation_for_beta(double beta) {
    return beta / K_BETA_HIGH_SCALE + K_BETA_HIGH_OFFSET_DB;
}

// Smallest table beta whose window meets the requested attenuation,
// stopband-first. Returns the grid ceiling (beta 12, ~117.6 dB) when the
// request exceeds it; the caller reports the shortfall through
// achieved_stopband_db
double snap_beta_to_table(double attenuation_db) {
    for (double beta : K_TABLE_BETA_GRID) {
        if (attenuation_for_beta(beta) >= attenuation_db) {
            return beta;
        }
    }
    return K_TABLE_BETA_GRID[std::size(K_TABLE_BETA_GRID) - 1];
}

// Taps needed for the requested attenuation across a transition band of
// transition_fraction (as a fraction of the input rate), before rounding
double ideal_filter_length(double attenuation_db, double transition_fraction) {
    const double delta_omega = 2.0 * M_PI * transition_fraction;
    return (attenuation_db - K_KAISER_LENGTH_OFFSET_DB) / (K_KAISER_LENGTH_SLOPE * delta_omega);
}

// Transition width (fraction of the input rate) achievable with the given
// tap count at the requested attenuation - the inverse of ideal_filter_length
double achievable_transition_fraction(double attenuation_db, int filter_length) {
    return (attenuation_db - K_KAISER_LENGTH_OFFSET_DB) /
           (K_KAISER_LENGTH_SLOPE * 2.0 * M_PI * static_cast<double>(filter_length));
}

int round_up_to_tap_multiple(double taps) {
    const int rounded = static_cast<int>(std::ceil(taps));
    const int remainder = rounded % K_TAP_MULTIPLE;
    const int aligned = (remainder == 0) ? rounded : rounded + (K_TAP_MULTIPLE - remainder);
    return std::max(aligned, K_TAP_MULTIPLE);
}

// Phase count needed so that linear-interpolation images stay below the
// stopband target: P = 2^(A / dB-per-octave), clamped to the supported range
uint32_t interpolated_phase_count(double attenuation_db) {
    const double ideal = std::exp2(attenuation_db / K_INTERP_DB_PER_OCTAVE);
    if (ideal >= static_cast<double>(K_MAX_PHASE_COUNT)) {
        return K_MAX_PHASE_COUNT;
    }
    const auto count = static_cast<uint32_t>(std::ceil(ideal));
    return std::max(count, uint32_t{1});
}

// Image suppression floor imposed by linearly interpolating P phases
double interpolation_floor_db(uint32_t phases) {
    return K_INTERP_DB_PER_OCTAVE * std::log2(static_cast<double>(phases));
}

size_t bank_bytes(uint32_t phases, int filter_length, size_t bytes_per_coefficient) {
    return static_cast<size_t>(phases) * static_cast<size_t>(filter_length) * bytes_per_coefficient;
}

}  // namespace

QualitySpec quality_spec_from_preset(ResamplerQuality quality) {
    // Provisional values pending sweep validation (docs/configuration_redesign.md)
    switch (quality) {
        case ResamplerQuality::VERY_LOW:
            return {45.0, 0.80};
        case ResamplerQuality::LOW:
            // Same 80 dB stopband as MEDIUM (the beta-6 window's ~63 dB
            // leaves aliasing spikes above the 16-bit audibility floor no
            // matter how many taps are spent); the narrower passband is
            // what makes this tier cheap
            return {80.0, 0.80};
        case ResamplerQuality::MEDIUM:
            return {80.0, 0.90};
        case ResamplerQuality::HIGH:
            // 99 dB rather than 100: it fits under the beta-10 table window
            // (A(10) ~= 99.44 dB), so table-based generation lands on beta 10
            // instead of jumping to beta 12 for the last half decibel
            return {99.0, 0.93};
        case ResamplerQuality::VERY_HIGH:
            return {120.0, 0.95};
        case ResamplerQuality::LUDICROUS:
            return {160.0, 0.98};
    }
    return {80.0, 0.90};  // Unreachable; matches MEDIUM
}

ResolvedDesign resolve_design(const QualitySpec& spec, uint32_t input_rate, uint32_t output_rate,
                              const DesignConstraints& constraints) {
    ResolvedDesign result;

    if (input_rate == 0 || output_rate == 0 || constraints.bytes_per_coefficient == 0) {
        return result;  // feasible defaults to false
    }

    const double attenuation_db =
        std::clamp(spec.stopband_db, K_MIN_STOPBAND_DB, K_MAX_STOPBAND_DB);
    const double passband_fraction =
        std::clamp(spec.passband_fraction, K_MIN_PASSBAND_FRACTION, K_MAX_PASSBAND_FRACTION);

    // The stopband edge is pinned at the lower Nyquist frequency: for
    // downsampling that is where aliasing begins, for upsampling where
    // imaging begins. The transition band spans passband edge -> stopband
    // edge, and the per-phase filter operates at the input rate.
    const double stopband_hz = 0.5 * static_cast<double>(std::min(input_rate, output_rate));
    const double requested_passband_hz = passband_fraction * stopband_hz;
    const double transition_fraction =
        (stopband_hz - requested_passband_hz) / static_cast<double>(input_rate);

    // The window attenuation is what the Kaiser window is designed to
    // deliver. It matches the request unless the table beta grid forces a
    // snap: then it is the snapped window's attenuation - above the request
    // when snapping up (the filter must be sized for the stronger window to
    // realize it), below it only when the request exceeds the grid ceiling.
    result.kaiser_beta = beta_for_attenuation(attenuation_db);
    double window_db = attenuation_db;
    if (constraints.table_beta_grid) {
        result.kaiser_beta = snap_beta_to_table(attenuation_db);
        window_db = attenuation_for_beta(result.kaiser_beta);
    }

    result.requested_passband_hz = requested_passband_hz;
    result.achieved_passband_hz = requested_passband_hz;

    int filter_length =
        round_up_to_tap_multiple(ideal_filter_length(window_db, transition_fraction));
    bool passband_reduced = filter_length > MAX_FILTER_LENGTH;
    filter_length = std::min(filter_length, MAX_FILTER_LENGTH);

    // Phase selection, CPU-first: exact phases (GCD-simplified output rate)
    // whenever they fit, otherwise linear interpolation with enough phases
    // to keep interpolation images below the stopband target.
    // Interpolation images only need to stay below the lesser of the
    // requested stopband and what the window delivers: a snapped-up window
    // must not inflate the phase count, and a grid-ceiling window cannot
    // benefit from suppression beyond its own attenuation.
    const uint32_t rate_gcd = std::gcd(input_rate, output_rate);
    const uint32_t exact_phases = output_rate / rate_gcd;
    const uint32_t interp_phases = interpolated_phase_count(std::min(attenuation_db, window_db));
    const size_t budget = constraints.memory_budget_bytes;

    const bool exact_allowed = !constraints.asrc && exact_phases <= K_MAX_PHASE_COUNT;
    const bool exact_dominant = exact_phases <= interp_phases;
    const bool exact_fits = budget == 0 || bank_bytes(exact_phases, filter_length,
                                                      constraints.bytes_per_coefficient) <= budget;

    uint32_t phases = 0;
    if (exact_allowed && (exact_dominant || exact_fits)) {
        phases = exact_phases;
        result.phase_mode = PhaseMode::EXACT;
        result.dotprods_per_output = 1;
    } else {
        phases = interp_phases;
        result.phase_mode = PhaseMode::INTERPOLATED;
        result.dotprods_per_output = 2;
    }

    // If the chosen bank exceeds the budget, keep the attenuation and shrink
    // the filter instead: the transition band widens downward and the
    // achieved passband shrinks. Attenuation is never silently sacrificed.
    if (budget > 0 &&
        bank_bytes(phases, filter_length, constraints.bytes_per_coefficient) > budget) {
        const size_t max_taps =
            budget / (static_cast<size_t>(phases) * constraints.bytes_per_coefficient);
        const int budget_taps = static_cast<int>(max_taps / K_TAP_MULTIPLE) * K_TAP_MULTIPLE;
        if (budget_taps < K_TAP_MULTIPLE) {
            filter_length = K_TAP_MULTIPLE;  // Best effort for the report
        } else {
            filter_length = budget_taps;
        }
        passband_reduced = true;
    }

    if (passband_reduced) {
        const double achieved_transition = achievable_transition_fraction(window_db, filter_length);
        result.achieved_passband_hz =
            stopband_hz - achieved_transition * static_cast<double>(input_rate);
    }

    // Feasibility: the budget must hold the final bank, and the widened
    // transition band must leave a usable passband.
    const size_t final_bytes = bank_bytes(phases, filter_length, constraints.bytes_per_coefficient);
    result.feasible = (budget == 0 || final_bytes <= budget) && result.achieved_passband_hz > 0.0;

    result.achieved_stopband_db = window_db;
    if (result.phase_mode == PhaseMode::INTERPOLATED) {
        result.achieved_stopband_db = std::min(window_db, interpolation_floor_db(phases));
    }

    result.phases = static_cast<int>(phases);
    result.filter_length = filter_length;
    result.filter_bank_bytes = final_bytes;

    // Populate the FilterDesign that MicroResampler consumes. The sinc
    // cutoff sits at the center of the transition band: the windowed-sinc
    // response is -6 dB at the cutoff and the transition spreads
    // symmetrically around it, so centering it puts the stopband edge at
    // the lower Nyquist. Both directional cutoffs get the same fraction
    // since each is expressed relative to that same lower Nyquist.
    const double cutoff_fraction =
        (result.achieved_passband_hz + stopband_hz) / (2.0 * stopband_hz);
    FilterDesign design = FilterDesign::from_quality(ResamplerQuality::MEDIUM);
    design.set_filter_length(filter_length);
    design.set_kaiser_beta(result.kaiser_beta);
    design.set_downsampling_cutoff_percent(cutoff_fraction);
    design.set_upsampling_cutoff_percent(cutoff_fraction);
    // The MicroResampler constructor takes min(GCD-simplified output rate,
    // default phase count), which lands on `phases` for both modes. ASRC
    // has no GCD structure to exploit, so force the exact count there.
    design.set_default_phase_count(result.phases);
    design.set_force_phase_count(constraints.asrc);
    result.design = design;

    return result;
}

bool precision_uses_beta_table(PrecisionStrategyType precision) {
    switch (precision) {
        case PrecisionStrategyType::TABLE_FLOAT:
        case PrecisionStrategyType::TABLE_DOUBLE:
            return true;
        case PrecisionStrategyType::QUAD:
#ifdef RESAMPLER_ENABLE_QUAD_PRECISION
            return false;
#else
            // Falls back to DefaultPrecisionStrategy in the coefficient
            // generator factory when quad support is not compiled in
            [[fallthrough]];
#endif
        case PrecisionStrategyType::DEFAULT:
            // Mirror the DefaultPrecisionStrategy alias rather than repeating
            // the platform conditional here
            return std::is_same_v<DefaultPrecisionStrategy, TableFloatStrategy> ||
                   std::is_same_v<DefaultPrecisionStrategy, TableDoubleStrategy>;
        case PrecisionStrategyType::SINGLE:
        case PrecisionStrategyType::DOUBLE:
            return false;
    }
    return false;  // Unreachable
}

}  // namespace micro_resampler
