// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 Kevin Ahrendt
//
// LPC-based sample extrapolation using Burg's method.
// Burg's algorithm computes optimal reflection coefficients directly from the signal,
// guaranteeing stable filter coefficients.

#include "detail/coefficient_ops.h"
#include "extrapolator/extrapolator.h"
#include "formats.h"
#include "memory_utils.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace micro_resampler {

// Fractional bits for Q15 fixed-point format; shared by the fixed-point Burg
// implementation below and the Q31<->Q15 conversion helper further down.
constexpr int32_t Q15_SHIFT = 15;

// ============================================================================
// Float Implementation Namespace
// ============================================================================

namespace burg_float {

// Compute LPC coefficients using Burg's method
// Burg's algorithm minimizes both forward and backward prediction errors
// and guarantees stable (minimum-phase) filter coefficients.
// error_scratch must hold 2n doubles (forward then backward prediction error).
static void burg_lpc(const float* x, double* lpc, int n, int order, double* error_scratch) {
    constexpr int MAX_ORDER = FloatExtrapolation::MAX_LPC_ORDER;

    if (n <= order) {
        for (int i = 0; i < order; i++) {
            lpc[i] = 0;
        }
        return;
    }

    double a[MAX_ORDER + 1] = {0};  // LPC coefficients (1, a1, a2, ...)
    double a_prev[MAX_ORDER + 1] = {0};
    double* ef = error_scratch;      // Forward prediction error
    double* eb = error_scratch + n;  // Backward prediction error

    // Initialize prediction errors with signal
    for (int i = 0; i < n; i++) {
        ef[i] = eb[i] = x[i];
    }

    a[0] = 1.0;

    for (int m = 1; m <= order; m++) {
        // Compute reflection coefficient
        double num = 0.0, den = 0.0;
        for (int j = m; j < n; j++) {
            num += ef[j] * eb[j - 1];
            den += ef[j] * ef[j] + eb[j - 1] * eb[j - 1];
        }

        double k = (den > 1e-10) ? (-2.0 * num / den) : 0.0;

        // Check stability (should always hold, but be safe)
        if (std::abs(k) >= 1.0) {
            k = (k > 0) ? 0.99 : -0.99;
        }

        // Update LPC coefficients from reflection coefficient
        for (int i = 0; i <= m; i++) {
            a_prev[i] = a[i];
        }

        a[m] = k;
        for (int i = 1; i < m; i++) {
            a[i] = a_prev[i] + k * a_prev[m - i];
        }

        // Update prediction errors
        for (int j = n - 1; j >= m; j--) {
            double ef_old = ef[j];
            ef[j] = ef_old + k * eb[j - 1];
            eb[j] = eb[j - 1] + k * ef_old;
        }
    }

    // Output coefficients with bandwidth expansion
    // Sign convention: predict_sample uses sum -= x[n-i] * lpc[i], so lpc should be positive
    double chirp = 1.0;
    for (int i = 0; i < order; i++) {
        chirp *= 1.0;  // FloatExtrapolation::CHIRP_FACTOR;
        lpc[i] = a[i + 1] * chirp;
    }
}

// Predict next sample using LPC coefficients
static inline float predict_sample(const float* x, const double* lpc, int order) {
    double sum = 0;
    for (int i = 0; i < order; i++) {
        sum -= x[-1 - i] * lpc[i];
    }
    return static_cast<float>(sum);
}

// Extrapolate forward using float arithmetic
// error_scratch must hold 2 * nvalues doubles.
static void extrapolate_forward(float* values, int nvalues, int num_to_extrapolate, int order,
                                double* error_scratch) {
    if (nvalues < order || num_to_extrapolate <= 0) {
        std::memset(values + nvalues, 0, num_to_extrapolate * sizeof(float));
        return;
    }

    double lpc[FloatExtrapolation::MAX_LPC_ORDER];
    burg_lpc(values, lpc, nvalues, order, error_scratch);

    for (int i = 0; i < num_to_extrapolate; i++) {
        float* pos = values + nvalues + i;
        *pos = predict_sample(pos, lpc, order);
    }
}

}  // namespace burg_float

