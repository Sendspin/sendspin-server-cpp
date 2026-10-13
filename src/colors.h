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

/// @file colors.h
/// @brief The color role's colors (spec roles/color/v1.md): WCAG contrast, the contrast the
/// server guarantees, and a palette derived from an image (median cut quantization, then the
/// fields picked from it). Pure.

#pragma once

#include "sendspin/server/types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sendspin::server {

/// @brief The least contrast ratio between the pairs spec roles/color/v1.md "server/state color
/// object" names
constexpr double MIN_CONTRAST = 4.5;

constexpr RgbColor BLACK{0, 0, 0};
constexpr RgbColor WHITE{255, 255, 255};

/// @brief WCAG 2 relative luminance of an sRGB color, from 0 to 1
double relative_luminance(RgbColor color);

/// @brief WCAG 2 contrast ratio, from 1 to 21
double contrast_ratio(RgbColor a, RgbColor b);

/// @brief color moved toward target by factor (0 keeps it, 1 is target), each channel rounded
RgbColor mix(RgbColor color, RgbColor target, double factor);

/// @brief The least move of color toward target, in steps of 1 / steps, that gives at least
/// min_contrast against every color in refs; nullopt when even target does not
std::optional<RgbColor> toward_contrast(RgbColor color, RgbColor target,
                                        std::span<const RgbColor> refs, double min_contrast,
                                        int steps);

/// @brief colors with every guarantee of spec roles/color/v1.md "server/state color object" met,
/// each field moved no further than it must be: on_dark toward white until it has MIN_CONTRAST
/// with black, on_light toward black until it has it with white, then background_dark toward
/// black until it has it with white and on_dark, and background_light toward white until it has
/// it with black and on_light. primary and accent are kept.
Colors with_contrast(const Colors& colors);

/// @brief The dominant colors of an image by modified median cut quantization (MMCQ): every
/// quality-th pixel that is mostly opaque (alpha 125 or more) and not near white, quantized to 5
/// bits a channel and cut into at most max_colors boxes, each nonempty box's average color, the
/// boxes covering most pixels and colors first. Empty when no pixel counts.
/// @param rgba 4 bytes a pixel
std::vector<RgbColor> quantize_palette(std::span<const uint8_t> rgba, size_t max_colors,
                                       size_t quality);

/// @brief The colors of an image whose dominant colors are candidates, the first the most
/// dominant: primary is the first; on_dark and on_light are the candidates with the most
/// contrast against black and white, preferring 7:1 and capping on_light's at 17.35:1 so a
/// near-black outline does not win, else primary moved until it has MIN_CONTRAST; the backgrounds
/// are primary moved toward black and white until they reach 7:1, else MIN_CONTRAST, with the
/// text color and on-color they pair with; accent is the first candidate far from primary. Then
/// with_contrast(), which these choices already satisfy. Empty with no candidate.
Colors palette_colors(std::span<const RgbColor> candidates);

/// @brief A server/state color object (spec roles/color/v1.md "server/state color object")
struct ColorState {
    /// When it takes effect, on the server's clock
    int64_t timestamp_us{0};
    Colors colors;

    bool operator==(const ColorState&) const = default;
};

}  // namespace sendspin::server
