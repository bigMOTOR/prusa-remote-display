// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <cmath>

#include "scenes/operational_form.hpp"
#include "theme/theme.hpp"

namespace prd::ui::scene {

// Shared R2 rasterisation seam. Caller-owned storage outlives lv_line's retained pointer.
inline constexpr std::uint32_t kRestingLineCapacity = 64;
inline constexpr std::uint32_t kRestingLineMinimum = 8;
inline constexpr double kRestingLineSpacing = 6.0;

inline std::uint32_t resting_line_points(const Energy energy, const double offset_y,
                                         lv_point_precise_t (&points)[kRestingLineCapacity]) {
    const RestingStroke stroke = resting_stroke(energy);
    const std::int32_t cap_radius = theme::kRingStroke / 2;
    const std::int32_t cap_correction = (theme::kRingStroke % 2) == 0 ? 1 : 0;
    const double left = std::max(stroke.centre_x - stroke.half_chord,
                                 static_cast<double>(theme::kCanvasContent.x + cap_radius));
    const double right = std::min(
        stroke.centre_x + stroke.half_chord,
        static_cast<double>(theme::kCanvasContent.right() - 1 - cap_radius + cap_correction));
    auto count = static_cast<std::uint32_t>(std::ceil((right - left) / kRestingLineSpacing)) + 1U;
    count = std::clamp(count, kRestingLineMinimum, kRestingLineCapacity);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto x = static_cast<lv_value_precise_t>(
            std::lround(left + (right - left) * i / static_cast<double>(count - 1U)));
        const double dx = static_cast<double>(x) - stroke.centre_x;
        double bow = 0.0;
        if (stroke.curvature_sign != 0) {
            bow = (dx * dx) /
                  (stroke.path_radius +
                   std::sqrt(std::max(0.0, stroke.path_radius * stroke.path_radius - dx * dx)));
        }
        points[i] = {x, static_cast<lv_value_precise_t>(std::lround(stroke.midpoint_y + offset_y -
                                                                    stroke.curvature_sign * bow))};
    }
    return count;
}

} // namespace prd::ui::scene
