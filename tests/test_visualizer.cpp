// Copyright 2026 Sendspin Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file test_visualizer.cpp
/// @brief The visualizer role's pure parts: the configuration the server derives from a request
/// and its key, the dB scale, A-weighting, the spectrum's display bins, the analysis instants and
/// window, and the features on synthetic signals and against aiosendspin's on a fixed one.

#include "fft.h"
#include "visualizer.h"
#include "visualizer_analysis.h"
#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using sendspin::server::RealFft;
using sendspin::server::SpectrumConfig;
using sendspin::server::SpectrumScale;
using sendspin::server::VisualizerAnalysis;
using sendspin::server::VisualizerConfig;
using sendspin::server::VisualizerRequest;
namespace type = sendspin::server::visualizer_type;

constexpr uint32_t RATE = 48000;
constexpr size_t WINDOW = 2048;
/// A frequency at the center of an FFT bin of the window at RATE: bin 44
constexpr double BIN_CENTERED_HZ = 44.0 * RATE / WINDOW;

// ==============================================================================
// Negotiation and the configuration key
// ==============================================================================

const SpectrumConfig MEL32{32, SpectrumScale::MEL, 40, 16000};

// spec roles/visualizer/v1.md "stream/start visualizer object": types the server cannot stream are
// omitted, among them any whose message is larger than the client's buffer_capacity ("client/hello
// visualizer@v1 support object"), the rate never exceeds the request's, and the spectrum is echoed
// exactly
TEST(VisualizerNegotiation, KeepsWhatTheServerCanStream) {
    struct Row {
        const char* name;
        VisualizerRequest request;
        VisualizerConfig expected;
        uint32_t capacity{100000};
    };
    const uint8_t all = type::LOUDNESS | type::F_PEAK | type::SPECTRUM | type::PEAK;
    const std::vector<Row> rows = {
        {"Control: every streamable type, the spectrum echoed", {all, 30, MEL32}, {all, 30, MEL32}},
        {"beat dropped",
         {type::BEAT | type::PEAK, 30, std::nullopt},
         {type::PEAK, 30, std::nullopt}},
        {"rate above the cap",
         {type::LOUDNESS, 120, std::nullopt},
         {type::LOUDNESS, sendspin::server::MAX_VISUALIZER_RATE, std::nullopt}},
        {"Control: rate at the cap",
         {type::LOUDNESS, 50, std::nullopt},
         {type::LOUDNESS, 50, std::nullopt}},
        {"bins above the cap",
         {type::SPECTRUM | type::PEAK, 30, SpectrumConfig{257, SpectrumScale::LIN, 0, 8000}},
         {type::PEAK, 30, std::nullopt}},
        {"Control: bins at the cap",
         {type::SPECTRUM, 30, SpectrumConfig{256, SpectrumScale::LIN, 0, 8000}},
         {type::SPECTRUM, 30, SpectrumConfig{256, SpectrumScale::LIN, 0, 8000}}},
        {"no bins",
         {type::SPECTRUM, 30, SpectrumConfig{0, SpectrumScale::LIN, 0, 8000}},
         {0, 30, std::nullopt}},
        {"an empty range",
         {type::SPECTRUM, 30, SpectrumConfig{8, SpectrumScale::LOG, 500, 500}},
         {0, 30, std::nullopt}},
        {"a bound past the cap",
         {type::SPECTRUM, 30, SpectrumConfig{8, SpectrumScale::LOG, 20, (1U << 20)}},
         {0, 30, std::nullopt}},
        {"a spectrum object it cannot read (an unknown scale)",
         {type::LOUDNESS | type::SPECTRUM, 30, std::nullopt, true},
         {type::LOUDNESS, 30, std::nullopt}},
        {"a spectrum object without the type",
         {type::LOUDNESS, 30, MEL32},
         {type::LOUDNESS, 30, std::nullopt}},
        {"a spectrum message larger than the capacity",
         {type::LOUDNESS | type::SPECTRUM, 30, SpectrumConfig{256, SpectrumScale::LIN, 0, 8000}},
         {type::LOUDNESS, 30, std::nullopt},
         520},
        {"Control: a spectrum message the capacity holds exactly",
         {type::LOUDNESS | type::SPECTRUM, 30, SpectrumConfig{256, SpectrumScale::LIN, 0, 8000}},
         {type::LOUDNESS | type::SPECTRUM, 30, SpectrumConfig{256, SpectrumScale::LIN, 0, 8000}},
         521},
        {"a capacity under f_peak's 13 bytes keeps loudness's 11 and peak's 10",
         {all, 30, MEL32},
         {type::LOUDNESS | type::PEAK, 30, std::nullopt},
         12},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const VisualizerConfig config =
            sendspin::server::negotiate_visualizer(row.request, row.capacity);
        EXPECT_EQ(config.types, row.expected.types);
        EXPECT_EQ(config.rate, row.expected.rate);
        EXPECT_EQ(config.spectrum, row.expected.spectrum);
    }
}