// ============================================================================
// Fixed-Point Implementation Namespace
// ============================================================================

namespace burg_fixed {

// Fractional bits for the Q30 reflection coefficients
constexpr int K_SHIFT = 30;
// Q30 reflection coefficient saturation ceiling (just under 1.0)
constexpr int32_t K_MAX = (1 << K_SHIFT) - 1;
// Fractional bits for the Q28 LPC coefficients (range +/-8, enough for the
// order-4 cap: the binomial worst case peaks at 6). A two-pole tone predictor
// only oscillates while a1^2 < 4 * a2, a margin of 4 * sin^2(w) that is ~3 LSB
// at Q12 for a 100 Hz tone; at Q12 the truncation in the Levinson and error
// updates pushes bass poles onto the real axis just outside the unit circle,
// and the extension grows instead of ringing
constexpr int LPC_SHIFT = 28;
// Peak the analysis window is scaled up to before training (half of Q15 full
// scale, leaving headroom for prediction errors that exceed the signal)
constexpr int32_t NORMALIZE_PEAK = 16383;
// den is normalized below this before forming k, so |num| < 2^30 (Burg's
// |2 * num| <= den) and num << 31 cannot overflow 64 bits
constexpr int64_t K_DEN_LIMIT = int64_t{1} << 31;
// Extra fractional bits the prediction recursion carries between samples.
// Rounding every predicted sample to a Q15 integer before feeding it back
// injects error that the near-unit-circle poles of a low tone integrate:
// on quiet input the extension random-walks away from the tone
constexpr int PRED_FRAC_BITS = 12;
// Residual-to-signal energy ratio (~30 dB) below which the remaining den is the
// Q15 quantization noise of the ef/eb updates, not signal
constexpr int64_t DEN_NOISE_FLOOR_RATIO = 1024;
// Absolute noise floor per squared error term in den (Q30, i.e. 16 Q15 LSB^2):
// catches noise-fitting stages on low-level input, where the Q15 truncation
// residue sits above the relative ratio
constexpr int64_t DEN_NOISE_FLOOR_PER_TERM = 16;
// Stages always trained before the noise-floor guard applies: two stages are
// the minimal predictor for one sinusoid, and on a low tone the first stage
// alone already removes most of the energy (residual ~sin^2(w), under the
// relative ratio below ~240 Hz at 48 kHz) while the remainder is still signal
constexpr int MIN_GUARDED_STAGE = 3;

// Fixed-point Burg's method
// Uses Q30 format for intermediate calculations. T is the storage type for Q15-range
// values (int16_t, or int32_t for the Q31 paths); the prediction errors are clamped to
// the int16 range, so either type holds them losslessly.
//
// Runs in place without scratch allocation: x[0, n) is overwritten with the forward
// prediction error and x[n, 2n) holds the backward prediction error, so the caller must
// provide 2n writable values and treat all of them as clobbered.
template <typename T>
static int burg_lpc_fixed(T* x, int32_t* lpc, int n, int order) {
    constexpr int MAX_ORDER = FixedExtrapolation::MAX_LPC_ORDER;

    if (n <= order) {
        for (int i = 0; i < order; i++) {
            lpc[i] = 0;
        }
        return 0;
    }

    // Scale the window up so its peak sits near half scale. The prediction
    // errors are stored as Q15 integers, so on a quiet window the later
    // stages would otherwise fit residuals only a few LSB tall, where rounding
    // swamps the signal (a -40 dBFS bass tone's stage-1 residual is ~3 LSB)
    // and the fitted poles land on the real axis: the extension ramps away
    // instead of ringing. LPC coefficients are scale invariant, so only the
    // absolute noise floor below needs to follow the gain
    int32_t peak = 0;
    for (int i = 0; i < n; i++) {
        peak = std::max(peak, std::abs(static_cast<int32_t>(x[i])));
    }
    int gain_shift = 0;
    if (peak > 0) {
        while ((peak << (gain_shift + 1)) <= NORMALIZE_PEAK) {
            gain_shift++;
        }
    }
    if (gain_shift > 0) {
        for (int i = 0; i < n; i++) {
            x[i] = static_cast<T>(static_cast<int32_t>(x[i]) * (1 << gain_shift));
        }
    }
    const int64_t noise_floor_per_term = DEN_NOISE_FLOOR_PER_TERM << (2 * gain_shift);

    // Prediction errors (Q15); ef starts as the signal already in place
    T* ef = x;
    T* eb = x + n;
    std::copy(x, x + n, eb);

    // LPC coefficients in Q28
    int32_t a[MAX_ORDER + 1] = {0};
    int32_t a_prev[MAX_ORDER + 1] = {0};

    int effective_order = order;
    int64_t den0 = 0;

    for (int m = 1; m <= order; m++) {
        // Compute reflection coefficient in Q30
        int64_t num = 0, den = 0;
        for (int j = m; j < n; j++) {
            num += static_cast<int64_t>(ef[j]) * eb[j - 1];  // Q30
            den += static_cast<int64_t>(ef[j]) * ef[j] +
                   static_cast<int64_t>(eb[j - 1]) * eb[j - 1];  // Q30
        }

        // Past the first two stages, stop once the residual energy has fallen
        // ~30 dB below the signal's or to a few LSB per sample: whatever is
        // left at that point is the Q15 quantization noise of the ef/eb
        // updates, not signal. On highly predictable input (e.g. a pure tone)
        // the later stages would otherwise fit that noise with large |k|,
        // wrecking the near-unit-circle poles: depending on the tone's phase
        // the extrapolation either decays to a few percent of signal level
        // within a couple hundred samples or grows past it. The unreached
        // coefficients simply stay zero - an effective order of m - 1.
        if (m == 1) {
            den0 = den;
        }
        const int64_t den_terms = static_cast<int64_t>(2) * (n - m);
        if (m >= MIN_GUARDED_STAGE &&
            (den < den0 / DEN_NOISE_FLOOR_RATIO || den <= den_terms * noise_floor_per_term)) {
            effective_order = m - 1;
            break;
        }

        int32_t k = 0;  // Reflection coefficient in Q30
        if (den > 0) {
            // k = -2 * num / den in Q30. Scale num and den down together
            // until num << 31 fits in 64 bits; the ratio is unchanged
            while (den >= K_DEN_LIMIT) {
                num >>= 1;
                den >>= 1;
            }
            int64_t k64 = (-num * (int64_t{1} << (K_SHIFT + 1))) / den;
            k = static_cast<int32_t>(
                std::clamp(k64, static_cast<int64_t>(-K_MAX), static_cast<int64_t>(K_MAX)));
        }

        // Update LPC coefficients from reflection coefficient
        for (int i = 0; i <= m; i++) {
            a_prev[i] = a[i];
        }

        a[m] = k >> (K_SHIFT - LPC_SHIFT);
        for (int i = 1; i < m; i++) {
            // a[i] + k * a_prev[m-i] in Q28 (k is Q30)
            int64_t prod = (static_cast<int64_t>(k) * a_prev[m - i]) >> K_SHIFT;
            a[i] =
                static_cast<int32_t>(std::clamp(a_prev[i] + prod, static_cast<int64_t>(-INT32_MAX),
                                                static_cast<int64_t>(INT32_MAX)));
        }

        // Update prediction errors
        for (int j = n - 1; j >= m; j--) {
            int32_t ef_old = ef[j];
            // ef[j] + k * eb[j-1] in Q15 (k is Q30)
            int32_t ef_new =
                ef_old + static_cast<int32_t>((static_cast<int64_t>(k) * eb[j - 1]) >> K_SHIFT);
            int32_t eb_new =
                eb[j - 1] + static_cast<int32_t>((static_cast<int64_t>(k) * ef_old) >> K_SHIFT);
            ef[j] = static_cast<T>(
                std::clamp(ef_new, static_cast<int32_t>(-32768), static_cast<int32_t>(INT16_MAX)));
            eb[j] = static_cast<T>(
                std::clamp(eb_new, static_cast<int32_t>(-32768), static_cast<int32_t>(INT16_MAX)));
        }
    }

    // Output coefficients with bandwidth expansion (chirp)
    // Sign convention: predict_sample_fixed uses sum -= x[n-i] * lpc[i], so lpc should be positive
    for (int i = 0; i < order; i++) {
        lpc[i] = static_cast<int32_t>(
            (static_cast<int64_t>(a[i + 1]) * FixedExtrapolation::CHIRP_POWERS[i]) >> Q15_SHIFT);
    }

    return effective_order;
}

// Predict the next sample from history[0, order) (most recent first), all in
// Q15 with PRED_FRAC_BITS extra fractional bits
static inline int32_t predict_sample_fixed(const int32_t* history, const int32_t* lpc, int order) {
    int64_t sum = 0;

    for (int i = 0; i < order; i++) {
        sum -= static_cast<int64_t>(history[i]) * lpc[i];
    }

    int64_t result = (sum + (int64_t{1} << (LPC_SHIFT - 1))) >> LPC_SHIFT;
    constexpr int64_t PRED_MIN = static_cast<int64_t>(INT16_MIN) * (1 << PRED_FRAC_BITS);
    constexpr int64_t PRED_MAX = static_cast<int64_t>(INT16_MAX) * (1 << PRED_FRAC_BITS);
    return static_cast<int32_t>(std::clamp(result, PRED_MIN, PRED_MAX));
}

// Extrapolate forward using fixed-point arithmetic
// values[0, nvalues) holds Q15-range input and is clobbered; values[nvalues,
// nvalues + num_to_extrapolate) receives the prediction and doubles as Burg's backward
// error scratch, so the analysis window is capped at num_to_extrapolate samples.
template <typename T>
static void extrapolate_forward(T* values, int nvalues, int num_to_extrapolate, int order) {
    if (nvalues < order || num_to_extrapolate <= 0) {
        std::memset(values + nvalues, 0, num_to_extrapolate * sizeof(T));
        return;
    }

    // Analyze the most recent samples that fit in the output region's scratch space. Both
    // current callers extrapolate at least as many samples as they analyze; the min() only
    // keeps a future caller that doesn't memory-safe.
    assert(nvalues <= num_to_extrapolate);
    int analysis_len = std::min(nvalues, num_to_extrapolate);
    T* analysis = values + (nvalues - analysis_len);

    // Burg overwrites the input in place, so keep the samples the predictor starts from
    T seed[FixedExtrapolation::MAX_LPC_ORDER];
    std::copy(values + nvalues - order, values + nvalues, seed);

    int32_t lpc[FixedExtrapolation::MAX_LPC_ORDER];
    int actual_order = burg_lpc_fixed(analysis, lpc, analysis_len, order);

    std::copy(seed, seed + order, values + nvalues - order);

    if (actual_order == 0) {
        std::memset(values + nvalues, 0, num_to_extrapolate * sizeof(T));
        return;
    }

    // Run the recursion on a high-precision copy of the latest samples and
    // round only the values written out
    int32_t history[FixedExtrapolation::MAX_LPC_ORDER];
    for (int i = 0; i < actual_order; i++) {
        history[i] = static_cast<int32_t>(values[nvalues - 1 - i]) * (1 << PRED_FRAC_BITS);
    }
    for (int i = 0; i < num_to_extrapolate; i++) {
        int32_t next = predict_sample_fixed(history, lpc, actual_order);
        for (int j = actual_order - 1; j > 0; j--) {
            history[j] = history[j - 1];
        }
        history[0] = next;
        values[nvalues + i] =
            static_cast<T>((next + (1 << (PRED_FRAC_BITS - 1))) >> PRED_FRAC_BITS);
    }
}

}  // namespace burg_fixed

