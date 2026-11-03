// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

#include "scenes/operational_accent.hpp"
#include "scenes/operational_energy.hpp"
#include "scenes/operational_form.hpp"
#include "theme/layout.hpp"
#include "theme/tokens.hpp"

// The operational form's geometry — UI Design Decision 004 §3.1.
//
// This is the only non-trivial arithmetic in Phase E, and it is tested here rather than on the
// panel for a reason the project has already paid for: the first Phase C ring drew nothing at
// all and passed a clean build, 350 host tests, clang-tidy, the token guard and the
// flush/transfer cross-check. What those checks could not see, a pure function can be asked
// directly.
//
// THE PROPERTY THIS SUITE EXISTS FOR. An earlier law kept the arc centred on its circle's apex
// and let that apex travel from the ring's 24 to the crown's 196. Continuous, monotone, and
// wrong: region.operational occupies y 86..176, so the form was drawn through the words at
// three of the six states. The correction opens the cycle at its top seam instead, which pins
// the arc's centre-column crossing between 196 and 216 for the whole axis. Every assertion
// about the text envelope below is guarding that.
//
// FOUR CLASSES OF PROPERTY, and the distinction matters:
//
//   1. ENDPOINT IDENTITY — exact, against layout.hpp's own constants. Not a tolerance. This is
//      what makes Printing and Ready controls on the panel: they must render exactly as they
//      do today, so any change seen on those two states is a defect rather than a design
//      consequence.
//   2. THE TEXT ENVELOPE — checked at every one of the 1001 energies, not at the six the
//      product currently uses, because the six are a working baseline Phase H will move and
//      §11's motion traverses everything between them.
//   3. CONTINUITY AND VALIDITY — the sagitta and the midpoint move by at most a pixel per
//      energy step, the curvature changes sign without a jump, and no energy produces arc
//      arguments LVGL would collapse.
//   4. NOTHING ABOUT THE TRAJECTORY. §15 reserves "the exact geometry of the form at any
//      energy" for the panel, so the shape at Paused is a working baseline. Monotonicity is
//      asserted; the values are not.

using namespace prd::ui::scene;