// A marker compares configurations by key (sched::FormatKey): equal keys exactly for equal
// configurations, each field told apart, and never a player's 0
TEST(VisualizerNegotiation, KeysEqualConfigurationsAlike) {
    const VisualizerConfig base{type::SPECTRUM | type::PEAK, 30,
                                SpectrumConfig{64, SpectrumScale::LOG, 20, 20000}};
    std::vector<VisualizerConfig> others;
    VisualizerConfig changed = base;
    changed.types = type::SPECTRUM;
    others.push_back(changed);
    changed = base;
    changed.rate = 29;
    others.push_back(changed);
    for (const SpectrumConfig& spectrum : {SpectrumConfig{63, SpectrumScale::LOG, 20, 20000},
                                           SpectrumConfig{64, SpectrumScale::MEL, 20, 20000},
                                           SpectrumConfig{64, SpectrumScale::LOG, 21, 20000},
                                           SpectrumConfig{64, SpectrumScale::LOG, 20, 20001}}) {
        changed = base;
        changed.spectrum = spectrum;
        others.push_back(changed);
    }
    const uint64_t key = sendspin::server::config_key(base);
    EXPECT_NE(key, 0U);
    EXPECT_NE(sendspin::server::config_key(VisualizerConfig{}), 0U);
    // Control: an equal configuration
    EXPECT_EQ(sendspin::server::config_key(VisualizerConfig{base}), key);
    for (const VisualizerConfig& other : others) {
        EXPECT_NE(sendspin::server::config_key(other), key);
    }
}

// ==============================================================================
// Scales, bins, instants and the window
// ==============================================================================

// spec roles/visualizer/v1.md "Visualization Data (Binary)": -60 dB is 0, 0 dB is 65535, linear in
// dB across that range
TEST(VisualizerScale, MapsDecibelsOntoTheUint16Range) {
    struct Row {
        const char* name;
        double db;
        uint16_t expected;
    };
    const std::vector<Row> rows = {
        {"Control: full scale", 0.0, 65535}, {"the floor", -60.0, 0},
        {"halfway", -30.0, 32767},           {"-6 dB", -6.0, 58981},
        {"below the floor", -80.0, 0},       {"above full scale", 6.0, 65535},
    };
    constexpr float REFERENCE = 512.0F;
    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);
        const auto value = static_cast<float>(REFERENCE * std::pow(10.0, row.db / 20.0));
        const uint16_t mapped =
            sendspin::server::unit_to_u16(sendspin::server::db_unit(value, REFERENCE));
        EXPECT_NEAR(mapped, row.expected, 1);
    }
    EXPECT_EQ(sendspin::server::unit_to_u16(sendspin::server::db_unit(0.0F, REFERENCE)), 0);
}

// IEC 61672-1 A-weighting, normalized to 0 dB at 1 kHz, clamped below at -50 dB
TEST(VisualizerScale, AWeightsByTheStandardCurve) {
    struct Row {
        float hz;
        double expected_db;
    };
    const std::vector<Row> rows = {
        {1000.0F, 0.0}, {100.0F, -19.1}, {10000.0F, -2.5}, {2500.0F, 1.3},
        {50.0F, -30.2}, {20.0F, -50.0},  {0.0F, -50.0},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.hz);
        EXPECT_NEAR(sendspin::server::a_weighting_db(row.hz), row.expected_db, 0.1);
    }
}

