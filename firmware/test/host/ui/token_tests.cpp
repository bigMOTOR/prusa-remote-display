// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

#include "theme/layout.hpp"
#include "theme/tokens.hpp"

// The token and layout suite — Production UI Specification §3 to §7.
//
// Most of what protects these values is a static_assert inside tokens.hpp and
// layout.hpp, which is the right place: a transcription error should not reach a build
// at all, let alone a test run. What is here is the part a static_assert cannot express
// — properties over the whole set, and the §4 role table's completeness.
//
// These tests deliberately do NOT restate the numbers. A test asserting
// kSpace4 == 16 is a second transcription of the same document, and it would agree with
// a mistake as readily as with the value. The one exception is the D8 geometry, which is
// asserted numerically in layout.hpp precisely because it is an owner-approved decision
// rather than a value read from a specification.

using namespace prd::ui::theme;

// --- §3 The spacing scale is closed ----------------------------------------

TEST(SpacingScaleTests, EveryValueIsAWholeNumberOfBaseUnits) {
    // §3: "No additional spacing values are permitted", on a base of 4. A value that
    // was not a multiple would be a new step smuggled into the scale.
    const std::int32_t scale[] = {kSpace0, kSpace1, kSpace2, kSpace3,
                                  kSpace4, kSpace5, kSpace6, kSpace8};

    for (const auto value : scale) {
        EXPECT_EQ(value % kUnitBase, 0) << "value " << value << " is not a multiple of the base";
    }
}

TEST(SpacingScaleTests, TheScaleHasNoDuplicates) {
    // Two tokens with the same value are two names for one step, which makes "which
    // token is this?" unanswerable at a use site.
    const std::set<std::int32_t> distinct{kSpace0, kSpace1, kSpace2, kSpace3,
                                          kSpace4, kSpace5, kSpace6, kSpace8};
    EXPECT_EQ(distinct.size(), 8U);
}

TEST(SpacingScaleTests, EveryStructuralTokenResolvesToAScaleValue) {
    // §3's structural tokens are defined in terms of the scale. This asserts they did
    // not drift into values of their own.
    const std::set<std::int32_t> scale{kSpace0, kSpace1, kSpace2, kSpace3,
                                       kSpace4, kSpace5, kSpace6, kSpace8};

    const std::int32_t structural[] = {kSpaceCompact,   kSpaceComponent,  kSpaceRegion,
                                       kPaddingCompact, kPaddingStandard, kPaddingRegion,
                                       kModuleMin,      kModuleStandard};

    for (const auto value : structural) {
        EXPECT_EQ(scale.count(value), 1U) << "structural token " << value << " is off the scale";
    }
}

// --- §5 The palette is distinct --------------------------------------------

TEST(ColourTokenTests, EverySemanticColourIsDistinct) {
    // Except `color.disabled`, which §5 defines as derived from `color.tertiary` and is
    // therefore the same hue by construction.
    const std::set<std::uint32_t> distinct{kColorBackground, kColorSurface,  kColorPrimary,
                                           kColorSecondary,  kColorTertiary, kColorAccent,
                                           kColorSuccess,    kColorWarning,  kColorError};
    EXPECT_EQ(distinct.size(), 9U);
}

// --- T20: the warning token, decided on real glass -------------------------
//
// ⛔ WHY THIS IS TESTED AS A VALUE AND AS A DISTANCE. Milestone 3.11 T20 found that the accepted
// `#F2B84B` rendered as lime on this panel — not through any pipeline fault (the framebuffer
// provably carried the correct RGB565) but because the token was wrong FOR THIS GLASS. The owner
// chose `#ED9A2E` on the board from neutrally-labelled candidates.
//
// The value alone is not the property worth keeping. What the decision actually rests on is that
// the warning stays far enough from `color.accent` to read as a separate semantic rather than as a
// poor reproduction of the accent — that is precisely why the warmer candidate beside it was
// rejected. So the separation is asserted in the same metric the decision used.

namespace {

struct Lab {
    double l;
    double a;
    double b;
};

[[nodiscard]] Lab to_lab(const std::uint32_t rgb) {
    const auto channel = [](const std::uint32_t v) {
        const double c = static_cast<double>(v) / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };

    const double r = channel((rgb >> 16) & 0xFFU);
    const double g = channel((rgb >> 8) & 0xFFU);
    const double b = channel(rgb & 0xFFU);

    const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;

    const auto f = [](const double t) {
        return t > 0.008856 ? std::cbrt(t) : (7.787 * t) + (16.0 / 116.0);
    };

    return {(116.0 * f(y)) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))};
}