// ============================================================================
// BurgLPCExtrapolator Implementation
// ============================================================================

// Helper: extrapolate a single channel forward
// Only values[nvalues, nvalues + num_to_extrapolate) is meaningful on return; the fixed-point
// paths use the input region as scratch. The float path needs conversion_buffer
// (nvalues + num_to_extrapolate floats) and error_buffer (2 * nvalues doubles).
template <typename CoeffFormat>
static void extrapolate_channel_forward(typename CoeffFormat::sample_t* values, int nvalues,
                                        int num_to_extrapolate, int lpc_order,
                                        float* conversion_buffer, double* error_buffer) {
    using sample_t = typename CoeffFormat::sample_t;

    // Use fixed-point Burg for int16_t samples (Q15Format)
    if constexpr (std::is_same_v<sample_t, int16_t>) {
        int actual_order = std::min(lpc_order, FixedExtrapolation::DEFAULT_LPC_ORDER);
        burg_fixed::extrapolate_forward(values, nvalues, num_to_extrapolate, actual_order);
    }
    // Use fixed-point Burg for int32_t samples (Q31Format), in place at Q15 scale
    else if constexpr (std::is_same_v<sample_t, int32_t>) {
        // Convert Q31 to Q15 (round in 64 bits so near-full-scale positives don't wrap negative)
        for (int i = 0; i < nvalues; ++i) {
            int64_t rounded = static_cast<int64_t>(values[i]) + (1 << Q15_SHIFT);
            auto shifted = static_cast<int32_t>(rounded >> 16);
            if (shifted > INT16_MAX) {
                shifted = INT16_MAX;
            }
            if (shifted < -32768) {
                shifted = -32768;
            }
            values[i] = shifted;
        }

        int actual_order = std::min(lpc_order, FixedExtrapolation::DEFAULT_LPC_ORDER);
        burg_fixed::extrapolate_forward(values, nvalues, num_to_extrapolate, actual_order);

        // Convert back to Q31 (multiply rather than shift: left-shifting a negative is UB)
        for (int i = nvalues; i < nvalues + num_to_extrapolate; ++i) {
            values[i] = values[i] * (1 << 16);
        }
    } else {
        // Float path
        for (int i = 0; i < nvalues; ++i) {
            conversion_buffer[i] = detail::FormatOps<CoeffFormat>::sample_to_float(values[i]);
        }

        burg_float::extrapolate_forward(conversion_buffer, nvalues, num_to_extrapolate, lpc_order,
                                        error_buffer);

        for (int i = 0; i < nvalues + num_to_extrapolate; ++i) {
            values[i] = detail::FormatOps<CoeffFormat>::sample_from_float(conversion_buffer[i]);
        }
    }
}