/// The edges as the spec defines each scale, computed here in double precision
std::vector<double> expected_edges(const SpectrumConfig& spectrum) {
    const auto mel = [](double hz) { return 2595.0 * std::log10(1.0 + (hz / 700.0)); };
    const auto hz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
    std::vector<double> edges;
    for (uint32_t i = 0; i <= spectrum.n_disp_bins; ++i) {
        const double fraction = static_cast<double>(i) / spectrum.n_disp_bins;
        const double low = spectrum.f_min;
        const double high = spectrum.f_max;
        switch (spectrum.scale) {
            case SpectrumScale::LIN:
                edges.push_back(low + ((high - low) * fraction));
                break;
            case SpectrumScale::LOG: {
                const double from = std::log10(std::max(low, 1.0));
                edges.push_back(std::pow(10.0, from + ((std::log10(high) - from) * fraction)));
                break;
            }
            case SpectrumScale::MEL:
                edges.push_back(hz(mel(low) + ((mel(high) - mel(low)) * fraction)));
                break;
        }
    }
    return edges;
}

// spec roles/visualizer/v1.md "client/state visualizer object": 'mel' the HTK mel formula, 'log'
// base-10 logarithm of frequency, 'lin' linear spacing, from f_min to f_max
TEST(VisualizerBins, SpaceTheEdgesByEachScale) {
    const std::vector<SpectrumConfig> rows = {
        {4, SpectrumScale::LIN, 0, 4000},
        {3, SpectrumScale::LOG, 10, 10000},
        {2, SpectrumScale::MEL, 0, 1000},
        {16, SpectrumScale::MEL, 40, 16000},
        // A log scale from 0 Hz starts at 1 Hz
        {4, SpectrumScale::LOG, 0, 10000},
    };
    for (const SpectrumConfig& spectrum : rows) {
        SCOPED_TRACE(static_cast<int>(spectrum.scale));
        std::vector<float> edges(spectrum.n_disp_bins + 1);
        sendspin::server::spectrum_edges(spectrum, edges);
        const std::vector<double> expected = expected_edges(spectrum);
        for (size_t i = 0; i < edges.size(); ++i) {
            EXPECT_NEAR(edges[i], expected[i], 1e-4 * std::max(1.0, expected[i])) << i;
        }
    }
    // The mel midpoint of 0 to 1 kHz, against its value
    std::vector<float> edges(3);
    sendspin::server::spectrum_edges({2, SpectrumScale::MEL, 0, 1000}, edges);
    EXPECT_NEAR(edges[1], 390.9, 0.1);
}

// A display bin holds its lower edge and not its upper; frequencies outside the range have none
TEST(VisualizerBins, PlaceAFrequencyInTheBinHoldingIt) {
    const std::vector<float> edges = {0.0F, 1000.0F, 2000.0F};
    struct Row {
        float hz;
        int32_t bin;
    };
    for (const Row& row : std::vector<Row>{
             {0.0F, 0}, {999.9F, 0}, {1000.0F, 1}, {1999.0F, 1}, {2000.0F, -1}, {-1.0F, -1}}) {
        EXPECT_EQ(sendspin::server::spectrum_bin(row.hz, edges), row.bin) << row.hz;
    }
}

// Instants `floor(j * 10^6 / rate)` from T0, at most one per cell
TEST(VisualizerInstants, FallOnTheRatesGridOncePerCellAtMost) {
    struct Row {
        int64_t cell;
        uint32_t rate;
        std::optional<int64_t> expected;
    };
    const std::vector<Row> rows = {
        {0, 50, 0},
        {1, 50, 20000},
        {0, 30, 0},
        {1, 30, 33333},
        {2, 30, std::nullopt},
        {3, 30, 66666},
        {4, 30, std::nullopt},
        {5, 30, 100000},
        {49, 1, std::nullopt},
        {50, 1, 1000000},
        {0, 0, std::nullopt},
    };
    for (const Row& row : rows) {
        EXPECT_EQ(sendspin::server::periodic_instant_us(row.cell, row.rate), row.expected)
            << row.cell << " at " << row.rate;
    }
    // Every instant of a rate, and only those, over a second of cells
    for (const uint32_t rate : {1U, 7U, 24U, 30U, 49U, 50U}) {
        int64_t count = 0;
        for (int64_t cell = 0; cell < 50; ++cell) {
            if (const std::optional<int64_t> at =
                    sendspin::server::periodic_instant_us(cell, rate)) {
                EXPECT_EQ(*at, (count * 1000000) / rate) << rate;
                ++count;
            }
        }
        EXPECT_EQ(count, rate);
    }
}

