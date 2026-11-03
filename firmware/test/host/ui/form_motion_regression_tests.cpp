// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "scenes/operational_form.hpp"
#include <cmath>
#include <gtest/gtest.h>

namespace prd::ui::scene {
namespace {
TEST(FormMotionRegression, RoundedRadiusCannotCauseSemicircleAngleCollapse) {
    EXPECT_EQ(form_geometry(496).half_angle, 84);
    EXPECT_EQ(form_geometry(488).half_angle, 83);
}
TEST(FormMotionRegression, FirstOpeningDoesNotTruncateBowBeforeAcos) {
    EXPECT_EQ(form_geometry(999).extent, 352);
    EXPECT_EQ(form_geometry(997).extent, 346);
}
TEST(FormMotionRegression, AllAcceptedEndpointGeometryIsExact) {
    struct Endpoint {
        Energy energy;
        int radius, x, y, angle, rotation;
    };
    constexpr Endpoint points[] = {{0, 693, 160, 889, 12, 258},   {200, 197, 128, 3, 29, 61},
                                   {400, 102, 128, 102, 70, 20},  {600, 96, 128, 112, 97, 353},
                                   {800, 96, 128, 116, 124, 326}, {1000, 96, 128, 120, 180, 270}};
    for (const auto& p : points) {
        const auto f = form_geometry(p.energy);
        EXPECT_EQ(f.radius, p.radius);
        EXPECT_EQ(f.centre_x, p.x);
        EXPECT_EQ(f.centre_y, p.y);
        EXPECT_EQ(f.half_angle, p.angle);
        EXPECT_EQ(f.rotation, p.rotation);
    }
}
TEST(FormMotionRegression, RestingAnchorContinuesAcrossZeroCurvatureWithoutStrokeWidthJump) {
    const double slope =
        static_cast<double>(theme::kRingCentreY + theme::kRingDiameter / 2 - theme::kHorizonApexY) /
            kEnergyMax -
        static_cast<double>(theme::kRingStroke) / kEnergyStopped;
    for (Energy e = 1; e < kEnergyStopped; ++e) {
        const auto a = resting_stroke(e - 1);
        const auto b = resting_stroke(e);
        EXPECT_TRUE(std::isfinite(b.path_radius));
        EXPECT_GE(b.path_radius, b.half_chord);
        EXPECT_GT(b.half_chord, 0.0);
        EXPECT_NEAR(b.midpoint_y - a.midpoint_y, slope, 1e-10);
    }
    EXPECT_EQ(resting_stroke(0).midpoint_y, theme::kHorizonApexY + theme::kRingStroke / 2.0);
    EXPECT_EQ(resting_stroke(kEnergyStopped).midpoint_y,
              midpoint_y(form_geometry(kEnergyStopped)) - theme::kRingStroke / 2.0);
}
} // namespace
} // namespace prd::ui::scene