namespace {

// ⛔ THERE IS NO LONGER A GLOBAL CENTRE TO ASSUME — owner ruling 2026-08-29. The settled
// horizon is centred on the canvas (160) and every operational state beside the telemetry
// column (128), so a file-scope kCentreX was the same defect the production code carried:
// it measured every form against a circle only five of the six actually sit on. Each helper
// now takes the centre from the form it was handed.
[[nodiscard]] std::int32_t centre_x_of(const FormGeometry& form) {
    return form.centre_x;
}
constexpr std::int32_t kRingRadius = prd::ui::theme::kRingDiameter / 2;
constexpr std::int32_t kRingBottom = prd::ui::theme::kRingCentreY + kRingRadius;

// The half-stroke either side of the nominal circle. What is drawn is a band, not a line.
constexpr double kHalfStroke = prd::ui::theme::kRingStroke / 2.0;

std::vector<Energy> all_energies() {
    std::vector<Energy> energies;
    for (Energy energy = kEnergyMin; energy <= kEnergyMax; ++energy) {
        energies.push_back(energy);
    }

    return energies;
}

// --- The operational text ink envelope --------------------------------------
//
// DERIVED, NOT MEASURED. The host build compiles no LVGL and no fonts, so the width of a drawn
// row cannot be asked for here. What can be derived is the widest ink each row could possibly
// hold: the composition was explicitly sized against the accepted ring's chord rather than its
// inscribed square (see layout.hpp), so the chord at a row's furthest edge bounds that row.
//
// One box per row of the centre stack, in both arrangements the scene lays out — the
// operational one, whose rows are the primary value, the state title and the third line; and
// the resting one, whose two lines centre within the same region. Both are computed from the
// same theme constants the scene uses.
struct Row {
    std::int32_t top;
    std::int32_t bottom;
    const char* name;
};

std::int32_t chord_half_at(const std::int32_t top, const std::int32_t bottom) {
    const std::int32_t furthest = std::max(std::abs(top - prd::ui::theme::kRingCentreY),
                                           std::abs(bottom - prd::ui::theme::kRingCentreY));
    const std::int32_t inner = kRingRadius - prd::ui::theme::kRingStroke;
    const std::int32_t span = (inner * inner) - (furthest * furthest);

    return span > 0 ? static_cast<std::int32_t>(std::sqrt(static_cast<double>(span))) : 0;
}

std::vector<prd::ui::theme::Rect> ink_envelope() {
    const std::int32_t region_top = prd::ui::theme::kRegionOperational.y;
    const std::int32_t region_height = prd::ui::theme::kRegionOperational.height;

    const std::int32_t resting_block = prd::ui::theme::kLineSection + prd::ui::theme::kLineBody;
    const std::int32_t resting_top = region_top + ((region_height - resting_block) / 2);

    // The outcome arrangement puts the same three heights in a different order: the state word
    // first, the record beneath it, the third line last. Its two moved rows are listed here so
    // the envelope covers every position a row can occupy in any arrangement — the record sits
    // 20 px lower than in the operational stack, which is 20 px closer to the chord's narrow end.
    const std::int32_t outcome_value_top = region_top + prd::ui::theme::kLineSection;

    const std::array<Row, 7> rows{{
        {region_top, region_top + prd::ui::theme::kLineHero, "primary value"},
        {region_top + prd::ui::theme::kLineHero,
         region_top + prd::ui::theme::kLineHero + prd::ui::theme::kLineSection, "state title"},
        {region_top + prd::ui::theme::kLineHero + prd::ui::theme::kLineSection,
         region_top + region_height, "third line"},
        {resting_top, resting_top + prd::ui::theme::kLineSection, "resting title"},
        {resting_top + prd::ui::theme::kLineSection, resting_top + resting_block, "resting third"},
        {region_top, outcome_value_top, "outcome title"},
        {outcome_value_top, outcome_value_top + prd::ui::theme::kLineHero, "outcome value"},
    }};

    std::vector<prd::ui::theme::Rect> boxes;
    for (const Row& row : rows) {
        const std::int32_t half = chord_half_at(row.top, row.bottom);
        // ⬅️ THE TEXT'S COLUMN, NOT THE FORM'S. Every row of the composition is centred on
        // kRingCentreX because it shares the canvas with the telemetry region; only the form
        // moves off that column, and only at rest, where no row is drawn at all.
        boxes.push_back(prd::ui::theme::Rect{prd::ui::theme::kRingCentreX - half, row.top, 2 * half,
                                             row.bottom - row.top});
    }

    return boxes;
}

// The arc's drawn points, sampled finely enough that no text row could hide between them.
std::vector<std::pair<double, double>> arc_points(const FormGeometry& form) {
    constexpr double kPi = 3.14159265358979323846;
    const auto samples = static_cast<std::int32_t>(form.extent * 8) + 1;

    std::vector<std::pair<double, double>> points;
    points.reserve(static_cast<std::size_t>(samples) + 1);

    for (std::int32_t index = 0; index <= samples; ++index) {
        const double degrees = form.rotation + (static_cast<double>(form.extent) * index / samples);
        const double radians = degrees * kPi / 180.0;

        points.emplace_back(centre_x_of(form) + (form.radius * std::cos(radians)),
                            form.centre_y + (form.radius * std::sin(radians)));
    }

    return points;
}

// True distance from a point to a rectangle — not an axis-by-axis inflation, which reports a
// diagonal near-miss at a corner as a hit.
double distance_to(const prd::ui::theme::Rect& box, const double point_x, const double point_y) {
    const double dx = std::max({box.x - point_x, 0.0, point_x - box.right()});
    const double dy = std::max({box.y - point_y, 0.0, point_y - box.bottom()});

    return std::sqrt((dx * dx) + (dy * dy));
}

double clearance(const FormGeometry& form, const std::vector<prd::ui::theme::Rect>& boxes) {
    double worst = 1e9;
    for (const auto& [point_x, point_y] : arc_points(form)) {
        for (const auto& box : boxes) {
            worst = std::min(worst, distance_to(box, point_x, point_y));
        }
    }

    return worst;
}

bool encloses(const FormGeometry& form, const prd::ui::theme::Rect& box) {
    for (const std::int32_t corner_x : {box.x, box.right()}) {
        for (const std::int32_t corner_y : {box.y, box.bottom()}) {
            const double dx = corner_x - centre_x_of(form);
            const double dy = corner_y - form.centre_y;
            if (std::sqrt((dx * dx) + (dy * dy)) >= form.radius - kHalfStroke) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

// --- 1. Endpoint identity, exact --------------------------------------------

TEST(OperationalFormTests, MaximumEnergyReproducesTheAcceptedRingExactly) {
    // UI Design Decision 001's composition, as layout.hpp derives it. Exact equality, because
    // the whole point of anchoring the axis here is that Printing does not move.
    const FormGeometry form = form_geometry(kEnergyMax);

    EXPECT_EQ(form.radius, kRingRadius);
    EXPECT_EQ(form.centre_y, prd::ui::theme::kRingCentreY);
    EXPECT_EQ(drawn_top_y(form), prd::ui::theme::kRegionPrimary.y);
    EXPECT_EQ(drawn_bottom_y(form), kRingBottom);
}

TEST(OperationalFormTests, MaximumEnergyIsAClosedCycleDrawnFromTwelveOClock) {
    // §3.1: at maximum energy the form is "closed and dominant, describing a complete cycle of
    // work". The rotation is the ring's existing origin, so the record's sweep starts where
    // UIDD 001's artwork starts it and today's composition is reproduced argument for argument.
    const FormGeometry form = form_geometry(kEnergyMax);

    EXPECT_TRUE(form.closed);
    EXPECT_EQ(form.extent, 360);
    EXPECT_EQ(form.rotation, 270);
    EXPECT_EQ(form.half_angle, 180);
}

TEST(OperationalFormTests, MinimumEnergyReproducesTheAcceptedHorizonExactly) {
    // UI Design Decision 002's resting expression, as layout.hpp derives it: radius 693, centre
    // sunk to 889, a 12-degree half-angle either side of the apex. Exact, for the same reason.
    const FormGeometry form = form_geometry(kEnergyMin);

    EXPECT_EQ(form.radius, prd::ui::theme::kHorizonRadius);
    EXPECT_EQ(form.centre_y, prd::ui::theme::kHorizonCentreY);
    EXPECT_EQ(form.half_angle, prd::ui::theme::kHorizonHalfAngle);
    EXPECT_EQ(midpoint_y(form), prd::ui::theme::kHorizonApexY);
    EXPECT_EQ(end_y(form), prd::ui::theme::kHorizonEndY);
}

TEST(OperationalFormTests, MinimumEnergyIsDrawnWithTheAcceptedCrownAngles) {
    // The accepted horizon spans 258..282 degrees. Expressed as (0, 24) placed at 258, which is
    // the same set of drawn points and the form the collapse guard requires.
    const FormGeometry form = form_geometry(kEnergyMin);

    EXPECT_FALSE(form.closed);
    EXPECT_EQ(form.rotation, prd::ui::theme::kHorizonStartAngle);
    EXPECT_EQ(form.rotation + form.extent, prd::ui::theme::kHorizonEndAngle);
}

// LVGL 9.5.0's sin0_90_table, transcribed verbatim from
// managed_components/lvgl__lvgl/src/misc/lv_math.c. It lives HERE, in a test, and deliberately
// not in product code: the production helper computes the same values arithmetically, and this
// table is the evidence that it may.
constexpr std::int32_t kVendorSinTable[] = {
    0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,  5690,  6252,  6813,
    7371,  7927,  8481,  9032,  9580,  10126, 10668, 11207, 11743, 12275, 12803, 13328, 13848,
    14365, 14876, 15384, 15886, 16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174,
    20622, 21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730, 25102, 25466,
    25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088, 28378, 28660, 28932, 29197, 29452,
    29698, 29935, 30163, 30382, 30592, 30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928,
    32052, 32166, 32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763, 32768};

// lv_trigo_sin(), transcribed from the same file.
[[nodiscard]] std::int32_t vendor_trigo_sin(std::int32_t angle) {
    while (angle < 0) {
        angle += 360;
    }
    while (angle >= 360) {
        angle -= 360;
    }

    std::int32_t ret = 0;
    if (angle < 90) {
        ret = kVendorSinTable[angle];
    } else if (angle < 180) {
        ret = kVendorSinTable[180 - angle];
    } else if (angle < 270) {
        ret = -kVendorSinTable[angle - 180];
    } else {
        ret = -kVendorSinTable[360 - angle];
    }

    if (ret == 32767) {
        return 32768;
    }
    if (ret == -32767) {
        return -32768;
    }
    return ret;
}

// get_rounded_area()'s x1/x2 for one cap, from lv_draw_sw_arc.c.
struct VendorColumns {
    std::int32_t left;
    std::int32_t right;
};

[[nodiscard]] VendorColumns vendor_cap(std::int32_t angle, std::int32_t radius,
                                       std::int32_t thickness) {
    const std::int32_t thick_half = thickness / 2;
    const std::int32_t thick_corr = (thickness & 0x01) != 0 ? 0 : 1;

    std::int32_t cir_x = ((radius - thick_half) * vendor_trigo_sin(angle + 90)) >> (15 - 8);

    if (cir_x > 0) {
        cir_x = (cir_x - 128) >> 8;
        return {cir_x - thick_half + thick_corr, cir_x + thick_half};
    }

    cir_x = (cir_x + 128) >> 8;
    return {cir_x - thick_half, cir_x + thick_half - thick_corr};
}

TEST(OperationalFormTests, ThePaintBoundsReproduceThePinnedRendererExactly) {
    // ⛔ THE CLAIM THIS TEST EXISTS TO EARN. The production helper computes LVGL's fixed-point
    // sine arithmetically instead of carrying its table, and describes its result as the pinned
    // renderer's own column bounds. That is only allowed if it actually agrees with the vendor's
    // code, so the vendor's table and cap arithmetic are transcribed above and compared here.
    //
    // The predecessor of these functions was a symmetric floating-point approximation described
    // as the exact lit pixels. It was one pixel short of the renderer's true left edge at 319 of
    // the 1001 reachable energies — safe-looking, and wrong.
    for (std::int32_t degrees = 0; degrees < 360; ++degrees) {
        const FormGeometry probe{100, 0, 0, 0, 1, -1, 2, degrees, false};

        // Exercises the reproduced sine at every whole degree through the public helpers.
        const VendorColumns start = vendor_cap(degrees, probe.radius, prd::ui::theme::kRingStroke);
        const VendorColumns end =
            vendor_cap(degrees + probe.extent, probe.radius, prd::ui::theme::kRingStroke);

        const bool covers_left = degrees <= 180 && 180 <= degrees + probe.extent;
        const bool covers_right = degrees == 0 || degrees + probe.extent >= 360;

        const std::int32_t want_left = covers_left ? -probe.radius : std::min(start.left, end.left);
        const std::int32_t want_right =
            covers_right ? probe.radius : std::max(start.right, end.right);

        EXPECT_EQ(paint_left_offset(probe, prd::ui::theme::kRingStroke), want_left)
            << "rotation " << degrees;
        EXPECT_EQ(paint_right_offset(probe, prd::ui::theme::kRingStroke), want_right)
            << "rotation " << degrees;
    }

    // And over every geometry the axis actually produces.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);

        if (form.closed) {
            EXPECT_EQ(paint_left_offset(form, prd::ui::theme::kRingStroke), -form.radius);
            EXPECT_EQ(paint_right_offset(form, prd::ui::theme::kRingStroke), form.radius);
            continue;
        }

        const VendorColumns start =
            vendor_cap(form.rotation, form.radius, prd::ui::theme::kRingStroke);
        const VendorColumns end =
            vendor_cap(form.rotation + form.extent, form.radius, prd::ui::theme::kRingStroke);

        bool covers_left = false;
        bool covers_right = false;
        for (std::int32_t step = 0; step <= form.extent; ++step) {
            const std::int32_t bearing = (form.rotation + step) % 360;
            covers_left = covers_left || bearing == 180;
            covers_right = covers_right || bearing == 0;
        }

        EXPECT_EQ(paint_left_offset(form, prd::ui::theme::kRingStroke),
                  covers_left ? -form.radius : std::min(start.left, end.left))
            << "energy " << energy;
        EXPECT_EQ(paint_right_offset(form, prd::ui::theme::kRingStroke),
                  covers_right ? form.radius : std::max(start.right, end.right))
            << "energy " << energy;
    }
}

TEST(OperationalFormTests, SettledReadyDrawsTheAcceptedF1Horizon) {
    // THE CONTRACT RECOVERED FROM docs/project-sources/assets/Ready Resting F1.jpeg, owner ruling
    // 2026-08-29. Centred on the whole canvas and running to the content box either side — not
    // on the ring's column at the ring's width, which is what this used to draw.
    const FormGeometry form = form_geometry(kEnergyReady);

    EXPECT_EQ(form.centre_x, 160) << "the settled horizon is centred on the 320 px canvas";

    // ⛔ THREE WIDTHS, ASSERTED SEPARATELY. Conflating them is how this checkpoint first claimed
    // the ink reached 12..308 while it actually reached 10..310.
    EXPECT_EQ(form.half_width, 148) << "the sagitta relation's input";
    EXPECT_EQ(half_extent(form), 144) << "the nominal path";
    // ⬅️ AND THE RENDERER'S OWN COLUMNS ARE WHAT §7 CONSTRAINS. Asymmetric, because LVGL's
    // half-pixel correction is sign-dependent and an even stroke corrects one side only.
    // Rect::right() is exclusive, so the last content pixel is 307.
    const std::int32_t left = form.centre_x + paint_left_offset(form, prd::ui::theme::kRingStroke);
    const std::int32_t right =
        form.centre_x + paint_right_offset(form, prd::ui::theme::kRingStroke);

    EXPECT_EQ(left, 13);
    EXPECT_EQ(right, 306);
    EXPECT_GE(left, prd::ui::theme::kCanvasContent.x);
    EXPECT_LT(right, prd::ui::theme::kCanvasContent.right());

    // Apex and stroke were already correct and must not have moved. The end sits one pixel
    // higher than the relation's 16 px input because the half-angle floors — recorded, not hidden.
    EXPECT_EQ(midpoint_y(form), 196);
    EXPECT_EQ(end_y(form), 211);
    EXPECT_EQ(end_y(form) - midpoint_y(form), prd::ui::theme::kHorizonRealisedSag);

    // ⛔ THE DEFECT THIS REPLACES, NAMED SO IT CANNOT COME BACK QUIETLY. The old horizon was
    // 192 px wide about x 128 — x 32..224.
    EXPECT_NE(form.centre_x, 128) << "the ring's column is not the canvas centre";
    EXPECT_NE(half_extent(form), 96) << "the ring's width is not the canvas width";
    EXPECT_NE(left, 32);
    EXPECT_NE(right, 224);
}

TEST(OperationalFormTests, OnlyTheRestingSegmentMovedHorizontally) {
    // The five accepted operational states keep the ring's own centre and width exactly. The
    // anchor is what makes that true by construction rather than by luck.
    const Energy preserved[] = {kEnergyStopped, kEnergyPreparing, kEnergyCompleted, kEnergyPaused,
                                kEnergyPrinting};

    for (const Energy energy : preserved) {
        const FormGeometry form = form_geometry(energy);

        EXPECT_EQ(form.centre_x, prd::ui::theme::kRingCentreX) << "energy " << energy;
        EXPECT_EQ(form.half_width, prd::ui::theme::kRingDiameter / 2) << "energy " << energy;
        EXPECT_EQ(half_extent(form), prd::ui::theme::kRingDiameter / 2) << "energy " << energy;
    }

    // Above the anchor nothing varies at all: one value, held.
    for (Energy energy = kEnergyStopped; energy <= kEnergyMax; ++energy) {
        EXPECT_EQ(form_geometry(energy).centre_x, prd::ui::theme::kRingCentreX) << energy;
        EXPECT_EQ(form_geometry(energy).half_width, prd::ui::theme::kRingDiameter / 2) << energy;
    }
}

TEST(OperationalFormTests, TheHorizontalAxisIsContinuousAcrossTheAnchor) {
    // §11's motion traverses this, so a jump would be visible as the form snapping width or
    // sliding sideways. One pixel per energy step is the bound; the anchor itself is where a
    // careless implementation would step.
    std::int32_t previous_centre = form_geometry(kEnergyMin).centre_x;
    std::int32_t previous_width = form_geometry(kEnergyMin).half_width;

    for (Energy energy = kEnergyMin + 1; energy <= kEnergyMax; ++energy) {
        const FormGeometry form = form_geometry(energy);

        EXPECT_LE(std::abs(form.centre_x - previous_centre), 1) << "energy " << energy;
        EXPECT_LE(std::abs(form.half_width - previous_width), 1) << "energy " << energy;

        previous_centre = form.centre_x;
        previous_width = form.half_width;
    }

    // The join itself, stated as the values either side of it.
    EXPECT_EQ(form_geometry(kEnergyStopped - 1).centre_x, 129);
    EXPECT_EQ(form_geometry(kEnergyStopped).centre_x, prd::ui::theme::kRingCentreX);
    EXPECT_EQ(form_geometry(kEnergyStopped - 1).half_width, 97);
    EXPECT_EQ(form_geometry(kEnergyStopped).half_width, prd::ui::theme::kRingDiameter / 2);
}

std::int32_t quantised_reach_bound(const Energy energy, const FormGeometry& form) {
    if (energy >= kEnergyStopped && form.sagitta <= form.half_width) {
        // Nearest-degree shallow arcs have a two-sided angular error of at most half a
        // degree. Derive the chord bound from that angle, not a fitted pixel allowance.
        constexpr double kPi = 3.14159265358979323846;
        const double angle = 2.0 * std::atan2(static_cast<double>(form.sagitta), form.half_width);
        const double upper = std::min(kPi / 2.0, angle + kPi / 360.0);
        return static_cast<std::int32_t>(std::lround(form.radius * std::sin(upper)));
    }
    return form.half_width;
}

TEST(OperationalFormTests, TheDrawnChordRespectsItsQuantiserAndCanvasBounds) {
    // ⛔ THE FLAT-CROSSING OVERFLOW THIS CHECKPOINT FIXED. The half-angle floors, and the radius
    // is capped so that even the minimum one-degree angle cannot subtend more than the half
    // width. Before both, energies 8..9 and 73..75 drew past the canvas edge — 73..75 reached
    // x = -1, off the display entirely.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);

        EXPECT_LE(half_extent(form), quantised_reach_bound(energy, form)) << energy;
        EXPECT_GT(half_extent(form), 0) << "energy " << energy << " draws nothing";
        EXPECT_GE(form.centre_x + paint_left_offset(form, prd::ui::theme::kRingStroke),
                  prd::ui::theme::kCanvasContent.x)
            << energy;
        EXPECT_LT(form.centre_x + paint_right_offset(form, prd::ui::theme::kRingStroke),
                  prd::ui::theme::kCanvasContent.right())
            << energy;
    }
}

TEST(OperationalFormTests, NoProductionMeasurementHasChanged) {
    // The corrective checkpoint changes the transformation LAW and nothing else. Every value the
    // derivation is anchored to is still the accepted one, asserted here so a token edit
    // disguised as a geometry change cannot pass.
    EXPECT_EQ(prd::ui::theme::kRingDiameter, 192);
    EXPECT_EQ(prd::ui::theme::kRingCentreX, 128);
    EXPECT_EQ(prd::ui::theme::kRingCentreY, 120);
    EXPECT_EQ(prd::ui::theme::kRingStroke, 8);
    // ⛔ NARROWED, NOT DELETED — owner ruling 2026-08-29. This test's job is to stop a token
    // edit passing itself off as a geometry change, and it still does that. What moved is the
    // settled horizon's WIDTH and CENTRE, measured from the accepted F1 reference; its apex,
    // end, sag and stroke are untouched below, and so is every ring value.
    EXPECT_EQ(prd::ui::theme::kHorizonCentreX, 160);
    EXPECT_EQ(prd::ui::theme::kHorizonHalfWidth, 148);
    EXPECT_EQ(prd::ui::theme::kHorizonHalfExtent, 144);
    EXPECT_EQ(prd::ui::theme::kHorizonPaintedHalfExtent, 147);
    EXPECT_EQ(prd::ui::theme::kHorizonRadius, 693);
    EXPECT_EQ(prd::ui::theme::kHorizonCentreY, 889);
    EXPECT_EQ(prd::ui::theme::kHorizonRealisedSag, 15);
    EXPECT_EQ(prd::ui::theme::kHorizonApexY, 196);
    EXPECT_EQ(prd::ui::theme::kHorizonEndY, 211);
    EXPECT_EQ(prd::ui::theme::kHorizonSag, 16);
    EXPECT_EQ(prd::ui::theme::kHorizonHalfAngle, 12);
    EXPECT_EQ(prd::ui::theme::kRegionOperational.y, 86);
    EXPECT_EQ(prd::ui::theme::kRegionOperational.bottom(), 176);
}

TEST(OperationalFormTests, EnergiesOutsideTheScaleClampToTheEndpoints) {
    // form_geometry() takes an Energy rather than an optional: a caller resolves membership
    // first. An out-of-range value is a caller defect, and clamping keeps it from becoming a
    // geometry defect on the panel.
    EXPECT_EQ(form_geometry(kEnergyMin - 1).radius, form_geometry(kEnergyMin).radius);
    EXPECT_EQ(form_geometry(kEnergyMax + 1).radius, form_geometry(kEnergyMax).radius);
    EXPECT_EQ(form_geometry(kEnergyMin - 1000).centre_y, form_geometry(kEnergyMin).centre_y);
    EXPECT_EQ(form_geometry(kEnergyMax + 1000).centre_y, form_geometry(kEnergyMax).centre_y);
}

// --- 2. The form never crosses the operational text -------------------------

TEST(OperationalFormTests, TheCentreColumnCrossingNeverEntersTheTextRegion) {
    // THE CORRECTION, STATED EXACTLY. The arc crosses the composition's centre column at one
    // point, and that point stays between the resting crown's apex and the ring's own bottom —
    // entirely below region.operational — for every energy on the axis.
    //
    // The superseded law failed precisely here: its crossing travelled from 24 to 196 and took
    // every value in between, so the form passed through the middle of the words.
    for (const Energy energy : all_energies()) {
        const std::int32_t crossing = midpoint_y(form_geometry(energy));

        EXPECT_GT(crossing, prd::ui::theme::kRegionOperational.bottom())
            << "energy " << energy << " draws the form through the operational text";
        EXPECT_GE(crossing, prd::ui::theme::kHorizonApexY) << "energy " << energy;
        EXPECT_LE(crossing, kRingBottom) << "energy " << energy;
    }
}

TEST(OperationalFormTests, TheFormNeverEntersTheOperationalTextInkEnvelope) {
    // The whole arc, not just its centre-column crossing: every drawn point of every open form
    // is checked against every row of the centre stack in both arrangements.
    //
    // The closed form is the exception, and not a carve-out: at maximum energy the text is
    // INSIDE the ring by design (UIDD 001 puts the percentage in the centre), so the property
    // there is containment rather than separation. It is asserted as such below.
    const std::vector<prd::ui::theme::Rect> envelope = ink_envelope();

    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);
        if (form.closed) {
            continue;
        }

        EXPECT_GT(clearance(form, envelope), 0.0)
            << "energy " << energy << " draws the form into a row of the centre stack";
    }
}