// ==============================================================================
// The FFT seam
// ==============================================================================

// fft.h's contract, which every backend meets: the powers of two from 256 to 8192, and |X[k]|^2 of
// the unnormalized DFT, so a constant 1 gives N^2 at bin 0, a cosine of amplitude 1 on bin k gives
// (N / 2)^2 there and nothing elsewhere, and an alternating +1, -1 gives N^2 at bin N / 2. Checked
// as magnitudes, the power's square root, against the magnitude each row expects
TEST(VisualizerFft, MeetsTheSeamsContract) {
    for (const size_t size : {size_t{256}, size_t{2048}, size_t{8192}}) {
        SCOPED_TRACE(size);
        const std::unique_ptr<RealFft> fft = RealFft::create(size);
        ASSERT_TRUE(fft);
        ASSERT_EQ(fft->size(), size);
        ASSERT_EQ(fft->input().size(), size);
        const auto n = static_cast<float>(size);
        // Single precision over up to 8192 points: within 1e-4 of N
        const float tolerance = n * 1e-4F;
        struct Row {
            const char* name;
            size_t bin;
            float expected;
        };
        const size_t k = size / 8;
        for (const Row& row :
             {Row{"DC", 0, n}, Row{"a cosine on bin k", k, n / 2}, Row{"Nyquist", size / 2, n}}) {
            SCOPED_TRACE(row.name);
            const std::span<float> input = fft->input();
            for (size_t i = 0; i < size; ++i) {
                const double phase = 2.0 * std::numbers::pi * static_cast<double>(row.bin * i) /
                                     static_cast<double>(size);
                input[i] = static_cast<float>(std::cos(phase));
            }
            const std::span<const float> power = fft->power();
            ASSERT_EQ(power.size(), (size / 2) + 1);
            for (size_t bin = 0; bin < power.size(); ++bin) {
                ASSERT_GE(power[bin], 0.0F) << bin;
                EXPECT_NEAR(std::sqrt(power[bin]), bin == row.bin ? row.expected : 0.0F, tolerance)
                    << bin;
            }
        }
    }
    // Control: sizes outside the supported ones
    for (const size_t size : {size_t{128}, size_t{384}, size_t{16384}}) {
        EXPECT_FALSE(RealFft::create(size)) << size;
    }
}

// The window spans about 43 ms at any rate: 2048 samples at 48 kHz scaled, a power of two within
// the FFT's sizes
TEST(VisualizerWindow, ScalesWithTheInputRate) {
    struct Row {
        uint32_t rate;
        size_t window;
    };
    for (const Row& row : std::vector<Row>{{48000, 2048},
                                           {44100, 2048},
                                           {24000, 1024},
                                           {96000, 4096},
                                           {8000, 512},
                                           {11025, 512},
                                           {192000, 8192},
                                           {384000, 8192}}) {
        EXPECT_EQ(sendspin::server::analysis_window(row.rate), row.window) << row.rate;
    }
}

// ==============================================================================
// Features
// ==============================================================================

/// The messages one analysis appends, decoded
struct Decoded {
    std::optional<uint16_t> loudness;
    std::optional<std::pair<uint16_t, uint16_t>> f_peak;
    std::optional<std::vector<uint16_t>> spectrum;
    std::optional<uint8_t> peak;
    std::vector<int64_t> timestamps;
};

uint16_t u16_at(const std::vector<uint8_t>& bytes, size_t at) {
    return static_cast<uint16_t>((bytes[at] << 8) | bytes[at + 1]);
}

Decoded decode(const std::vector<uint8_t>& bytes, uint32_t bins) {
    Decoded out;
    size_t at = 0;
    while (at < bytes.size()) {
        const uint8_t id = bytes[at];
        int64_t ts = 0;
        for (size_t i = 1; i < 9; ++i) {
            ts = (ts << 8) | bytes[at + i];
        }
        out.timestamps.push_back(ts);
        const size_t data = at + 9;
        switch (id) {
            case 16:
                out.loudness = u16_at(bytes, data);
                at = data + 2;
                break;
            case 18:
                out.f_peak = {u16_at(bytes, data), u16_at(bytes, data + 2)};
                at = data + 4;
                break;
            case 19: {
                std::vector<uint16_t> values;
                for (uint32_t i = 0; i < bins; ++i) {
                    values.push_back(u16_at(bytes, data + (2 * size_t{i})));
                }
                out.spectrum = values;
                at = data + (2 * size_t{bins});
                break;
            }
            case 20:
                out.peak = bytes[data];
                at = data + 1;
                break;
            default:
                ADD_FAILURE() << "message type " << int{id};
                return out;
        }
    }
    return out;
}

