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

#include "colors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace sendspin::server {

namespace {

// ==============================================================================
// Contrast
// ==============================================================================

/// The contrast palette_colors() prefers, WCAG AAA
constexpr double PREFERRED_CONTRAST = 7.0;
/// The most contrast on_light may have with white, so a near-black region does not win
constexpr double MAX_DARK_PICK_CONTRAST = 17.35;
/// palette_colors() moves a color in steps of a twentieth
constexpr int PALETTE_STEPS = 20;
/// with_contrast() moves a color in steps of 1/256, so it stays as close to the host's as it can
constexpr int GUARANTEE_STEPS = 256;
/// Squared RGB distance from primary an accent needs
constexpr int ACCENT_DISTANCE_SQUARED = 60 * 60;

/// sRGB's linear value of an 8-bit channel (IEC 61966-2-1), as WCAG 2 defines luminance
double linear(uint8_t channel) {
    constexpr double MAX = 255.0;
    constexpr double KNEE = 0.04045;
    constexpr double SLOPE = 12.92;
    constexpr double OFFSET = 0.055;
    constexpr double GAMMA = 2.4;
    const double s = channel / MAX;
    return s <= KNEE ? s / SLOPE : std::pow((s + OFFSET) / (1 + OFFSET), GAMMA);
}

/// The candidate with the most contrast against target within [min, max]
std::optional<RgbColor> pick_on_color(std::span<const RgbColor> candidates, RgbColor target,
                                      double min_contrast, double max_contrast) {
    std::optional<RgbColor> best;
    double best_ratio = 0;
    for (const RgbColor candidate : candidates) {
        const double ratio = contrast_ratio(candidate, target);
        if (ratio >= min_contrast && ratio <= max_contrast && ratio > best_ratio) {
            best = candidate;
            best_ratio = ratio;
        }
    }
    return best;
}

std::optional<RgbColor> pick_on_color_preferring(std::span<const RgbColor> candidates,
                                                 RgbColor target, double max_contrast) {
    std::optional<RgbColor> picked =
        pick_on_color(candidates, target, PREFERRED_CONTRAST, max_contrast);
    return picked ? picked : pick_on_color(candidates, target, MIN_CONTRAST, max_contrast);
}

std::optional<RgbColor> background_preferring(RgbColor color, RgbColor target,
                                              std::span<const RgbColor> refs) {
    std::optional<RgbColor> moved =
        toward_contrast(color, target, refs, PREFERRED_CONTRAST, PALETTE_STEPS);
    return moved ? moved : toward_contrast(color, target, refs, MIN_CONTRAST, PALETTE_STEPS);
}

/// The field with at least MIN_CONTRAST against every ref, moved toward target when it lacks it
void guarantee(std::optional<RgbColor>& field, RgbColor target, std::span<const RgbColor> refs) {
    if (!field) {
        return;
    }
    // target itself has the contrast whenever the refs have it with target's opposite, which the
    // order of with_contrast() ensures
    field = toward_contrast(*field, target, refs, MIN_CONTRAST, GUARANTEE_STEPS).value_or(target);
}

// ==============================================================================
// Median cut (MMCQ)
// ==============================================================================

constexpr int SIGBITS = 5;
constexpr int RSHIFT = 8 - SIGBITS;
constexpr int HISTOGRAM_SIZE = 1 << (3 * SIGBITS);
constexpr int MAX_ITERATIONS = 1000;
/// The share of max_colors cut by pixel count before cutting by count times volume
constexpr double FRACTION_BY_POPULATION = 0.75;
/// A pixel counts when its alpha is at least this and it is not near white
constexpr uint8_t MIN_ALPHA = 125;
constexpr uint8_t NEAR_WHITE = 250;

int color_index(int r, int g, int b) {
    return (r << (2 * SIGBITS)) + (g << SIGBITS) + b;
}

struct Box {
    std::array<int, 3> lo{};
    std::array<int, 3> hi{};
    int64_t count{0};