TEST(OperationalFormTests, TheClosedFormContainsTheTextRatherThanTouchingIt) {
    // Maximum energy: the accepted composition, where every row sits inside the ring's chord.
    // Asserted so that "the form never crosses the text" is a complete statement across the
    // axis rather than one with a silent exception at the top of it.
    const FormGeometry form = form_geometry(kEnergyMax);
    ASSERT_TRUE(form.closed);

    for (const auto& box : ink_envelope()) {
        EXPECT_TRUE(encloses(form, box)) << "a row of the centre stack is not inside the ring";
    }
}

TEST(OperationalFormTests, TheFormNeverLeavesTheContentBox) {
    // Production UI §7: "All content shall remain inside `canvas.inset`. No component may touch
    // the display edge."
    //
    // The arc's bounding box does extend off-canvas — a kHorizonRadius-sized circle on a
    // 240 px display, which LVGL clips (§11) — but what is DRAWN must stay inside.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);

        EXPECT_GE(drawn_top_y(form), prd::ui::theme::kCanvasContent.y) << "energy " << energy;
        EXPECT_LE(drawn_bottom_y(form), prd::ui::theme::kCanvasContent.bottom())
            << "energy " << energy;

        const std::int32_t reach = half_extent(form);
        EXPECT_GE(form.centre_x - reach, prd::ui::theme::kCanvasContent.x) << "energy " << energy;
        EXPECT_LE(form.centre_x + reach, prd::ui::theme::kCanvasContent.right())
            << "energy " << energy;

        // ⛔ AND THE INK, WHICH IS WHAT §7 ACTUALLY CONSTRAINS. Asserting only the nominal path
        // left a real hole: with nearest-degree rounding the form at energy 5 kept a nominal
        // reach of 147 — inside the box — while painting 150 either side, at x 10..310. The
        // caps reach past the path, so a test that stops at the path cannot see the escape.
        // Rect::right() is exclusive, so the last permitted content pixel is right() - 1.
        const std::int32_t paint_l =
            form.centre_x + paint_left_offset(form, prd::ui::theme::kRingStroke);
        const std::int32_t paint_r =
            form.centre_x + paint_right_offset(form, prd::ui::theme::kRingStroke);

        EXPECT_GE(paint_l, prd::ui::theme::kCanvasContent.x)
            << "energy " << energy << " paints to " << paint_l;
        EXPECT_LT(paint_r, prd::ui::theme::kCanvasContent.right())
            << "energy " << energy << " paints to " << paint_r;
    }
}