/// A sine at hz with amplitude, WINDOW samples at RATE
std::vector<float> sine(double hz, double amplitude, size_t count = WINDOW) {
    std::vector<float> samples(count);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / RATE));
    }
    return samples;
}

/// The uint16 value of level dB with the A-weighting at hz, computed here in double precision
double expected_value(double level_db, double hz) {
    const double f2 = hz * hz;
    const double ra =
        (12194.0 * 12194.0 * f2 * f2) /
        ((f2 + (20.6 * 20.6)) * std::sqrt((f2 + (107.7 * 107.7)) * (f2 + (737.9 * 737.9))) *
         (f2 + (12194.0 * 12194.0)));
    const double db = level_db + 2.0 + (20.0 * std::log10(ra));
    return std::clamp((db + 60.0) / 60.0, 0.0, 1.0) * 65535.0;
}

/// The symmetric Hann window's DFT at m bins from DC, in double precision: a sine of amplitude a
/// centered on a bin puts `a / 2 * |W(m)|` into the bin m away
double window_dft(int64_t m) {
    std::complex<double> sum;
    for (size_t i = 0; i < WINDOW; ++i) {
        const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                               static_cast<double>(WINDOW - 1)));
        const double phase = -2.0 * std::numbers::pi * static_cast<double>(m) *
                             static_cast<double>(i) / static_cast<double>(WINDOW);
        sum += w * std::polar(1.0, phase);
    }
    return std::abs(sum);
}

/// The dB a bin-centered sine's bins m_from to m_to around it hold, against the full-scale
/// reference of a quarter window the features use
double window_db(int64_t m_from, int64_t m_to) {
    double power = 0.0;
    for (int64_t m = m_from; m <= m_to; ++m) {
        power += window_dft(m) * window_dft(m);
    }
    return 20.0 * std::log10(2.0 * std::sqrt(power) / static_cast<double>(WINDOW));
}

/// The dB the window's power stands from the periodic Hann's, 3 N / 8, which loudness's full-scale
/// reference assumes
double window_power_db() {
    double power = 0.0;
    for (size_t i = 0; i < WINDOW; ++i) {
        const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) /
                                               static_cast<double>(WINDOW - 1)));
        power += w * w;
    }
    return 10.0 * std::log10(power / (3.0 * WINDOW / 8.0));
}

class VisualizerFeatures : public testing::Test {
protected:
    /// An analysis of config at RATE whose window holds samples last
    VisualizerAnalysis analysis(const VisualizerConfig& config) {
        VisualizerAnalysis made(config);
        EXPECT_TRUE(made.set_rate(RATE));
        EXPECT_EQ(made.window(), WINDOW);
        return made;
    }

    static Decoded run(VisualizerAnalysis& analysis, const std::vector<float>& samples, int64_t ts,
                       bool periodic = true, bool onset = false) {
        analysis.push(samples);
        std::vector<uint8_t> bytes;
        analysis.analyze(ts, periodic, onset, bytes);
        const VisualizerConfig& config = analysis.config();
        return decode(bytes, config.spectrum ? config.spectrum->n_disp_bins : 0);
    }

    const uint8_t periodic_ = type::LOUDNESS | type::F_PEAK;
};