// CIE76. Enough to hold a palette apart, and the metric the T20 decision was recorded in.
[[nodiscard]] double delta_e(const std::uint32_t lhs, const std::uint32_t rhs) {
    const Lab a = to_lab(lhs);
    const Lab b = to_lab(rhs);
    const double dl = a.l - b.l;
    const double da = a.a - b.a;
    const double db = a.b - b.b;
    return std::sqrt((dl * dl) + (da * da) + (db * db));
}

} // namespace

TEST(ColourTokenTests, WarningIsTheValueDecidedOnGlassAtT20) {
    // A change to this line is a change to a product decision made on the board, not a tweak.
    EXPECT_EQ(kColorWarning, 0xED9A2EU);

    // And explicitly NOT the specification's original value, which read as lime on this panel.
    // Recorded as a divergence pending a Production UI revision.
    EXPECT_NE(kColorWarning, 0xF2B84BU);
}

TEST(ColourTokenTests, WarningStaysASeparateSemanticFromAccent) {
    // ⛔ THE PROPERTY THAT RULED OUT THE WARMER CANDIDATE. Too close to accent and a warning stops
    // reading as its own meaning and starts reading as a badly reproduced accent.
    EXPECT_GT(delta_e(kColorWarning, kColorAccent), 15.0);
}

TEST(ColourTokenTests, WarningIsUnmistakableAgainstErrorAndSuccess) {
    // Production UI §8's Attention Indicator uses warning OR error, so those two must never be
    // confusable; success shares the status vocabulary and must not be either.
    EXPECT_GT(delta_e(kColorWarning, kColorError), 40.0);
    EXPECT_GT(delta_e(kColorWarning, kColorSuccess), 40.0);
}

TEST(ColourTokenTests, WarningRemainsLegibleOnTheProductionBackground) {
    // The decision recorded 8.45:1. WCAG AA normal text is 4.5:1, so this keeps real margin, and a
    // future warmer revision that traded away contrast would fail here rather than on a panel.
    const auto relative = [](const std::uint32_t rgb) {
        const auto channel = [](const std::uint32_t v) {
            const double c = static_cast<double>(v) / 255.0;
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        };
        return (0.2126 * channel((rgb >> 16) & 0xFFU)) + (0.7152 * channel((rgb >> 8) & 0xFFU)) +
               (0.0722 * channel(rgb & 0xFFU));
    };

    const double warning = relative(kColorWarning);
    const double background = relative(kColorBackground);
    const double ratio =
        (std::max(warning, background) + 0.05) / (std::min(warning, background) + 0.05);

    EXPECT_GT(ratio, 4.5);
}

TEST(ColourTokenTests, DisabledIsDerivedFromTertiary) {
    EXPECT_EQ(kColorDisabled, kColorTertiary);
}

TEST(ColourTokenTests, EveryColourFitsTwentyFourBits) {
    const std::uint32_t palette[] = {kColorBackground, kColorSurface,  kColorPrimary,
                                     kColorSecondary,  kColorTertiary, kColorAccent,
                                     kColorSuccess,    kColorWarning,  kColorError};

    for (const auto colour : palette) {
        EXPECT_EQ(colour & 0xFF000000U, 0U) << "colour " << colour << " has bits above RGB";
    }
}

// --- §4 The text role table is complete ------------------------------------

TEST(TextRoleTests, EveryRoleHasASizeALineHeightAndAWeight) {
    // The switch statements in tokens.cpp have a fallback return, so an unhandled role
    // compiles. This is what catches one: a role that fell through would report the
    // Body values, and Body's own row is the only one allowed to.
    const TextRole roles[] = {TextRole::StateTitle, TextRole::PrimaryValue, TextRole::PrimaryUnit,
                              TextRole::Section,    TextRole::Body,         TextRole::Label,
                              TextRole::Metadata,   TextRole::OutcomeTitle};

    static_assert(sizeof(roles) / sizeof(roles[0]) == kTextRoleCount,
                  "the role list here must cover the enumeration");

    for (const auto role : roles) {
        EXPECT_GT(font_size(role), 0);
        EXPECT_GE(font_weight(role), kFontWeightRegular);
        EXPECT_LE(font_weight(role), kFontWeightBold);

        // Every line box must hold its face's cap height, which is what decides whether a
        // row can contain its glyphs. Inter's cap height is 0.7275 em; 72/100 is the
        // integer floor used in tokens.hpp.
        EXPECT_GE(line_height(role), font_size(role) * 72 / 100)
            << "line box cannot hold the cap height";

        // And for the prose roles it must also exceed the em, because their faces carry
        // descenders. PrimaryValue is deliberately exempt: its face is digits only, so its
        // line box is its cap height and a larger one would reserve unusable space. The
        // exemption is stated here rather than implied by a weaker blanket rule.
        if (role != TextRole::PrimaryValue) {
            EXPECT_GT(line_height(role), font_size(role))
                << "a prose line box must contain its descenders";
        }
    }
}