// --- 3. Continuity, reachability and valid arc arguments --------------------

TEST(OperationalFormTests, NoEnergyProducesAnArcAngleThatLvglWouldCollapse) {
    // THE DEFECT CLASS THIS EXISTS FOR. lv_arc_set_bg_end_angle() contains a single
    // `if (end > 360) end -= 360;` — a subtraction, not a normalising loop. An end angle of 361
    // becomes 1 and one of 630 becomes 270, and if the result equals the start the arc has zero
    // extent and draws nothing at all: no track, no indicator, no error, and every automated
    // check passes. That is exactly what the first Phase C build shipped.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);

        EXPECT_GT(form.extent, 0) << "energy " << energy << " draws a zero-extent arc";
        EXPECT_LE(form.extent, 360) << "energy " << energy << " would be wrapped by LVGL";
        EXPECT_GE(form.rotation, 0) << "energy " << energy;
        EXPECT_LT(form.rotation, 360) << "energy " << energy;
        EXPECT_GT(form.radius, 0) << "energy " << energy;
    }
}

TEST(OperationalFormTests, TheFormIsContinuousAcrossTheWholeAxis) {
    // Continuity, in the only form a discrete axis can carry it: neither the shape parameter nor
    // the drawn crossing may move by more than a pixel between adjacent energies. §3.1 requires
    // the intermediate shapes to be reachable, and §11 will animate along this axis — a jump
    // here would be a jump on the panel.
    const std::vector<Energy> energies = all_energies();

    for (std::size_t index = 1; index < energies.size(); ++index) {
        const FormGeometry lower = form_geometry(energies[index - 1]);
        const FormGeometry higher = form_geometry(energies[index]);

        EXPECT_LE(std::abs(higher.sagitta - lower.sagitta), 1) << "at energy " << energies[index];
        EXPECT_LE(std::abs(midpoint_y(higher) - midpoint_y(lower)), 1)
            << "at energy " << energies[index];
    }
}