// A sine on a bin's center: f_peak reports its frequency, and loudness and the f_peak amplitude
// its level A-weighted on the spec's scale; a full-scale sine is 0 dB through a periodic Hann
// window. Against the symmetric window's exact peak bin, computed here in double precision, the
// amplitude lands within a count: the scale truncates, and single precision adds less. Loudness,
// against the window's exact power, lands within 2: its side-lobe bins carry their own A-weighting
// gain, for which the center bin's stands here (about 0.0002 dB)
TEST_F(VisualizerFeatures, MeasureASineAtItsFrequencyAndLevel) {
    const double power_db = window_power_db();
    const double peak_db = window_db(0, 0);
    for (const double level_db : {0.0, -6.0206, -24.0}) {
        SCOPED_TRACE(level_db);
        VisualizerAnalysis analysis = this->analysis({this->periodic_, 30, std::nullopt});
        const Decoded decoded =
            run(analysis, sine(BIN_CENTERED_HZ, std::pow(10.0, level_db / 20.0)), 1000);
        ASSERT_TRUE(decoded.loudness && decoded.f_peak);
        EXPECT_NEAR(*decoded.loudness, expected_value(level_db + power_db, BIN_CENTERED_HZ), 2);
        EXPECT_EQ(decoded.f_peak->first, static_cast<uint16_t>(std::round(BIN_CENTERED_HZ)));
        EXPECT_NEAR(decoded.f_peak->second, expected_value(level_db + peak_db, BIN_CENTERED_HZ), 1);
    }
    // Between bins, the interpolated peak lands within a few Hz of the frequency
    VisualizerAnalysis analysis = this->analysis({this->periodic_, 30, std::nullopt});
    const Decoded decoded = run(analysis, sine(3000.0, 0.5), 1000);
    ASSERT_TRUE(decoded.f_peak);
    EXPECT_NEAR(decoded.f_peak->first, 3000, 3);
}

// f_peak stays on the bin it reported while another bin is under F_PEAK_SWITCH_RATIO (1.1) times
// as strong, A-weighted, and moves once one is stronger than that
TEST_F(VisualizerFeatures, KeepTheFPeakBinUnderTheSwitchRatio) {
    const double held_hz = BIN_CENTERED_HZ;
    const double other_hz = 60.0 * RATE / WINDOW;
    // The other tone's amplitude for a weighted magnitude ratio times the held one's
    const double gain = std::pow(10.0, (sendspin::server::a_weighting_db(other_hz) -
                                        sendspin::server::a_weighting_db(held_hz)) /
                                           20.0);
    struct Row {
        const char* name;
        double ratio;
        double expected_hz;
    };
    for (const Row& row : {Row{"5% stronger: the held bin stays", 1.05, held_hz},
                           Row{"Control: 20% stronger: it moves", 1.2, other_hz}}) {
        SCOPED_TRACE(row.name);
        VisualizerAnalysis analysis = this->analysis({type::F_PEAK, 30, std::nullopt});
        const Decoded alone = run(analysis, sine(held_hz, 0.25), 0);
        ASSERT_TRUE(alone.f_peak);
        EXPECT_NEAR(alone.f_peak->first, held_hz, 1);
        std::vector<float> mix = sine(held_hz, 0.25);
        const std::vector<float> other = sine(other_hz, 0.25 * row.ratio / gain);
        for (size_t i = 0; i < mix.size(); ++i) {
            mix[i] += other[i];
        }
        const Decoded both = run(analysis, mix, 1);
        ASSERT_TRUE(both.f_peak);
        EXPECT_NEAR(both.f_peak->first, row.expected_hz, 1);
    }
}

// The window is the newest window's samples in order, however they were pushed: pieces that wrap
// the history give the messages of the same samples pushed at once
TEST_F(VisualizerFeatures, AnalyzeTheNewestSamplesInOrder) {
    const VisualizerConfig config{type::LOUDNESS | type::F_PEAK | type::SPECTRUM, 30,
                                  SpectrumConfig{16, SpectrumScale::LOG, 20, 20000}};
    // Not a whole number of periods in the window, so samples out of order would show
    const std::vector<float> tone = sine(1234.5, 0.5, 3000);
    VisualizerAnalysis pieces = this->analysis(config);
    pieces.push(std::vector<float>(WINDOW, 0.25F));
    for (size_t from = 0; from < tone.size(); from += 1000) {
        pieces.push(std::span(tone).subspan(from, 1000));
    }
    std::vector<uint8_t> wrapped;
    pieces.analyze(0, true, false, wrapped);
    VisualizerAnalysis whole = this->analysis(config);
    whole.push(std::span(tone).last(WINDOW));
    std::vector<uint8_t> once;
    whole.analyze(0, true, false, once);
    EXPECT_EQ(wrapped, once);
}