TEST(TextRoleTests, TheRolesUseFiveDistinctSizes) {
    // §4 pairs six roles across six sizes, but Label and Metadata differ only by size
    // and share a weight class with others. What must hold is that no two roles are
    // identical in both size and weight, which would make one of them redundant.
    std::set<std::pair<std::int32_t, std::int32_t>> pairs;

    const TextRole roles[] = {TextRole::StateTitle, TextRole::PrimaryValue, TextRole::PrimaryUnit,
                              TextRole::Section,    TextRole::Body,         TextRole::Label,
                              TextRole::Metadata,   TextRole::OutcomeTitle};

    for (const auto role : roles) {
        pairs.insert({font_size(role), font_weight(role)});
    }

    EXPECT_EQ(pairs.size(), kTextRoleCount) << "two roles resolve to the same size and weight";
}

TEST(TextRoleTests, ThePrimaryValueIsTheLargestAndHeaviest) {
    // §4's hierarchy, and §12's "operational state shall always be identifiable before
    // detailed telemetry" — which depends on the primary value dominating.
    const TextRole others[] = {TextRole::StateTitle, TextRole::PrimaryUnit, TextRole::Section,
                               TextRole::Body,       TextRole::Label,       TextRole::Metadata};

    for (const auto role : others) {
        EXPECT_GT(font_size(TextRole::PrimaryValue), font_size(role));
        EXPECT_GE(font_weight(TextRole::PrimaryValue), font_weight(role));
    }
}

// --- §5 Luminance hierarchy ------------------------------------------------

TEST(LuminanceTests, TheTextHierarchyDescendsAndTheSurfaceIsRaised) {
    // §5: "Hierarchy shall primarily be established through luminance." The property to
    // protect is that the hierarchy never inverts, which a colour edit could do while
    // leaving every value a plausible grey.
    EXPECT_GT(relative_luminance(kColorPrimary), relative_luminance(kColorSecondary));
    EXPECT_GT(relative_luminance(kColorSecondary), relative_luminance(kColorTertiary));
    EXPECT_GT(relative_luminance(kColorTertiary), relative_luminance(kColorSurface));
    EXPECT_GT(relative_luminance(kColorSurface), relative_luminance(kColorBackground));
}

TEST(LuminanceTests, TheAccentIsBrighterThanBothNeutralGroundTokens) {
    // UIDD 004 §5.1: `color.accent` is applied to the operational form alone, and is one colour
    // at every energy.
    //
    // WHAT THE PRODUCTION FORM ACTUALLY SITS ON: `color.background`, and nothing else. The
    // second assertion is therefore the STRONGER of the two by construction, not a claim about
    // placement — it holds the accent above `color.surface`, the LIGHTER of §5's two neutral
    // ground tokens, as a luminance reference. Clearing the lighter ground implies clearing the
    // darker one, so the pair states the invariant with margin rather than at the boundary.
    //
    // ⛔ IT IS NOT EVIDENCE THAT THE FORM SITS ON, OR PASSES BEHIND, A SURFACE PANEL. It does
    // not. `Colour::Surface` has exactly one consumer in production — theme.cpp's token-to-
    // lv_color_t switch arm — and no scene or component in the operational family ever requests
    // it, so no Surface panel is painted at all. An earlier revision of this comment asserted
    // such a placement on 2026-09-01; that claim was false and is withdrawn.
    //
    // ⛔ NOR IS IT BECAUSE THE ACCENT CARRIES TEXT. Before 2026-09-01 this comment justified the
    // same two assertions by saying the accent carried the state title. It does not: §5.1's
    // referent has been the operational form alone since Phase F, and "all Operational Family
    // text is achromatic, except under 5.2" — scenes/operational_content.hpp records that
    // change. The §12 typography clause once quoted here was the wrong citation for a test about
    // the form.
    //
    // The §5 ladder itself — tertiary > surface > background — is fixed by static_assert in
    // theme/tokens.hpp; this test adds the accent's relation to the two grounds.
    EXPECT_GT(relative_luminance(kColorAccent), relative_luminance(kColorSurface));
    EXPECT_GT(relative_luminance(kColorAccent), relative_luminance(kColorBackground));
}

// --- §7 Region geometry (owner decision D8) --------------------------------