TEST(OperationalFormTests, TheCurvatureChangesSignWithoutADiscontinuity) {
    // The form passes through a straight horizon on its way to the accepted crown, so the
    // circle's centre crosses from one side of the arc to the other. That is a sign change in
    // the sagitta, and it must be a crossing rather than a flip: at the energies either side of
    // it the drawn arc has to be nearly identical.
    const std::vector<Energy> energies = all_energies();
    std::vector<Energy> sign_changes;

    for (std::size_t index = 1; index < energies.size(); ++index) {
        const FormGeometry lower = form_geometry(energies[index - 1]);
        const FormGeometry higher = form_geometry(energies[index]);

        if ((lower.sagitta > 0) != (higher.sagitta > 0)) {
            sign_changes.push_back(energies[index]);

            // THE CROSSING HAPPENS AT THE FLATTEST SHAPE THE PIXEL GRID CAN HOLD, which is what
            // makes it a crossing rather than a flip. Either side of it the form bows by a
            // single pixel — down on one side, up on the other — so:
            //
            //   * the midpoint moves by at most a pixel, like everywhere else on the axis;
            //   * the arc is within one pixel of straight on BOTH sides;
            //   * the ends therefore move by two pixels, from one pixel below the midpoint to
            //     one pixel above it. Two is the minimum a sign change can cost on an integer
            //     grid — a zero-bow arc is not a circle — so this is the floor, not a tolerance.
            EXPECT_LE(std::abs(midpoint_y(higher) - midpoint_y(lower)), 1);
            EXPECT_LE(std::abs(midpoint_y(higher) - end_y(higher)), 1);
            EXPECT_LE(std::abs(midpoint_y(lower) - end_y(lower)), 1);
            EXPECT_LE(std::abs(end_y(higher) - end_y(lower)), 2);
        }
    }

    // Exactly one crossing: the form flattens once and settles, rather than oscillating.
    EXPECT_EQ(sign_changes.size(), 1U);
}

TEST(OperationalFormTests, EveryIntermediateShapeIsReachable) {
    // §3.1: "Every intermediate shape is reachable and legitimate. A form that exists only at
    // its extremes is two forms."
    //
    // The falsifiable reading: the axis must produce many distinct shapes, not two. A binary
    // would score 2 here — which is what the implementation scored before Phase E.
    std::vector<std::array<std::int32_t, 4>> shapes;
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);
        const std::array<std::int32_t, 4> shape{form.radius, form.centre_y, form.half_angle,
                                                form.rotation};
        if (shapes.empty() || shapes.back() != shape) {
            shapes.push_back(shape);
        }
    }

    EXPECT_GT(shapes.size(), 100U) << "the form has " << shapes.size() << " distinct shapes";
}

TEST(OperationalFormTests, TheFormOpensMonotonicallyAsEnergyFalls) {
    // §3.1: "as energy falls it opens". Read upward, as energy RISES the gap closes, so the
    // drawn extent grows. The radius and centre are NOT asserted monotone: the form passes
    // through a straight horizon, where the radius is at its largest and the centre is furthest
    // away, so both are non-monotone by construction and the sagitta is what carries the axis.
    const std::vector<Energy> energies = all_energies();

    for (std::size_t index = 1; index < energies.size(); ++index) {
        const FormGeometry lower = form_geometry(energies[index - 1]);
        const FormGeometry higher = form_geometry(energies[index]);

        EXPECT_GE(higher.sagitta, lower.sagitta) << "at energy " << energies[index];
        EXPECT_GE(midpoint_y(higher), midpoint_y(lower)) << "at energy " << energies[index];
    }
}

TEST(OperationalFormTests, TheSixCurrentEnergiesProduceSixDistinctShapes) {
    // The states the product draws today, each at its own shape — including Stopped below
    // Preparing, which is UIDD 004 §2.2's ordering. The values are NOT asserted: the magnitudes
    // are a working baseline and §15 reserves the geometry at any energy for the panel. What is
    // asserted is that no two states collapse onto one shape.
    const std::vector<Energy> six{kEnergyReady,     kEnergyStopped, kEnergyPreparing,
                                  kEnergyCompleted, kEnergyPaused,  kEnergyPrinting};

    for (std::size_t index = 1; index < six.size(); ++index) {
        EXPECT_LT(form_geometry(six[index - 1]).sagitta, form_geometry(six[index]).sagitta)
            << "two operational states share a shape at index " << index;
    }
}

TEST(OperationalFormTests, TheFormHoldsItsWidthExceptWhereIntegerDegreesCannot) {
    // ⛔ NARROWED, NOT DELETED — owner ruling 2026-08-29. This used to assert the ring's width
    // at EVERY energy, which was true only while one width served the whole axis. The settled
    // horizon is now the canvas's width, so the invariant is restated against the width the form
    // was actually solved for: it holds ITS OWN width, whatever that is at this energy. That is
    // the property that stops the form growing off a 320 px canvas, and it still holds.
    //
    // IT IS NOT EXACT EVERYWHERE, AND THE EXCEPTION IS RECORDED RATHER THAN ROUNDED AWAY. LVGL
    // takes arc angles in whole degrees. Near the straight horizon the form's width subtends
    // only a couple of degrees, so a degree of flooring costs real width. Away from that band —
    // everywhere the form's bow is at least the accepted resting sag — the width holds to the
    // same two pixels the accepted endpoints themselves exhibit.
    //
    // Floor-quantised branches retain one-sided error. The repaired upper shallow branch
    // rounds to nearest; its two-sided error is bounded analytically below.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);
        const std::int32_t reach = half_extent(form);

        EXPECT_LE(reach, quantised_reach_bound(energy, form)) << energy;

        // ⬅️ AND THE SHORTFALL IS BOUNDED BY WHAT ONE DEGREE IS WORTH HERE, which is the honest
        // statement of the limit and replaces two magic pixel tolerances. A degree of chord is
        // about 2 px at the ring's radius and about 12 px at the resting crown's, so a single
        // fixed allowance could only ever be wrong at one end of the axis.
        constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;
        const double here = form.radius * std::sin(form.half_angle * kDegreesToRadians);
        const double next = form.radius * std::sin((form.half_angle + 1) * kDegreesToRadians);

        // Past the quarter turn sin decreases, so a raw difference would go negative; there the
        // arc contains the circle's widest points, the reach IS the radius, and the shortfall is
        // zero by construction. Clamped rather than branched, because the claim is the same one.
        EXPECT_LT(form.half_width - reach, std::max(0.0, next - here) + 1.0)
            << "energy " << energy << " reaches " << reach << " of " << form.half_width
            << ", more than one degree short";
    }
}

// --- Where the arc is actually placed ---------------------------------------