// spec roles/visualizer/v1.md "f_peak": with no peak, frequency and amplitude are 0; silence is 0
// on every scale and fires no onset
TEST_F(VisualizerFeatures, ReportSilenceAsZero) {
    const VisualizerConfig config{type::LOUDNESS | type::F_PEAK | type::SPECTRUM | type::PEAK, 30,
                                  SpectrumConfig{8, SpectrumScale::LIN, 0, 4000}};
    VisualizerAnalysis analysis = this->analysis(config);
    for (int64_t ts = 0; ts < 5; ++ts) {
        const Decoded decoded = run(analysis, std::vector<float>(WINDOW / 4), ts, true, true);
        ASSERT_TRUE(decoded.loudness && decoded.f_peak && decoded.spectrum);
        EXPECT_EQ(*decoded.loudness, 0);
        EXPECT_EQ(decoded.f_peak->first, 0);
        EXPECT_EQ(decoded.f_peak->second, 0);
        EXPECT_EQ(*decoded.spectrum, std::vector<uint16_t>(8, 0));
        EXPECT_FALSE(decoded.peak);
    }
}

// A sine's power lands in the display bin holding its frequency: the window's bins within it (the
// main lobe, about 1.8 dB above the peak bin, and side lobes), at its level A-weighted, within a
// count of their exact power computed here in double precision; bins away from it hold only far
// side lobes
TEST_F(VisualizerFeatures, BinTheSpectrumByFrequency) {
    const VisualizerConfig config{type::SPECTRUM, 30,
                                  SpectrumConfig{8, SpectrumScale::LIN, 0, 4000}};
    VisualizerAnalysis analysis = this->analysis(config);
    const double level_db = -24.0;
    const Decoded decoded =
        run(analysis, sine(BIN_CENTERED_HZ, std::pow(10.0, level_db / 20.0)), 0);
    ASSERT_TRUE(decoded.spectrum);
    const std::vector<uint16_t>& bins = *decoded.spectrum;
    // 1031.25 Hz is in 1000 to 1500 Hz
    EXPECT_EQ(std::max_element(bins.begin(), bins.end()) - bins.begin(), 2);
    // FFT bins 43 to 63 lie in 1000 to 1500 Hz, -1 to 19 from the sine's 44
    EXPECT_NEAR(bins[2], expected_value(level_db + window_db(-1, 19), BIN_CENTERED_HZ), 1);
    for (const size_t far : {5U, 6U, 7U}) {
        EXPECT_LT(bins[far], bins[2] / 4) << far;
    }
}

// Loudness and the spectrum are smoothed (new value weighted LOUDNESS_SMOOTHING and
// SPECTRUM_SMOOTHING); reset() starts afresh
TEST_F(VisualizerFeatures, SmoothLoudnessAndTheSpectrumUntilReset) {
    const VisualizerConfig config{type::LOUDNESS | type::SPECTRUM, 30,
                                  SpectrumConfig{4, SpectrumScale::LIN, 500, 2500}};
    VisualizerAnalysis analysis = this->analysis(config);
    const Decoded loud = run(analysis, sine(BIN_CENTERED_HZ, 1.0), 0);
    ASSERT_TRUE(loud.loudness && loud.spectrum);
    EXPECT_EQ(*loud.loudness, 65535);
    const uint16_t peak_bin = (*loud.spectrum)[1];
    EXPECT_EQ(peak_bin, 65535);
    const Decoded quiet = run(analysis, std::vector<float>(WINDOW), 1);
    ASSERT_TRUE(quiet.loudness && quiet.spectrum);
    EXPECT_NEAR(*quiet.loudness, 0.5 * 65535, 1);
    EXPECT_NEAR((*quiet.spectrum)[1], 0.6 * 65535, 1);
    analysis.reset();
    const Decoded fresh = run(analysis, std::vector<float>(WINDOW), 2);
    ASSERT_TRUE(fresh.loudness && fresh.spectrum);
    EXPECT_EQ(*fresh.loudness, 0);
    EXPECT_EQ((*fresh.spectrum)[1], 0);
}