TEST(LayoutTests, NoContentTouchesTheDisplayEdge) {
    // §7 Canvas: "All content shall remain inside canvas.inset. No component may touch
    // the display edge."
    EXPECT_TRUE(contains(kCanvas, kCanvasContent));
    EXPECT_GT(kCanvasContent.x, 0);
    EXPECT_GT(kCanvasContent.y, 0);
    EXPECT_LT(kCanvasContent.right(), kDisplayWidth);
    EXPECT_LT(kCanvasContent.bottom(), kDisplayHeight);

    EXPECT_TRUE(contains(kCanvasContent, kRegionTelemetry));
    EXPECT_TRUE(contains(kCanvasContent, kRegionPrimary));
    EXPECT_TRUE(contains(kCanvasContent, kRegionOperational));
}

TEST(LayoutTests, TheTelemetryRowsTileTheRegionExactly) {
    // A remainder would leave an unowned strip whose ownership nothing decides, and §8
    // requires the rows to "maintain stable vertical positions".
    EXPECT_EQ(kTelemetryRowHeight * kTelemetryRows, kRegionTelemetry.height);
}

TEST(LayoutTests, TheRingOverlapsTheTelemetryRegionAsD8Accepted) {
    // Asserted, not merely commented. Measurement of the accepted UIDD 001 asset puts
    // the ring's right edge inside the telemetry column's band; D8 accepted that regions
    // define ownership rather than clipping. If a later geometry change removes the
    // overlap, the note in layout.hpp becomes untrue, and this is what says so.
    EXPECT_TRUE(overlaps(kRegionPrimary, kRegionTelemetry));
    EXPECT_EQ(kRegionPrimary.right() - kRegionTelemetry.x, 12);
}

TEST(LayoutTests, TheRingIsCentredOnTheDisplayVerticalAxis) {
    EXPECT_EQ(kRingCentreY, kDisplayHeight / 2);
    EXPECT_EQ(kRegionPrimary.y, kDisplayHeight / 2 - kRingDiameter / 2);
    EXPECT_EQ(kDisplayHeight - kRegionPrimary.bottom(), kRegionPrimary.y)
        << "the ring's top and bottom margins must be equal";
}

TEST(LayoutTests, TheTextStackClearsTheArcStroke) {
    // The operational region is the square inscribed in the ring's inner circle, so text
    // cannot collide with the arc at any angle.
    EXPECT_TRUE(contains(kRegionPrimary, kRegionOperational));
    EXPECT_GT(kRegionOperational.x - kRegionPrimary.x, kRingStroke);
    EXPECT_GT(kRegionPrimary.right() - kRegionOperational.right(), kRingStroke);
}

TEST(LayoutTests, TheOperationalRegionFitsTheThreeLinesUidd001Places) {
    // Progress percentage, state title, remaining time — at their §4 line heights.
    EXPECT_LE(kLineDisplay + kLineTitle + kLineBody, kRegionOperational.height);
}

TEST(LayoutTests, TheOperationalRegionIsHorizontallyCentredOnTheRing) {
    const std::int32_t left = kRegionOperational.x - kRegionPrimary.x;
    const std::int32_t right = kRegionPrimary.right() - kRegionOperational.right();
    EXPECT_EQ(left, right);
}

// --- Rect algebra ----------------------------------------------------------
//
// The predicates the assertions above rest on. Tested directly, because an `overlaps`
// that returned false unconditionally would make every geometry test pass.

TEST(RectTests, OverlapsDetectsAnActualOverlap) {
    EXPECT_TRUE(overlaps(Rect{0, 0, 10, 10}, Rect{5, 5, 10, 10}));
}

TEST(RectTests, TouchingEdgesDoNotOverlap) {
    // Half-open rectangles: a region ending at x=10 and one starting at x=10 share no
    // pixel. If this were inclusive, every adjacent region pair would report an overlap
    // and the D8 assertion above would be meaningless.
    EXPECT_FALSE(overlaps(Rect{0, 0, 10, 10}, Rect{10, 0, 10, 10}));
    EXPECT_FALSE(overlaps(Rect{0, 0, 10, 10}, Rect{0, 10, 10, 10}));
}

TEST(RectTests, ContainsIsInclusiveOfTheBoundary) {
    EXPECT_TRUE(contains(Rect{0, 0, 10, 10}, Rect{0, 0, 10, 10}));
    EXPECT_TRUE(contains(Rect{0, 0, 10, 10}, Rect{2, 2, 6, 6}));
    EXPECT_FALSE(contains(Rect{0, 0, 10, 10}, Rect{2, 2, 9, 6}));
}