TEST(OperationalFormTests, BothEndpointsArePlacedInTheirAcceptedRectangles) {
    // bounding_box() is the function that positions the arc on the panel, and until now nothing
    // checked it. An error here does not fail a build or a geometry assertion — it draws the
    // form in the wrong place, which is the exact class of defect that cost this project a whole
    // flash cycle when the first ring drew nothing at all.
    //
    // Both endpoints must land in the rectangles the composition has always used: the ring in
    // region.primary, and the resting crown in the horizon's own box. Together with the endpoint
    // identity above, this is what makes "Printing and Ready render exactly as they did"
    // a checked claim rather than a hoped-for one.
    EXPECT_EQ(bounding_box(form_geometry(kEnergyMax)), prd::ui::theme::kRegionPrimary);
    EXPECT_EQ(bounding_box(form_geometry(kEnergyMin)), prd::ui::theme::kRegionHorizon);
}

TEST(OperationalFormTests, EveryFormIsPlacedConcentricallyOnTheCompositionsCentreLine) {
    // §6's concentric construction: whatever the shape, its circle is centred on the same
    // vertical line the composition is built around. A box that drifted sideways would move the
    // form off the centre column and take the whole family with it.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);
        const prd::ui::theme::Rect box = bounding_box(form);

        EXPECT_EQ(box.x + (box.width / 2), form_geometry(energy).centre_x) << "energy " << energy;
        EXPECT_EQ(box.y + (box.height / 2), form.centre_y) << "energy " << energy;
        EXPECT_EQ(box.width, 2 * form.radius) << "energy " << energy;
        EXPECT_EQ(box.height, box.width) << "energy " << energy;
    }
}

// --- The diagnostic read-back -----------------------------------------------
//
// form_report() fills ui::OperationalFormReport, which is what the renderer publishes and the
// operational walk reads back so a photograph can be checked against the form the firmware
// actually drew.
//
// IT IS TESTED HERE BECAUSE NOTHING ELSE CAN SEE IT. The report is a fourteen-field
// transcription, and a transcription is the one kind of error that survives every gate this
// project runs: it compiles, it emits entirely plausible numbers, and a swapped pair would first
// be noticed by a reviewer trusting a wrong figure against a photograph. A lying diagnostic is
// worse than none — this repository has paid for that twice.
//
// Nothing below asserts a VALUE. The report must agree with what it was handed, whatever that
// is; the geometry's and the ramp's own values are the business of their own suites.

namespace {

// A colour the accent rule never produces, used where a test needs to prove the report did not
// simply copy what it was handed. Deliberately not the accent: `color.accent` has red as its
// largest channel, so pure blue cannot be mistaken for it.
constexpr std::uint32_t kAnyAccent = 0x0000FF;

} // namespace

TEST(OperationalFormReportTests, NothingIsReportedWhenNoFormIsDrawn) {
    // A state outside the Operational Family has no energy at all — energy_for() returns nothing
    // rather than the minimum, because "not on the axis" and "at the bottom of it" are different
    // facts. The report has to preserve that distinction: Ready is drawn, a non-member is not.
    //
    // Handed a real colour deliberately: a form that is not on the panel was drawn in no colour,
    // and the report must say so rather than pass the argument through.
    const prd::ui::OperationalFormReport report =
        form_report(std::nullopt, std::nullopt, kAnyAccent, false);

    EXPECT_FALSE(report.drawn);
    EXPECT_EQ(report.energy, 0);
    EXPECT_EQ(report.radius, 0);
    EXPECT_EQ(report.accent, 0U);
    EXPECT_FALSE(report.closed);
    EXPECT_FALSE(report.draws_record);
}

TEST(OperationalFormReportTests, ANoDrawnFormIsNeverReportedAsCarryingTheRecord) {
    // §3.2's sweep cannot be carrying a record on a form that is not on the panel. Passed true
    // deliberately: the report must not simply copy what it was handed.
    EXPECT_FALSE(form_report(std::nullopt, std::nullopt, kAnyAccent, true).draws_record);
}

TEST(OperationalFormReportTests, AHalfFilledPairIsReportedAsNothingDrawn) {
    // The energy and the geometry are set together and cleared together by the one call that
    // places the form, so one without the other means they have drifted apart. That is a defect
    // in the retaining component, and the report must not paper over it by publishing half a
    // record with the other half defaulted to zero — which would put a real energy beside a
    // radius of 0 and read as a collapsed form.
    const FormGeometry ring = form_geometry(kEnergyMax);

    EXPECT_FALSE(form_report(kEnergyMax, std::nullopt, kAnyAccent, true).drawn);
    EXPECT_FALSE(form_report(std::nullopt, ring, kAnyAccent, true).drawn);
}

TEST(OperationalFormReportTests, EveryFieldAgreesWithTheGeometryAtEveryEnergy) {
    // The whole axis, not the six magnitudes the product currently uses: those are a working
    // baseline Phase H will move, and §11's motion traverses everything between them.
    for (const Energy energy : all_energies()) {
        const FormGeometry form = form_geometry(energy);
        const prd::ui::OperationalFormReport report = form_report(energy, form, accent(), false);

        EXPECT_TRUE(report.drawn) << "energy " << energy;
        EXPECT_EQ(report.energy, energy);

        EXPECT_EQ(report.radius, form.radius) << "energy " << energy;
        EXPECT_EQ(report.centre_y, form.centre_y) << "energy " << energy;
        EXPECT_EQ(report.half_angle, form.half_angle) << "energy " << energy;
        EXPECT_EQ(report.extent, form.extent) << "energy " << energy;
        EXPECT_EQ(report.rotation, form.rotation) << "energy " << energy;
        EXPECT_EQ(report.sagitta, form.sagitta) << "energy " << energy;
        EXPECT_EQ(report.closed, form.closed) << "energy " << energy;

        // The derived extremes, each against the accessor the renderer's own gauges use. These
        // are the four fields a transcription error would hide in, because any of them is a
        // plausible number in isolation.
        EXPECT_EQ(report.midpoint_y, midpoint_y(form)) << "energy " << energy;
        EXPECT_EQ(report.top_y, drawn_top_y(form)) << "energy " << energy;
        EXPECT_EQ(report.bottom_y, drawn_bottom_y(form)) << "energy " << energy;
        EXPECT_EQ(report.half_extent, half_extent(form)) << "energy " << energy;
    }
}

TEST(OperationalFormReportTests, TheFourExtremesAreDistinguishedFromOneAnother) {
    // The test above would pass if two of the derived fields were transcribed from the same
    // accessor AND those two accessors happened to agree. At the accepted ring they do not:
    // the midpoint is the circle's bottom, the top is its apex, and the reach is the radius.
    // Asserting they differ is what makes the field-by-field comparison load-bearing.
    const FormGeometry ring = form_geometry(kEnergyMax);
    const prd::ui::OperationalFormReport report = form_report(kEnergyMax, ring, accent(), true);

    EXPECT_NE(report.midpoint_y, report.top_y);
    EXPECT_NE(report.midpoint_y, report.half_extent);
    EXPECT_NE(report.top_y, report.bottom_y);
    EXPECT_NE(report.radius, report.centre_y);
    EXPECT_NE(report.half_angle, report.extent);
}

TEST(OperationalFormReportTests, TheAccentIsReportedAsHandedOverAndNotRederived) {
    // THE POINT OF REPORTING IT AT ALL. The renderer retains the colour it gave LVGL and this
    // states that value; it does not call accent() again. A report that re-derived the
    // colour would agree with the ramp instead of with the panel — which is precisely the
    // failure ui::OperationalFormReport exists to prevent, and the one a photograph cannot
    // catch, because white balance and the panel's off-axis response move a colour anyway.
    //
    // Handed a value the ramp cannot produce, so a re-derivation would be visible.
    const FormGeometry ring = form_geometry(kEnergyMax);
    const prd::ui::OperationalFormReport report = form_report(kEnergyMax, ring, kAnyAccent, true);

    EXPECT_EQ(report.accent, kAnyAccent);
    EXPECT_NE(report.accent, accent());
}