// spec roles/visualizer/v1.md "peak": an energy onset fires once, with a strength scaling its
// excess, and not again within ONSET_GAP_US; the running average follows the level
TEST_F(VisualizerFeatures, FireAPeakOnAnOnsetOnce) {
    VisualizerAnalysis analysis = this->analysis({type::PEAK, 30, std::nullopt});
    const int64_t cell = 20000;
    // Control: silence, the average's first value and then steady
    EXPECT_FALSE(run(analysis, std::vector<float>(WINDOW), 0, false, true).peak);
    EXPECT_FALSE(run(analysis, std::vector<float>(WINDOW), cell, false, true).peak);
    const Decoded onset = run(analysis, sine(BIN_CENTERED_HZ, 0.5), 2 * cell, false, true);
    ASSERT_TRUE(onset.peak);
    EXPECT_EQ(*onset.peak, 255);
    EXPECT_EQ(onset.timestamps, std::vector<int64_t>{2 * cell});
    // Louder still, but within the gap
    EXPECT_FALSE(run(analysis, sine(BIN_CENTERED_HZ, 1.0), 3 * cell, false, true).peak);
    // A periodic instant alone checks no onset
    EXPECT_FALSE(run(analysis, sine(BIN_CENTERED_HZ, 1.0), 10 * cell, true, false).peak);
}

// spec roles/visualizer/v1.md "Visualization Data (Binary)": `[type][ts:8 big-endian][data]`, in
// type order, uint16 fields big-endian
TEST_F(VisualizerFeatures, WriteEachMessageInTheSpecsLayout) {
    VisualizerAnalysis analysis = this->analysis({this->periodic_, 30, std::nullopt});
    analysis.push(std::vector<float>(WINDOW));
    std::vector<uint8_t> bytes;
    analysis.analyze(0x0102030405060708, true, true, bytes);
    const std::vector<uint8_t> expected = {16, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 18,
                                           1,  2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(sendspin::server::visualizer_message_bytes(type::LOUDNESS, 0), 11U);
    EXPECT_EQ(sendspin::server::visualizer_message_bytes(type::F_PEAK, 0), 13U);
    EXPECT_EQ(sendspin::server::visualizer_message_bytes(type::SPECTRUM, 32), 73U);
    EXPECT_EQ(sendspin::server::visualizer_message_bytes(type::PEAK, 0), 10U);
}

// The features on a fixed signal (440 Hz at half scale and 3 kHz at a quarter, 16-bit, one window
// at 48 kHz) against reference values from aiosendspin's visualizer feature extractor (aiosendspin
// 2ef6fec, Python 3.12.11, numpy 2.5.3, float64), for each scale. Loudness and f_peak agree within
// 1, single precision against double truncating to the next count at most; the spectrum within 6:
// its full-scale reference here is N / 4 and there (N + 1) / 4, 20 log10((N + 1) / N) = 0.0042 dB
// or 4.6 counts at N = 2048, plus that truncation
TEST_F(VisualizerFeatures, MatchesReferenceFeatureValuesOnAFixedSignal) {
    std::vector<float> samples(WINDOW);
    for (size_t i = 0; i < WINDOW; ++i) {
        const double t = static_cast<double>(i) / RATE;
        const double x = (0.5 * std::sin(2.0 * std::numbers::pi * 440.0 * t)) +
                         (0.25 * std::sin((2.0 * std::numbers::pi * 3000.0 * t) + 0.3));
        samples[i] = static_cast<float>(std::round(x * 32767.0) / 32768.0);
    }
    struct Row {
        SpectrumConfig spectrum;
        std::vector<uint16_t> bins;
    };
    const std::vector<Row> rows = {
        {{16, SpectrumScale::MEL, 40, 16000},
         {0, 11361, 56401, 0, 0, 0, 0, 0, 55640, 0, 0, 0, 0, 0, 0, 0}},
        {{12, SpectrumScale::LOG, 20, 20000}, {0, 0, 0, 0, 1933, 56402, 0, 0, 55640, 0, 0, 0}},
        {{10, SpectrumScale::LIN, 0, 5000}, {56401, 9478, 0, 0, 0, 47150, 54774, 0, 0, 0}},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(static_cast<int>(row.spectrum.scale));
        VisualizerAnalysis analysis =
            this->analysis({type::LOUDNESS | type::F_PEAK | type::SPECTRUM, 50, row.spectrum});
        const Decoded decoded = run(analysis, samples, 0);
        ASSERT_TRUE(decoded.loudness && decoded.f_peak && decoded.spectrum);
        EXPECT_NEAR(*decoded.loudness, 57406, 1);
        EXPECT_EQ(decoded.f_peak->first, 442);
        EXPECT_NEAR(decoded.f_peak->second, 54365, 1);
        for (size_t i = 0; i < row.bins.size(); ++i) {
            EXPECT_NEAR((*decoded.spectrum)[i], row.bins[i], 6) << i;
        }
    }
}

}  // namespace