template <typename CoeffFormat>
BurgLPCExtrapolator<CoeffFormat>::~BurgLPCExtrapolator() {
    this->free_buffers();
}

template <typename CoeffFormat>
bool BurgLPCExtrapolator<CoeffFormat>::allocate(int filter_length) {
    if (filter_length <= this->allocated_filter_length_) {
        return true;
    }

    this->free_buffers();

    // Prime needs up to filter_length input + filter_length extrapolated samples; flush less
    size_t scratch_size = 2 * static_cast<size_t>(filter_length);

    this->channel_buffer_ = MemoryUtils::allocate<sample_t>(scratch_size);
    bool allocated = this->channel_buffer_ != nullptr;

    if constexpr (std::is_floating_point_v<sample_t>) {
        this->conversion_buffer_ = MemoryUtils::allocate<float>(scratch_size);
        this->error_buffer_ = MemoryUtils::allocate<double>(scratch_size);
        allocated =
            allocated && this->conversion_buffer_ != nullptr && this->error_buffer_ != nullptr;
    }

    if (!allocated) {
        this->free_buffers();
        return false;
    }

    this->allocated_filter_length_ = filter_length;
    return true;
}

template <typename CoeffFormat>
void BurgLPCExtrapolator<CoeffFormat>::prime_buffer(sample_t* history_buffer, int num_channels,
                                                    const sample_t* initial_samples,
                                                    int available_frames, int filter_length) {
    int frames_to_use = std::min(available_frames, filter_length);

    // Handle insufficient samples for LPC
    if (frames_to_use < this->lpc_order_) {
        MirrorExtrapolator<CoeffFormat> mirror;
        mirror.prime_buffer(history_buffer, num_channels, initial_samples, available_frames,
                            filter_length);
        return;
    }

    if (frames_to_use <= 0) {
        return;
    }

    // No-op when MicroResampler::initialize() already allocated for this filter length
    if (!this->allocate(filter_length)) {
        return;
    }

    // Fill the entire pre-history region [0, filter_length). Leaving positions
    // unwritten (as prior `extrapolate_frames = frames_to_use` did when the input
    // was shorter than the filter) left zeros in the filter window at startup,
    // destroying gapless splice alignment for long filters on short inputs.
    int extrapolate_frames = filter_length;

    int dst_offset = 0;

    for (int ch = 0; ch < num_channels; ++ch) {
        // Extract channel data (reversed for backward extrapolation)
        for (int i = 0; i < frames_to_use; ++i) {
            this->channel_buffer_[i] = initial_samples[(frames_to_use - 1 - i) * num_channels + ch];
        }

        extrapolate_channel_forward<CoeffFormat>(this->channel_buffer_, frames_to_use,
                                                 extrapolate_frames, this->lpc_order_,
                                                 this->conversion_buffer_, this->error_buffer_);

        // Copy extrapolated samples (reverse them back)
        for (int j = 0; j < extrapolate_frames; ++j) {
            sample_t extrapolated_sample =
                this->channel_buffer_[frames_to_use + extrapolate_frames - 1 - j];
            int dst_frame = dst_offset + j;
            history_buffer[dst_frame * num_channels + ch] = extrapolated_sample;
        }
    }
}

