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

#include "visualizer.h"

#include <algorithm>

namespace sendspin::server {

VisualizerConfig negotiate_visualizer(const VisualizerRequest& request, uint32_t buffer_capacity) {
    VisualizerConfig config;
    config.types = request.types & visualizer_type::STREAMABLE;
    config.rate = std::min(request.rate_max, MAX_VISUALIZER_RATE);
    if ((config.types & visualizer_type::SPECTRUM) != 0) {
        const std::optional<SpectrumConfig>& spectrum = request.spectrum;
        const bool streamable =
            spectrum && spectrum->n_disp_bins > 0 && spectrum->n_disp_bins <= MAX_DISPLAY_BINS &&
            spectrum->f_min < spectrum->f_max && spectrum->f_max <= MAX_SPECTRUM_HZ;
        if (streamable) {
            config.spectrum = spectrum;
        } else {
            config.types &= static_cast<uint8_t>(~visualizer_type::SPECTRUM);
        }
    }
    // spec roles/visualizer/v1.md "client/hello visualizer@v1 support object": a message larger
    // than buffer_capacity could never be sent
    const uint32_t bins = config.spectrum ? config.spectrum->n_disp_bins : 0;
    for (uint8_t type = visualizer_type::LOUDNESS; type <= visualizer_type::PEAK; type <<= 1) {
        if ((config.types & type) != 0 && visualizer_message_bytes(type, bins) > buffer_capacity) {
            config.types &= static_cast<uint8_t>(~type);
        }
    }
    if ((config.types & visualizer_type::SPECTRUM) == 0) {
        config.spectrum.reset();
    }
    return config;
}

uint64_t config_key(const VisualizerConfig& config) {
    // 5 type bits, 6 rate bits, 2 scale bits, 9 bin bits and two 20-bit bounds, each field within
    // its width by negotiate_visualizer(); the top bit keeps the key from 0
    constexpr unsigned RATE_SHIFT = 5;
    constexpr unsigned SCALE_SHIFT = 11;
    constexpr unsigned BINS_SHIFT = 13;
    constexpr unsigned F_MIN_SHIFT = 22;
    constexpr unsigned F_MAX_SHIFT = 42;
    constexpr unsigned NONZERO_SHIFT = 63;
    uint64_t key = uint64_t{1} << NONZERO_SHIFT;
    key |= uint64_t{config.types};
    key |= uint64_t{config.rate} << RATE_SHIFT;
    if (config.spectrum) {
        key |= uint64_t{static_cast<uint8_t>(config.spectrum->scale)} << SCALE_SHIFT;
        key |= uint64_t{config.spectrum->n_disp_bins} << BINS_SHIFT;
        key |= uint64_t{config.spectrum->f_min} << F_MIN_SHIFT;
        key |= uint64_t{config.spectrum->f_max} << F_MAX_SHIFT;
    }
    return key;
}

size_t visualizer_message_bytes(uint8_t type, uint32_t bins) {
    // spec roles/visualizer/v1.md "Visualization Data (Binary)": the data after the header
    switch (type) {
        case visualizer_type::LOUDNESS:
            return VISUALIZER_HEADER_BYTES + 2;
        case visualizer_type::F_PEAK:
            return VISUALIZER_HEADER_BYTES + 4;
        case visualizer_type::SPECTRUM:
            return VISUALIZER_HEADER_BYTES + (size_t{2} * bins);
        default:
            return VISUALIZER_HEADER_BYTES + 1;
    }
}

uint8_t visualizer_type_of(uint8_t message_id) {
    if (message_id < VISUALIZER_FIRST_MESSAGE_ID || message_id > VISUALIZER_FIRST_MESSAGE_ID + 4) {
        return 0;
    }
    return static_cast<uint8_t>(1U << (message_id - VISUALIZER_FIRST_MESSAGE_ID));
}

}  // namespace sendspin::server