    int64_t volume() const {
        int64_t volume = 1;
        for (size_t axis = 0; axis < 3; ++axis) {
            volume *= this->hi[axis] - this->lo[axis] + 1;
        }
        return volume;
    }
};

class MedianCut {
public:
    explicit MedianCut(std::vector<int64_t> histogram) : histogram_(std::move(histogram)) {}

    int64_t count(const Box& box) const {
        int64_t count = 0;
        for (int r = box.lo[0]; r <= box.hi[0]; ++r) {
            for (int g = box.lo[1]; g <= box.hi[1]; ++g) {
                for (int b = box.lo[2]; b <= box.hi[2]; ++b) {
                    count += this->histogram_[static_cast<size_t>(color_index(r, g, b))];
                }
            }
        }
        return count;
    }

    RgbColor average(const Box& box) const {
        constexpr int MULT = 1 << RSHIFT;
        constexpr double HALF = 0.5;
        int64_t total = 0;
        std::array<double, 3> sums{};
        for (int r = box.lo[0]; r <= box.hi[0]; ++r) {
            for (int g = box.lo[1]; g <= box.hi[1]; ++g) {
                for (int b = box.lo[2]; b <= box.hi[2]; ++b) {
                    const int64_t n = this->histogram_[static_cast<size_t>(color_index(r, g, b))];
                    total += n;
                    sums[0] += static_cast<double>(n) * (r + HALF) * MULT;
                    sums[1] += static_cast<double>(n) * (g + HALF) * MULT;
                    sums[2] += static_cast<double>(n) * (b + HALF) * MULT;
                }
            }
        }
        std::array<uint8_t, 3> rgb{};
        for (size_t axis = 0; axis < 3; ++axis) {
            const double value = total != 0 ? sums[axis] / static_cast<double>(total)
                                            : MULT * (box.lo[axis] + box.hi[axis] + 1) / 2.0;
            rgb[axis] = static_cast<uint8_t>(std::clamp(std::trunc(value), 0.0, 255.0));
        }
        return {rgb[0], rgb[1], rgb[2]};
    }

    /// Splits box at the median of its longest axis; nullopt for the second when it holds one
    /// color, and for both when it is empty
    std::optional<std::pair<Box, std::optional<Box>>> cut(const Box& box) const {
        if (box.count == 0) {
            return std::nullopt;
        }
        if (box.count == 1) {
            return std::pair<Box, std::optional<Box>>{box, std::nullopt};
        }
        size_t axis = 0;
        for (size_t other = 1; other < 3; ++other) {
            if (box.hi[other] - box.lo[other] > box.hi[axis] - box.lo[axis]) {
                axis = other;
            }
        }
        // The count in each slice of the axis, accumulated
        const int first = box.lo[axis];
        const int last = box.hi[axis];
        std::vector<int64_t> partial(static_cast<size_t>(last - first + 1));
        int64_t total = 0;
        for (int i = first; i <= last; ++i) {
            Box slice = box;
            slice.lo[axis] = i;
            slice.hi[axis] = i;
            total += this->count(slice);
            partial[static_cast<size_t>(i - first)] = total;
        }
        const auto partial_at = [&](int i) -> int64_t {
            return i < first || i > last ? 0 : partial[static_cast<size_t>(i - first)];
        };
        for (int i = first; i <= last; ++i) {
            if (2 * partial_at(i) <= total) {
                continue;
            }
            const int left = i - first;
            const int right = last - i;
            int cut_at = left <= right ? std::min(last - 1, i + right / 2)
                                       : std::max(first, static_cast<int>((2 * i - 2 - left) / 2));
            // No box of no pixels
            while (partial_at(cut_at) == 0) {
                ++cut_at;
            }
            int64_t count_after = total - partial_at(cut_at);
            while (count_after == 0 && partial_at(cut_at - 1) != 0) {
                --cut_at;
                count_after = total - partial_at(cut_at);
            }
            Box low = box;
            Box high = box;
            low.hi[axis] = cut_at;
            high.lo[axis] = cut_at + 1;
            low.count = this->count(low);
            high.count = high.lo[axis] > high.hi[axis] ? 0 : this->count(high);
            return std::pair<Box, std::optional<Box>>{low, high};
        }
        return std::nullopt;
    }

