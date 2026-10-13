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

/// @file test_colors.cpp
/// @brief The color role's pure parts (spec roles/color/v1.md): WCAG contrast, the guarantees
/// with_contrast() and palette_colors() keep as a seeded property test with a contrast computed
/// here, the median cut, and the schedule of color states.

#include "colors.h"
#include "scheduled_updates.h"
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace {

using sendspin::server::Colors;
using sendspin::server::ColorState;
using sendspin::server::RgbColor;

/// WCAG 2 contrast, computed here from the definition rather than by colors.cpp
double wcag_contrast(RgbColor a, RgbColor b) {
    const auto luminance = [](RgbColor c) {
        const auto linear = [](uint8_t v) {
            const double s = v / 255.0;
            return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

constexpr RgbColor BLACK{0, 0, 0};
constexpr RgbColor WHITE{255, 255, 255};
constexpr double MIN = 4.5;

/// Every pair spec roles/color/v1.md "server/state color object" names has 4.5:1
void expect_guarantees(const Colors& c) {
    if (c.background_dark) {
        EXPECT_GE(wcag_contrast(*c.background_dark, WHITE), MIN);
        if (c.on_dark) {
            EXPECT_GE(wcag_contrast(*c.background_dark, *c.on_dark), MIN);
        }
    }
    if (c.background_light) {
        EXPECT_GE(wcag_contrast(*c.background_light, BLACK), MIN);
        if (c.on_light) {
            EXPECT_GE(wcag_contrast(*c.background_light, *c.on_light), MIN);
        }
    }
    if (c.on_dark) {
        EXPECT_GE(wcag_contrast(*c.on_dark, BLACK), MIN);
    }
    if (c.on_light) {
        EXPECT_GE(wcag_contrast(*c.on_light, WHITE), MIN);
    }
}

TEST(ColorContrast, MatchesTheWcagDefinition) {
    EXPECT_DOUBLE_EQ(sendspin::server::contrast_ratio(BLACK, WHITE), 21.0);
    EXPECT_DOUBLE_EQ(sendspin::server::contrast_ratio(WHITE, BLACK), 21.0);
    EXPECT_DOUBLE_EQ(sendspin::server::contrast_ratio({90, 90, 90}, {90, 90, 90}), 1.0);
    // #767676 on white is the classic 4.54:1
    EXPECT_NEAR(sendspin::server::contrast_ratio({0x76, 0x76, 0x76}, WHITE), 4.54, 0.01);
    EXPECT_EQ(sendspin::server::mix({0, 100, 200}, WHITE, 0.5), (RgbColor{128, 178, 228}));
}

// spec roles/color/v1.md "server/state color object": the server MUST ensure 4.5:1 for every
// background and on-color pair, whatever the host gives; a field that has it is kept, primary
// and accent are never adjusted, and an absent field stays absent
TEST(ColorContrast, HostColorsGetEveryGuaranteeSeeded) {
    std::mt19937 random(20261010);
    std::uniform_int_distribution<int> byte(0, 255);
    std::bernoulli_distribution present(0.7);
    const auto maybe = [&]() -> std::optional<RgbColor> {
        if (!present(random)) {
            return std::nullopt;
        }
        return RgbColor{static_cast<uint8_t>(byte(random)), static_cast<uint8_t>(byte(random)),
                        static_cast<uint8_t>(byte(random))};
    };
    size_t adjusted = 0;
    size_t kept = 0;
    for (int i = 0; i < 20000; ++i) {
        Colors in;
        in.background_dark = maybe();
        in.background_light = maybe();
        in.primary = maybe();
        in.accent = maybe();
        in.on_dark = maybe();
        in.on_light = maybe();
        const Colors out = sendspin::server::with_contrast(in);
        SCOPED_TRACE(i);
        expect_guarantees(out);
        EXPECT_EQ(out.primary, in.primary);
        EXPECT_EQ(out.accent, in.accent);
        EXPECT_EQ(out.background_dark.has_value(), in.background_dark.has_value());
        EXPECT_EQ(out.on_light.has_value(), in.on_light.has_value());
        // An on-color with its contrast is kept as given
        if (in.on_dark && wcag_contrast(*in.on_dark, BLACK) >= MIN) {
            EXPECT_EQ(out.on_dark, in.on_dark);
            ++kept;
        } else if (in.on_dark) {
            EXPECT_NE(out.on_dark, in.on_dark);
            ++adjusted;
        }
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
    // Control: the seed reaches both branches
    EXPECT_GT(adjusted, 1000U);
    EXPECT_GT(kept, 1000U);
}

// The palette derived from an image keeps the same guarantees, for any candidates
TEST(ColorContrast, ImagePalettesGetEveryGuaranteeSeeded) {
    std::mt19937 random(5);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> count(1, 5);
    for (int i = 0; i < 5000; ++i) {
        std::vector<RgbColor> candidates(static_cast<size_t>(count(random)));
        for (RgbColor& candidate : candidates) {
            candidate = {static_cast<uint8_t>(byte(random)), static_cast<uint8_t>(byte(random)),
                         static_cast<uint8_t>(byte(random))};
        }
        const Colors out = sendspin::server::palette_colors(candidates);
        SCOPED_TRACE(i);
        expect_guarantees(out);
        EXPECT_EQ(out.primary, candidates.front());
        EXPECT_TRUE(out.background_dark && out.background_light && out.on_dark && out.on_light);
        if (::testing::Test::HasFailure()) {
            return;
        }
    }
    EXPECT_EQ(sendspin::server::palette_colors({}), Colors{});
}

/// An RGBA image: count pixels of each color, in order
std::vector<uint8_t> pixels(const std::vector<std::pair<std::array<uint8_t, 4>, int>>& runs) {
    std::vector<uint8_t> out;
    for (const auto& [rgba, count] : runs) {
        for (int i = 0; i < count; ++i) {
            out.insert(out.end(), rgba.begin(), rgba.end());
        }
    }
    return out;
}

// The median cut: the dominant color first; transparent and near-white pixels do not count
TEST(ColorPalette, PutsTheDominantColorFirst) {
    const std::vector<uint8_t> image = pixels({{{200, 30, 30, 255}, 700},
                                               {{20, 40, 220, 255}, 300},
                                               {{0, 255, 0, 0}, 5000},
                                               {{255, 255, 255, 255}, 5000}});
    const std::vector<RgbColor> palette = sendspin::server::quantize_palette(image, 5, 1);
    // Two colors, two boxes: a cut that leaves a box of no pixel reports no color for it
    ASSERT_EQ(palette.size(), 2U);
    // The bucket centre of 5 bits a channel: 200 is in 25, at 204
    EXPECT_EQ(palette[0], (RgbColor{204, 28, 28}));
    EXPECT_EQ(palette[1], (RgbColor{20, 44, 220}));
    // Control: the green, opaque, would lead
    const std::vector<uint8_t> opaque = pixels({{{0, 255, 0, 255}, 5000}, {{200, 30, 30, 255}, 7}});
    EXPECT_EQ(sendspin::server::quantize_palette(opaque, 5, 1)[0], (RgbColor{4, 252, 4}));
    EXPECT_TRUE(
        sendspin::server::quantize_palette(pixels({{{255, 255, 255, 255}, 10}}), 5, 1).empty());
}

// spec roles/color/v1.md "Scheduled color updates", through the schedule metadata shares
TEST(ColorSchedule, SendsCurrentAndScheduledStates) {
    constexpr int64_t S = 1000000;
    sendspin::server::ScheduledUpdates<ColorState> colors;
    Colors red;
    red.primary = RgbColor{255, 0, 0};
    EXPECT_EQ(colors.current(5).timestamp_us, 5);
    EXPECT_EQ(colors.current(5).colors, Colors{});
    std::vector<ColorState> sends = colors.set({S, red}, 2 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0], (ColorState{S, red}));
    Colors blue;
    blue.primary = RgbColor{0, 0, 255};
    EXPECT_TRUE(colors.set({30 * S, blue}, 3 * S).empty());
    EXPECT_EQ(colors.next_due_us(), 10 * S);
    sends = colors.advance(10 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0].colors, blue);
    EXPECT_EQ(colors.sent_pending(), (ColorState{30 * S, blue}));
    // Cancelled once sent: the current state again
    sends = colors.cancel(11 * S);
    ASSERT_EQ(sends.size(), 1U);
    EXPECT_EQ(sends[0], (ColorState{S, red}));
}

}  // namespace