template <typename CoeffFormat>
void BurgLPCExtrapolator<CoeffFormat>::prepare_flush(sample_t* history_buffer, int num_channels,
                                                     int filter_length) {
    int extrapolate_frames = filter_length / 2 + 1;
    int samples_for_lpc = filter_length / 2;
    int history_offset = filter_length - samples_for_lpc;

    // No-op when MicroResampler::initialize() already allocated for this filter length
    if (!this->allocate(filter_length)) {
        return;
    }

    for (int ch = 0; ch < num_channels; ++ch) {
        for (int i = 0; i < samples_for_lpc; ++i) {
            this->channel_buffer_[i] = history_buffer[(history_offset + i) * num_channels + ch];
        }

        extrapolate_channel_forward<CoeffFormat>(this->channel_buffer_, samples_for_lpc,
                                                 extrapolate_frames, this->lpc_order_,
                                                 this->conversion_buffer_, this->error_buffer_);

        for (int j = 0; j < extrapolate_frames; ++j) {
            int dst_frame = filter_length + j;
            history_buffer[dst_frame * num_channels + ch] =
                this->channel_buffer_[samples_for_lpc + j];
        }
    }
}

template <typename CoeffFormat>
void BurgLPCExtrapolator<CoeffFormat>::free_buffers() {
    MemoryUtils::free(this->channel_buffer_);
    MemoryUtils::free(this->conversion_buffer_);
    MemoryUtils::free(this->error_buffer_);
    this->channel_buffer_ = nullptr;
    this->conversion_buffer_ = nullptr;
    this->error_buffer_ = nullptr;
    this->allocated_filter_length_ = 0;
}

// ============================================================================
// Explicit Template Instantiations
// ============================================================================

template class BurgLPCExtrapolator<Q15Format>;
template class BurgLPCExtrapolator<Q31Format>;
template class BurgLPCExtrapolator<Q31LiteFormat>;
template class BurgLPCExtrapolator<Float64Format>;
template class BurgLPCExtrapolator<Float32Format>;

}  // namespace micro_resampler