    /// Cuts the box with the largest key until boxes holds target of them
    template <typename Key>
    void split(std::vector<Box>& boxes, double target, Key key) const {
        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
            if (static_cast<double>(boxes.size()) >= target) {
                return;
            }
            // The last of the largest, as a stable sort and a pop from its end take it
            size_t largest = 0;
            for (size_t i = 1; i < boxes.size(); ++i) {
                if (key(boxes[i]) >= key(boxes[largest])) {
                    largest = i;
                }
            }
            const Box box = boxes[largest];
            boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(largest));
            if (box.count == 0) {
                boxes.push_back(box);
                continue;
            }
            const std::optional<std::pair<Box, std::optional<Box>>> halves = this->cut(box);
            if (!halves) {
                boxes.push_back(box);
                return;
            }
            boxes.push_back(halves->first);
            if (halves->second) {
                boxes.push_back(*halves->second);
            }
        }
    }

private:
    std::vector<int64_t> histogram_;
};

}  // namespace

// ==============================================================================
// Contrast
// ==============================================================================

double relative_luminance(RgbColor color) {
    constexpr double RED = 0.2126;
    constexpr double GREEN = 0.7152;
    constexpr double BLUE = 0.0722;
    return RED * linear(color.r) + GREEN * linear(color.g) + BLUE * linear(color.b);
}

double contrast_ratio(RgbColor a, RgbColor b) {
    constexpr double FLARE = 0.05;
    const double la = relative_luminance(a);
    const double lb = relative_luminance(b);
    return (std::max(la, lb) + FLARE) / (std::min(la, lb) + FLARE);
}

RgbColor mix(RgbColor color, RgbColor target, double factor) {
    const auto channel = [factor](uint8_t from, uint8_t to) {
        return static_cast<uint8_t>(std::lround(from + (to - from) * factor));
    };
    return {channel(color.r, target.r), channel(color.g, target.g), channel(color.b, target.b)};
}

std::optional<RgbColor> toward_contrast(RgbColor color, RgbColor target,
                                        std::span<const RgbColor> refs, double min_contrast,
                                        int steps) {
    for (int step = 0; step <= steps; ++step) {
        const RgbColor candidate = mix(color, target, static_cast<double>(step) / steps);
        if (std::all_of(refs.begin(), refs.end(), [&](RgbColor ref) {
                return contrast_ratio(candidate, ref) >= min_contrast;
            })) {
            return candidate;
        }
    }
    return std::nullopt;
}

Colors with_contrast(const Colors& colors) {
    Colors out = colors;
    // spec roles/color/v1.md "server/state color object": each on-color also serves as a
    // background for the opposite text color, and each background pairs with its text color and
    // its on-color. The on-colors go first, so black and white stay within reach of the
    // backgrounds' guarantee: on_dark has the contrast with black, on_light with white.
    guarantee(out.on_dark, WHITE, std::array{BLACK});
    guarantee(out.on_light, BLACK, std::array{WHITE});
    std::vector<RgbColor> dark_refs{WHITE};
    if (out.on_dark) {
        dark_refs.push_back(*out.on_dark);
    }
    guarantee(out.background_dark, BLACK, dark_refs);
    std::vector<RgbColor> light_refs{BLACK};
    if (out.on_light) {
        light_refs.push_back(*out.on_light);
    }
    guarantee(out.background_light, WHITE, light_refs);
    return out;
}

// ==============================================================================
// Palettes
// ==============================================================================