TEST(OperationalFormReportTests, TheAccentIsTheOneTokenAtEveryEnergy) {
    // What the renderer actually passes. The whole axis rather than the six magnitudes, for the
    // reason the geometry suite gives: those are a working baseline Phase H will move, and §11's
    // motion traverses everything between them.
    //
    // ⬅️ AND THE ANSWER IS THE SAME AT ALL OF THEM — §5.1 as ruled on 2026-09-01. Sweeping the
    // axis is still the right shape of test: it is what would catch an attenuation reintroduced
    // downstream of accent(), in the report path rather than in the accent itself.
    for (const Energy energy : all_energies()) {
        const prd::ui::OperationalFormReport report =
            form_report(energy, form_geometry(energy), accent(), false);

        EXPECT_EQ(report.accent, accent()) << "energy " << energy;
    }
}

TEST(OperationalFormReportTests, TheReportedAccentIsTheAcceptedTokenAtMaximumEnergy) {
    // The control, restated where a reviewer reading a walk line will look for it: the accent is
    // `color.accent` itself, so the closed ring is drawn exactly as it always was and anything
    // seen to change there is a defect rather than a design consequence.
    const prd::ui::OperationalFormReport ring =
        form_report(kEnergyMax, form_geometry(kEnergyMax), accent(), true);

    EXPECT_EQ(ring.accent, prd::ui::theme::kColorAccent);
}

TEST(OperationalFormReportTests, TheAccentIsCarriedSeparatelyFromTheShape) {
    // §5.1 puts one accent on the form and §3.1 makes the shape energy; only the shape follows the
    // axis,
    // and neither is derived from the other. Two forms at one energy drawn in different colours
    // must report different colours and identical geometry — which is what keeps a future
    // treatment change from silently moving the shape.
    const FormGeometry ring = form_geometry(kEnergyMax);

    const prd::ui::OperationalFormReport lit = form_report(kEnergyMax, ring, accent(), true);
    const prd::ui::OperationalFormReport other = form_report(kEnergyMax, ring, kAnyAccent, true);

    EXPECT_NE(lit.accent, other.accent);

    EXPECT_EQ(lit.radius, other.radius);
    EXPECT_EQ(lit.centre_y, other.centre_y);
    EXPECT_EQ(lit.extent, other.extent);
    EXPECT_EQ(lit.rotation, other.rotation);
    EXPECT_EQ(lit.closed, other.closed);
}

TEST(OperationalFormReportTests, TheRecordIsCarriedSeparatelyFromTheShape) {
    // §3.2: "The record shall never determine the form's shape." The report keeps them apart in
    // the same way — draws_record is what the renderer was told, and it changes nothing else.
    const FormGeometry ring = form_geometry(kEnergyMax);

    const prd::ui::OperationalFormReport with = form_report(kEnergyMax, ring, accent(), true);
    const prd::ui::OperationalFormReport without = form_report(kEnergyMax, ring, accent(), false);

    EXPECT_TRUE(with.draws_record);
    EXPECT_FALSE(without.draws_record);

    EXPECT_EQ(with.energy, without.energy);
    EXPECT_EQ(with.radius, without.radius);
    EXPECT_EQ(with.centre_y, without.centre_y);
    EXPECT_EQ(with.extent, without.extent);
    EXPECT_EQ(with.rotation, without.rotation);
    EXPECT_EQ(with.closed, without.closed);
}

TEST(OperationalFormReportTests, TheCycleTreatmentAlphaIsReportedExactlyAsPainted) {
    // This value qualifies the stable FormTreatment while motion is cross-fading the form into
    // a cycle. It is diagnostic evidence, not a value to re-derive from energy: hand over a
    // deliberately non-endpoint byte and require the report to preserve it exactly.
    constexpr std::uint8_t kIntermediateTreatment = 173;
    const FormGeometry ring = form_geometry(kEnergyMax);
    const prd::ui::OperationalFormReport report =
        form_report(kEnergyMax, ring, accent(), true, kIntermediateTreatment);

    EXPECT_EQ(report.cycle_treatment_alpha, kIntermediateTreatment);

    // A form that was not drawn cannot retain stale transition evidence.
    EXPECT_EQ(form_report(std::nullopt, std::nullopt, accent(), true, kIntermediateTreatment)
                  .cycle_treatment_alpha,
              0);
}

TEST(OperationalFormReportTests, TheReportCarriesEveryWidthTheFormHas) {
    // ⛔ WITHOUT THESE FIELDS A PHYSICAL RUN CANNOT PROVE THE REPAIR. Every other field in the
    // report is identical whether the settled horizon was drawn on x 160 or x 128 — which is the
    // exact defect being corrected. The centre, the three widths and both endpoint pairs are
    // what let a serial capture answer the question the photograph raises.
    const FormGeometry form = form_geometry(kEnergyMin);
    const prd::ui::OperationalFormReport crown = form_report(kEnergyMin, form, accent(), false);

    // Field-by-field transcription, each against the geometry it came from — never against a
    // number restated here, which is how a report starts describing a form nobody drew.
    EXPECT_EQ(crown.centre_x, form.centre_x);
    EXPECT_EQ(crown.half_width, form.half_width);
    EXPECT_EQ(crown.half_extent, half_extent(form));
    EXPECT_EQ(crown.paint_left,
              form.centre_x + paint_left_offset(form, prd::ui::theme::kRingStroke));
    EXPECT_EQ(crown.paint_right,
              form.centre_x + paint_right_offset(form, prd::ui::theme::kRingStroke));

    // The endpoints are the centre plus each reach, and the report carries them rather than
    // leaving the reading site to recompute a pair it might get wrong.
    EXPECT_EQ(crown.nominal_left, form.centre_x - half_extent(form));
    EXPECT_EQ(crown.nominal_right, form.centre_x + half_extent(form));

    // And the accepted values, so a reviewer reading a capture knows what a correct line says.
    EXPECT_EQ(crown.centre_x, 160);
    EXPECT_EQ(crown.half_width, 148);
    EXPECT_EQ(crown.half_extent, 144);

    EXPECT_EQ(crown.nominal_left, 16);
    EXPECT_EQ(crown.nominal_right, 304);
    EXPECT_EQ(crown.paint_left, 13);
    EXPECT_EQ(crown.paint_right, 306);

    // ⬅️ THE PAIRS ARE NOT INTERCHANGEABLE, so a swap has to fail rather than look plausible.
    EXPECT_NE(crown.paint_left, crown.nominal_left);
    EXPECT_NE(crown.paint_right, crown.nominal_right);
    EXPECT_LT(crown.paint_left, crown.nominal_left) << "ink reaches further than the path";
    EXPECT_GT(crown.paint_right, crown.nominal_right);

    // ⬅️ AND THE TWO SIDES ARE NOT MIRRORS, which is why no half-extent is reported.
    EXPECT_NE(crown.paint_right - crown.centre_x, crown.centre_x - crown.paint_left);

    // A form that is not on the panel reports nothing rather than stale zeroes read as geometry.
    const prd::ui::OperationalFormReport absent = form_report({}, {}, 0, false);
    EXPECT_FALSE(absent.drawn);
    EXPECT_EQ(absent.centre_x, 0);
    EXPECT_EQ(absent.paint_left, 0);
    EXPECT_EQ(absent.paint_right, 0);
}

