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

/// @file visualizer.h
/// @brief The visualizer role's values (spec roles/visualizer/v1.md): the configuration a client
/// requests in client/state, the stream configuration the server derives from it and echoes in
/// stream/start, and the binary messages' layout. Pure.

#pragma once

#include "sched/constants.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sendspin::server {

/// @brief The visualization data types as a set: one bit per type, bit `id - 16` for the type's
/// binary message type `id` (spec roles/visualizer/v1.md "Visualization Data (Binary)")
namespace visualizer_type {
constexpr uint8_t LOUDNESS = 0x01;
constexpr uint8_t BEAT = 0x02;
constexpr uint8_t F_PEAK = 0x04;
constexpr uint8_t SPECTRUM = 0x08;
constexpr uint8_t PEAK = 0x10;
/// The periodic types, which `rate_max` applies to
constexpr uint8_t PERIODIC = LOUDNESS | F_PEAK | SPECTRUM;
/// The types this server computes; `beat` needs a beat tracker it does not have
constexpr uint8_t STREAMABLE = PERIODIC | PEAK;
}  // namespace visualizer_type

/// @brief The binary message type of the first visualization type; a type's is this plus its
/// bit's index
constexpr uint8_t VISUALIZER_FIRST_MESSAGE_ID = 16;
/// @brief A visualization message's type byte and big-endian timestamp
constexpr size_t VISUALIZER_HEADER_BYTES = 9;

enum class SpectrumScale : uint8_t { MEL, LOG, LIN };

/// @brief A spectrum configuration as the client sent it (spec roles/visualizer/v1.md
/// "client/state visualizer object"); stream/start echoes it exactly
struct SpectrumConfig {
    uint32_t n_disp_bins{0};
    SpectrumScale scale{SpectrumScale::MEL};
    uint32_t f_min{0};
    uint32_t f_max{0};

    bool operator==(const SpectrumConfig&) const = default;
};

/// @brief A validated client/state visualizer object (spec roles/visualizer/v1.md "client/state
/// visualizer object")
struct VisualizerRequest {
    /// visualizer_type bits of the recognized types requested
    uint8_t types{0};
    /// Positive
    uint32_t rate_max{0};
    /// Present whenever types holds SPECTRUM, unless spectrum_unusable
    std::optional<SpectrumConfig> spectrum;
    /// The spectrum object is well-formed but names what the server cannot stream: an unknown
    /// scale, or an integer outside uint32; negotiated away like any unstreamable spectrum
    bool spectrum_unusable{false};

    bool operator==(const VisualizerRequest&) const = default;
};

/// @brief A visualizer stream's configuration, as its stream/start announces it (spec
/// roles/visualizer/v1.md "stream/start visualizer object")
struct VisualizerConfig {
    /// visualizer_type bits, within visualizer_type::STREAMABLE
    uint8_t types{0};
    /// Periodic messages per second per type, from 1 to MAX_VISUALIZER_RATE
    uint32_t rate{0};
    /// Present exactly when types holds SPECTRUM
    std::optional<SpectrumConfig> spectrum;

    bool operator==(const VisualizerConfig&) const = default;
};

/// @brief The most periodic messages per second per type the server sends: one analysis instant
/// per 20 ms cell at most, so each cell carries at most one message of each type
constexpr uint32_t MAX_VISUALIZER_RATE = sched::CELLS_PER_SECOND;

/// @brief The most visualizer analyses a group runs, one per distinct configuration its clients
/// stream or want: each holds its windows of history, FFT and tables and runs at most one FFT a
/// cell. A configuration past the cap fails as an analysis that failed would.
constexpr size_t MAX_GROUP_ANALYSES = 4;

/// @brief The most spectrum display bins the server streams (spec roles/visualizer/v1.md
/// "spectrum": an implementation-defined upper bound); a request for more gets no spectrum
constexpr uint32_t MAX_DISPLAY_BINS = 256;

/// @brief The highest spectrum frequency bound the server streams, above any input's Nyquist
/// frequency; a request past it gets no spectrum. Twenty bits, so config_key() is exact.
constexpr uint32_t MAX_SPECTRUM_HZ = (uint32_t{1} << 20) - 1;

/// @brief The stream configuration for a request: the types the server can stream (no `beat`;
/// no `spectrum` with 0 bins or more than MAX_DISPLAY_BINS, an empty range or a bound past
/// MAX_SPECTRUM_HZ; no type one of whose messages is larger than buffer_capacity), at most
/// MAX_VISUALIZER_RATE per second, the spectrum echoed exactly (spec roles/visualizer/v1.md
/// "stream/start visualizer object": types the server cannot stream are omitted)
VisualizerConfig negotiate_visualizer(const VisualizerRequest& request, uint32_t buffer_capacity);

/// @brief A key equal for two configurations exactly when they are equal, and never 0 (the key
/// sched::FormatKey gives a player)
uint64_t config_key(const VisualizerConfig& config);

/// @brief The size of one message of a type, header included; bins is the spectrum's
/// `n_disp_bins`
/// @param type one visualizer_type bit
size_t visualizer_message_bytes(uint8_t type, uint32_t bins);

/// @brief The visualizer_type bit of a binary message type; 0 for one outside the role's
uint8_t visualizer_type_of(uint8_t message_id);

}  // namespace sendspin::server