std::vector<RgbColor> quantize_palette(std::span<const uint8_t> rgba, size_t max_colors,
                                       size_t quality) {
    constexpr size_t RGBA = 4;
    std::vector<int64_t> histogram(HISTOGRAM_SIZE, 0);
    Box box;
    box.lo = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(),
              std::numeric_limits<int>::max()};
    box.hi = {0, 0, 0};
    const size_t pixels = rgba.size() / RGBA;
    for (size_t i = 0; i < pixels; i += std::max<size_t>(quality, 1)) {
        const uint8_t* pixel = &rgba[i * RGBA];
        if (pixel[3] < MIN_ALPHA ||
            (pixel[0] > NEAR_WHITE && pixel[1] > NEAR_WHITE && pixel[2] > NEAR_WHITE)) {
            continue;
        }
        const std::array<int, 3> q{pixel[0] >> RSHIFT, pixel[1] >> RSHIFT, pixel[2] >> RSHIFT};
        ++histogram[static_cast<size_t>(color_index(q[0], q[1], q[2]))];
        ++box.count;
        for (size_t axis = 0; axis < 3; ++axis) {
            box.lo[axis] = std::min(box.lo[axis], q[axis]);
            box.hi[axis] = std::max(box.hi[axis], q[axis]);
        }
    }
    if (box.count == 0 || max_colors < 2) {
        return {};
    }
    const MedianCut cut(std::move(histogram));
    std::vector<Box> boxes{box};
    cut.split(boxes, FRACTION_BY_POPULATION * static_cast<double>(max_colors),
              [](const Box& b) { return b.count; });
    cut.split(boxes, static_cast<double>(max_colors),
              [](const Box& b) { return b.count * b.volume(); });
    // The largest count times volume first; equal ones in the order a stable sort leaves them
    std::stable_sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
        return a.count * a.volume() > b.count * b.volume();
    });
    std::vector<RgbColor> palette;
    palette.reserve(boxes.size());
    for (const Box& b : boxes) {
        // A cut can leave a box of no pixel, whose average is no color of the image
        if (b.count != 0) {
            palette.push_back(cut.average(b));
        }
    }
    return palette;
}

Colors palette_colors(std::span<const RgbColor> candidates) {
    if (candidates.empty()) {
        return {};
    }
    Colors colors;
    const RgbColor primary = candidates.front();
    colors.primary = primary;
    colors.on_dark =
        pick_on_color_preferring(candidates, BLACK, std::numeric_limits<double>::infinity());
    colors.on_light = pick_on_color_preferring(candidates, WHITE, MAX_DARK_PICK_CONTRAST);
    if (!colors.on_dark) {
        colors.on_dark =
            toward_contrast(primary, WHITE, std::array{BLACK}, MIN_CONTRAST, PALETTE_STEPS);
    }
    if (!colors.on_light) {
        colors.on_light =
            toward_contrast(primary, BLACK, std::array{WHITE}, MIN_CONTRAST, PALETTE_STEPS);
    }
    std::vector<RgbColor> dark_refs{WHITE};
    if (colors.on_dark) {
        dark_refs.push_back(*colors.on_dark);
    }
    colors.background_dark = background_preferring(primary, BLACK, dark_refs);
    std::vector<RgbColor> light_refs{BLACK};
    if (colors.on_light) {
        light_refs.push_back(*colors.on_light);
    }
    colors.background_light = background_preferring(primary, WHITE, light_refs);
    for (const RgbColor candidate : candidates) {
        const int dr = candidate.r - primary.r;
        const int dg = candidate.g - primary.g;
        const int db = candidate.b - primary.b;
        if (candidate != primary && dr * dr + dg * dg + db * db >= ACCENT_DISTANCE_SQUARED) {
            colors.accent = candidate;
            break;
        }
    }
    return with_contrast(colors);
}

}  // namespace sendspin::server