TEST(OperationalFormTests, EveryPreservedStateMatchesTheExactPreEditBaseline) {
    // ⛔ THE FULL-FIELD PRESERVATION TABLE, captured from HEAD before this checkpoint touched the
    // form. Pinning only centre_x, half_width and half_extent was not enough: the angle law
    // itself changed (nearest-degree rounding became flooring, and the radius gained a cap), so
    // any of these fields could have moved without those three noticing.
    //
    // Ready is deliberately absent — it is the one endpoint this checkpoint moves.
    struct Baseline {
        const char* name;
        Energy energy;
        std::int32_t radius;
        std::int32_t centre_x;
        std::int32_t centre_y;
        std::int32_t half_width;
        std::int32_t half_angle;
        std::int32_t sagitta;
        std::int32_t extent;
        std::int32_t rotation;
        bool closed;
        std::int32_t nominal_half_extent;
        std::int32_t midpoint_y;
        std::int32_t end_y;
    };

    constexpr Baseline kBaseline[] = {
        {"Stopped", kEnergyStopped, 197, 128, 3, 96, 29, 25, 58, 61, false, 96, 200, 175},
        {"Preparing", kEnergyPreparing, 102, 128, 102, 96, 70, 67, 140, 20, false, 96, 204, 137},
        {"Completed", kEnergyCompleted, 96, 128, 112, 96, 97, 108, 194, 353, false, 96, 208, 100},
        {"Paused", kEnergyPaused, 96, 128, 116, 96, 124, 150, 248, 326, false, 96, 212, 62},
        {"Printing", kEnergyPrinting, 96, 128, 120, 96, 180, 192, 360, 270, true, 96, 216, 24},
    };

    for (const Baseline& want : kBaseline) {
        const FormGeometry form = form_geometry(want.energy);

        EXPECT_EQ(form.radius, want.radius) << want.name;
        EXPECT_EQ(form.centre_x, want.centre_x) << want.name;
        EXPECT_EQ(form.centre_y, want.centre_y) << want.name;
        EXPECT_EQ(form.half_width, want.half_width) << want.name;
        EXPECT_EQ(form.half_angle, want.half_angle) << want.name;
        EXPECT_EQ(form.sagitta, want.sagitta) << want.name;
        EXPECT_EQ(form.extent, want.extent) << want.name;
        EXPECT_EQ(form.rotation, want.rotation) << want.name;
        EXPECT_EQ(form.closed, want.closed) << want.name;
        EXPECT_EQ(half_extent(form), want.nominal_half_extent) << want.name;
        EXPECT_EQ(midpoint_y(form), want.midpoint_y) << want.name;
        EXPECT_EQ(end_y(form), want.end_y) << want.name;
    }
}

TEST(OperationalFormTests, ThePrintingEndpointIsClosedAndUnmoved) {
    // Kept separate and explicit: the closed cycle is the one shape that can host the record, and
    // §3.2 makes that a property of the shape rather than of the state. A Printing form that
    // opened by a degree would still pass every width and centre check above.
    const FormGeometry ring = form_geometry(kEnergyPrinting);

    EXPECT_TRUE(ring.closed);
    EXPECT_EQ(ring.extent, 360);
    EXPECT_EQ(ring.rotation, 270);
    EXPECT_EQ(ring.radius, prd::ui::theme::kRingDiameter / 2);
    EXPECT_EQ(ring.centre_x, prd::ui::theme::kRingCentreX);
    EXPECT_EQ(ring.centre_y, prd::ui::theme::kRingCentreY);
    EXPECT_EQ(midpoint_y(ring), prd::ui::theme::kRingCentreY + (prd::ui::theme::kRingDiameter / 2));
}

TEST(OperationalFormReportTests, BothAcceptedEndpointsAreReportedExactly) {
    // Printing and Ready are the controls on the panel: their geometry is anchored to
    // layout.hpp's own constants by name, and any change seen on those two states is a defect
    // rather than a design consequence. A reviewer reading the walk's line for either of them
    // is checking that claim, so the report must carry the accepted figures without rounding,
    // clamping or re-deriving anything.
    const prd::ui::OperationalFormReport ring =
        form_report(kEnergyMax, form_geometry(kEnergyMax), accent(), true);

    EXPECT_TRUE(ring.drawn);
    EXPECT_EQ(ring.radius, kRingRadius);
    EXPECT_EQ(ring.centre_y, prd::ui::theme::kRingCentreY);
    EXPECT_TRUE(ring.closed);
    EXPECT_TRUE(ring.draws_record);

    const prd::ui::OperationalFormReport crown =
        form_report(kEnergyMin, form_geometry(kEnergyMin), accent(), false);

    EXPECT_TRUE(crown.drawn);
    EXPECT_EQ(crown.radius, prd::ui::theme::kHorizonRadius);
    EXPECT_EQ(crown.centre_y, prd::ui::theme::kHorizonCentreY);
    EXPECT_EQ(crown.midpoint_y, prd::ui::theme::kHorizonApexY);
    EXPECT_FALSE(crown.closed);
    EXPECT_FALSE(crown.draws_record);
}

// --- The form's paint rule (§5.1, owner ruling 2026-08-14) ------------------
//
// One rule for three drawn cases. These tests exist because the alternative — a selection made
// inside the LVGL component — is invisible to every gate this project runs, and because the
// case that was wrong for three phases was wrong precisely by falling through to another case's
// treatment.

TEST(FormTreatmentTests, AnUndrawnFormHasNoTreatment) {
    EXPECT_EQ(form_treatment(false, false, false), FormTreatment::None);
    EXPECT_EQ(form_treatment(false, true, true), FormTreatment::None);
}

TEST(FormTreatmentTests, EveryOpenShapeCarriesTheAccentOnTheFormItself) {
    // Five of the six states. Only a cycle can host a record (§3.2), so an open shape never
    // has one and the form is the drawn element.
    EXPECT_EQ(form_treatment(true, false, false), FormTreatment::FormAccent);
}

TEST(FormTreatmentTests, AClosedShapeHostingARecordDrawsTheAccentAsTheSweep) {
    EXPECT_EQ(form_treatment(true, true, true), FormTreatment::RecordSweep);
}

TEST(FormTreatmentTests, AClosedShapeWithNoRecordDrawsTheTrackAlone) {
    // THE CORRECTION. This case previously fell through to FormAccent and painted a closed
    // accent cycle — indistinguishable from a completed record on the panel.
    EXPECT_EQ(form_treatment(true, true, false), FormTreatment::TrackOnly);
    EXPECT_NE(form_treatment(true, true, false), FormTreatment::FormAccent);
}

TEST(FormTreatmentTests, TheAccentIsNeverDrawnAsAClosedCycleUnlessItIsARecord) {
    // The rule stated as the property it exists to guarantee, over the whole axis rather than
    // at the two endpoints. A closed shape is reachable only at maximum energy today, but the
    // property must hold wherever closure occurs.
    for (std::int32_t energy = kEnergyMin; energy <= kEnergyMax; ++energy) {
        const FormGeometry form = form_geometry(energy);
        if (!form.closed) {
            continue;
        }

        EXPECT_NE(form_treatment(true, form.closed, false), FormTreatment::FormAccent)
            << "a closed form with no record would be painted as a full accent cycle at energy "
            << energy;
    }
}

TEST(FormTreatmentTests, TheReportNamesTheTreatmentItWasDrawnWith) {
    // The read-back is how three frames are checked against one rule, so the report and the
    // rule must not be able to disagree.
    const FormGeometry ring = form_geometry(kEnergyMax);

    const prd::ui::OperationalFormReport with_record =
        form_report(kEnergyMax, ring, accent(), true);
    const prd::ui::OperationalFormReport without_record =
        form_report(kEnergyMax, ring, accent(), false);
    const prd::ui::OperationalFormReport crown =
        form_report(kEnergyMin, form_geometry(kEnergyMin), accent(), false);

    EXPECT_EQ(with_record.treatment, static_cast<std::uint8_t>(FormTreatment::RecordSweep));
    EXPECT_EQ(without_record.treatment, static_cast<std::uint8_t>(FormTreatment::TrackOnly));
    EXPECT_EQ(crown.treatment, static_cast<std::uint8_t>(FormTreatment::FormAccent));

    // An undrawn form reports None, which mirrors `drawn == false`.
    const prd::ui::OperationalFormReport nothing =
        form_report(std::nullopt, std::nullopt, 0, false);
    EXPECT_FALSE(nothing.drawn);
    EXPECT_EQ(nothing.treatment, static_cast<std::uint8_t>(FormTreatment::None));
}
