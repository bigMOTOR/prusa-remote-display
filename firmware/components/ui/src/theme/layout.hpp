// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "theme/tokens.hpp"

// Layout geometry — Production UI Specification §7's persistent regions.
//
// §7 NAMES THE REGIONS AND SPECIFIES NO GEOMETRY. It lists five regions, requires
// them to "remain structurally stable throughout all operational states", requires
// content to stay inside `canvas.inset`, and requires the implementation to
// "preserve the optical balance established by the accepted artwork" — but gives no
// coordinate, no width and no region-to-component assignment. §14's checklist then
// forbids literal geometry outside tokens.
//
// The rectangles below are therefore DERIVED from the accepted artwork and the §3
// token scale. They are not a re-specification of §7 and they do not reopen it.
//
// ---------------------------------------------------------------------------
// WORKING BASELINE — NOT AN ACCEPTED DESIGN
// ---------------------------------------------------------------------------
//
// Owner position, 2026-08-06: this geometry is the current working baseline and is
// explicitly NOT accepted. It was derived against one screen — Printing — and the
// questions it settles (how much of the canvas the ring takes, how the telemetry column
// relates to it, the spacing and the hierarchy) are questions about the operational
// FAMILY, four of whose five other states do not even draw a ring.
//
// Judging any of it against Printing alone would bake in a compromise made in isolation.
// So nothing here is frozen: the whole operational family is built first, then reviewed
// as one system, and only then are these values and the token divergences they rest on
// settled — once, against the complete composition, rather than screen by screen.
//
// Read every figure below as "what the baseline currently is", not "what was decided".
//
// REGIONS ARE OWNERSHIP, NOT CLIPPING. Accepted with the geometry: bounding boxes
// may overlap. `region.primary` — the progress ring's box — extends 12 px into
// `region.telemetry`, because measurement of the UIDD 001 asset puts the ring at
// roughly 200 px across on a 240 px display with its right edge inside the telemetry
// column's horizontal band, and the row dividers visibly cross the unfilled arc
// track. A strict non-overlapping grid cannot reproduce that composition; shrinking
// the ring to avoid the overlap would abandon §7's optical-balance requirement to
// satisfy a constraint §7 never states. What must not overlap is drawn foreground
// content, which is a scene obligation and is verified by eye on the panel.
//
// Free of LVGL, like tokens.hpp, so the derivations below are host-testable.

namespace prd::ui::theme {

struct Rect {
    std::int32_t x{0};
    std::int32_t y{0};
    std::int32_t width{0};
    std::int32_t height{0};

    [[nodiscard]] constexpr std::int32_t right() const {
        return x + width;
    }

    [[nodiscard]] constexpr std::int32_t bottom() const {
        return y + height;
    }
};

[[nodiscard]] constexpr bool operator==(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

[[nodiscard]] constexpr bool operator!=(const Rect& lhs, const Rect& rhs) {
    return !(lhs == rhs);
}

[[nodiscard]] constexpr bool overlaps(const Rect& lhs, const Rect& rhs) {
    return lhs.x < rhs.right() && rhs.x < lhs.right() && lhs.y < rhs.bottom() &&
           rhs.y < lhs.bottom();
}

[[nodiscard]] constexpr bool contains(const Rect& outer, const Rect& inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
           inner.bottom() <= outer.bottom();
}

// --- Panel -----------------------------------------------------------------

// Production UI §1: 320×240, landscape, 2.8-inch IPS.
inline constexpr std::int32_t kDisplayWidth = 320;
inline constexpr std::int32_t kDisplayHeight = 240;

inline constexpr Rect kCanvas{0, 0, kDisplayWidth, kDisplayHeight};

// §7 Canvas: "All content shall remain inside `canvas.inset`. No component may
// touch the display edge."
inline constexpr Rect kCanvasContent{kCanvasInset, kCanvasInset, kDisplayWidth - (2 * kCanvasInset),
                                     kDisplayHeight - (2 * kCanvasInset)};

// --- Regions ---------------------------------------------------------------

// Telemetry Region — §8's Temperature Row and Telemetry Column. Three rows of
// equal height filling the content box, right-aligned to the content edge.
inline constexpr std::int32_t kTelemetryWidth = 96;
inline constexpr std::int32_t kTelemetryRows = 3;

// Owner-reviewed native-size preview, 2026-09-06. Columns stay fixed as digits change.
inline constexpr std::int32_t kTelemetryIconX = 238;
inline constexpr std::int32_t kTelemetryValueX = 260;
inline constexpr std::int32_t kTelemetryFirstY = 38;
inline constexpr std::int32_t kTelemetryIconOffsetY = 6;
inline constexpr std::int32_t kTelemetryLabelOffsetY = 26;
inline constexpr std::int32_t kTelemetryIconWidth = 16;
inline constexpr std::int32_t kTelemetryIconHeight = 18;

// Shared progress for geometry, placement, size and the pause marker. This is the
// accepted preview exception to the earlier constant-typography/inventory proposal.
inline constexpr std::uint32_t kOperationalMotionMs = 1100;
inline constexpr std::int32_t kMotionTitleY = 145;
inline constexpr std::int32_t kMotionTitleSize = 20;
inline constexpr std::int32_t kMotionValueY = 76;
inline constexpr std::int32_t kMotionThirdY = 172;
inline constexpr std::int32_t kMotionRestingTitleY = 119;
inline constexpr std::int32_t kMotionCompletedTitleY = 54;
inline constexpr std::int32_t kMotionCompletedValueY = 112;
inline constexpr std::int32_t kMotionCompletedValueSize = 44;
inline constexpr std::int32_t kMotionUnknownTitleY = 98;
inline constexpr std::int32_t kMotionUnknownTitleSize = 26;
inline constexpr std::int32_t kMotionUnknownThirdY = 137;
inline constexpr std::int32_t kPauseLeft = 117;
inline constexpr std::int32_t kPauseTop = 31;
inline constexpr std::int32_t kPauseBarWidth = 8;
inline constexpr std::int32_t kPauseBarHeight = 26;
inline constexpr std::int32_t kPauseBarSpacing = 14;

inline constexpr Rect kRegionTelemetry{kCanvasContent.right() - kTelemetryWidth, kCanvasContent.y,
                                       kTelemetryWidth, kCanvasContent.height};

inline constexpr std::int32_t kTelemetryRowHeight = kCanvasContent.height / kTelemetryRows;

// The hairline between rows (§6 `stroke.hairline`). Inset from the region's left
// edge so it begins clear of the icon column, as the artwork shows.
inline constexpr std::int32_t kTelemetryDividerInset = kSpace1;

// Primary Region — the progress ring's box, and the Ready Horizon's later.
//
// 192 px: the artwork measures at roughly 200, and 192 is the nearest value that
// leaves a symmetric `space.6` margin above and below when centred on the display's
// own vertical axis. Centring on y = 120 rather than on the content box is
// deliberate: the artwork's ring is centred on the panel, and §7's inset governs
// content, which the arc respects with 12 px to spare.
inline constexpr std::int32_t kRingDiameter = 192;
inline constexpr std::int32_t kRingCentreX = 128;
inline constexpr std::int32_t kRingCentreY = kDisplayHeight / 2;
// §6 specifies `stroke.emphasis` = 4 px for the Progress Ring. On the panel that reads
// noticeably flatter than the accepted UIDD 001 artwork, whose stroke measures roughly
// 10-12 px against a ~200 px ring — and the owner asked for the ring to carry more visual
// energy (2026-08-06). Doubled to `space.2`, which is a token rather than a new value.
//
// A divergence from a binding §6 token, recorded as such. It is also the change that most
// affects the ring's weight: opacity and colour are already at their strongest, since the
// indicator is `color.accent` at `opacity.full`.
inline constexpr std::int32_t kRingStroke = kSpace2;

static_assert(kRingStroke > kStrokeEmphasis,
              "the ring stroke is deliberately heavier than §6's stroke.emphasis; see above");

inline constexpr Rect kRegionPrimary{kRingCentreX - (kRingDiameter / 2),
                                     kRingCentreY - (kRingDiameter / 2), kRingDiameter,
                                     kRingDiameter};

// Operational Region — the text stack inside the ring: primary value, state title,
// remaining duration (UIDD 001: "Progress percentage in the center. Status under
// the percentage. Remaining time below status.").
//
// WIDENED FROM 136 x 88 on owner instruction, 2026-08-06.
//
// The earlier figures were derived for a primary value at §4's `font.size.display` = 36 px. On
// the physical panel that numeral measured 26 px of cap height — under half the accepted
// UIDD 001 artwork's weight — and the owner directed matching the artwork instead. The
// numeral is now 72 px (`font.size.hero`), and the region has to hold it.
//
// SIZED AGAINST THE RING'S CHORD, NOT ITS INSCRIBED SQUARE. The square was the earlier,
// over-conservative bound: text is a horizontal band, so what constrains it is the chord
// at the band's own height, which is widest at the ring's centre. The numeral row is
// therefore centred on the ring centre, where the chord is 184 px, and the two smaller
// lines hang below it — which is also how the artwork is composed.
//
// Measured clearance at the widest row: the three-digit field at 100 % plus its unit is
// 160 px against a 177 px chord at that height. The figures come from the generated face's
// own metrics read on the device — Inter's tabular advance is 0.653 em, not the 0.594 the
// font tables implied, and 72 px worked out at 175 px against a 175 px chord.
inline constexpr std::int32_t kOperationalWidth = 172;
inline constexpr std::int32_t kOperationalHeight = kLineHero + kLineSection + kLineBody;

// LIFTED ABOVE THE GEOMETRIC CENTRE, as one block.
//
// With the numeral's line box centred exactly on the ring, the stack as a whole sits low:
// the numeral is centred but the title and duration hang below it, so the block's centre of
// mass lands ~19 px under the ring's. That reads as bottom-heavy on the panel even though
// every element is where the arithmetic put it — optical centre and geometric centre are not
// the same thing for a stack whose weight is at the top.
//
// One spacing step, applied to the region rather than to any element, so progress, title and
// duration move together and their relationship to each other is untouched.
inline constexpr std::int32_t kOperationalLift = kSpace2;

inline constexpr Rect kRegionOperational{kRingCentreX - (kOperationalWidth / 2),
                                         kRingCentreY - (kLineHero / 2) - kOperationalLift,
                                         kOperationalWidth, kOperationalHeight};

// --- The Ready Horizon, as a transformation of the ring ---------------------
//
// §6: "The Ready horizon is the terminal geometric transformation of the Progress Ring and
// shall not exist as an independent primitive." UI Design Decision 002 adds what it means:
// "the final remaining visual energy of the Printing composition", communicating "quiet
// presence rather than activity".
//
// So it is the SAME circle, grown and sunk until only a shallow crown of it remains on the
// canvas — one lv_arc object with a different radius, centre and angular extent. Nothing new
// is constructed, which is precisely what §6 forbids.
//
// ⛔ THE CROWN IS THE WIDTH OF THE CANVAS, NOT THE WIDTH OF THE RING — owner ruling 2026-08-29,
// against the accepted physical reference at docs/project-sources/assets/Ready Resting F1.jpeg.
//
// WHAT THIS REPLACES, AND WHY IT WAS WRONG. The first derivation read: "the crown spans exactly
// the ring's own horizontal extent (x 32 to 224), so the two forms occupy the same width and the
// transformation reads as one object settling rather than as a swap." That is a coherent
// argument and it is not what the accepted expression looks like. Settled Ready draws NOTHING
// but this arc — no name, no telemetry, no state word — so there is no telemetry column for it
// to share the canvas with and no reason to inherit a width that was chosen to sit beside one.
// The reference photograph shows the horizon running nearly edge to edge.
//
// MEASURED, NOT ESTIMATED. The lit aperture was located by fitting its left and right edges
// across 271 rows, then the arc's lit pixels were mapped back into panel space. Rectified
// measurement, in panel pixels:
//
//     centre x 156.9   drawn half-extent 149.0   apex y 197.8   sag 16.9   stroke 7.7
//
// ⚠️ THE HORIZONTAL FIGURES ARE THE TRUSTWORTHY ONES, AND THE ASPECT SAYS WHY. The fitted
// aperture is 417 px wide by 306 px tall — an in-image aspect of 1.363 against the panel's true
// 1.333 — so the row fit under-measures the height by about 2 percent, and only the VERTICAL
// scale carries that error. Nothing is concluded from the vertical measurement here: apex, sag
// and stroke are left at the values already accepted, and the 197.8 / 16.9 / 7.7 above are
// reported as corroboration of them rather than as a source for them. The width and centre,
// which the fit measures directly and which this checkpoint actually changes, do not depend on
// the vertical scale at all.
//
// Each measured value lands within that error of a figure the accepted Sources already fix,
// which is what makes this recoverable rather than a judgement call:
//
//     centre x   160 = kDisplayWidth / 2, the canvas centre
//     half-ext   148 = kCanvasContent.width / 2 — Production UI §7: "All content shall remain
//                inside canvas.inset. No component may touch the display edge."
//     apex y     196, sag 16, stroke 8 — UNCHANGED. The vertical geometry was already right.
//
// ⛔ THREE DIFFERENT WIDTHS LIVE HERE AND THEY ARE NOT INTERCHANGEABLE. Conflating them is how
// this checkpoint first claimed the ink reached x 12..308 when it actually reached 10..310.
//
//   kHorizonHalfWidth      150 -> 148   the sagitta relation's INPUT. Not drawn.
//   kHorizonHalfExtent     144          the NOMINAL path: radius * sin(half_angle).
//   kHorizonPaintedHalf    147          the PAINTED PIXELS, caps and stroke included.
//
// WHY THE PAINTED FIGURE IS THE CONTRACT. Production UI §7 constrains content, and content is
// ink. The F1 measurement observed lit pixels for the same reason. LVGL 9.5.0 draws the stroke
// INWARD from the widget radius (lv_draw_arc.c: `rin = radius - w`), so the radius is the arc's
// OUTER edge, and each rounded cap is a disc of radius w/2 centred at (radius - w/2) on the end
// angle (lv_draw_sw_arc.c, get_rounded_area). The cap therefore reaches BEYOND the path:
//
//     painted_half = floor((radius - w/2) * sin(half_angle)) + w/2
//
// AND THE BOX IS NOT SYMMETRIC ABOUT 160. Rect::right() is exclusive — kCanvasContent spans
// x 12..307 inclusive — so a painted span centred on 160 may reach 147, not 148.
//
//     radius = (half_width^2 + sag^2) / (2 * sag) = (148^2 + 16^2) / 32 = 692.5 -> 693
//     nominal  = round(693 * sin(12 deg))       = 144   ->  path at x 16 .. 304
//     painted  = floor(689 * sin(12 deg)) + 4   = 147   ->  ink  at x 13 .. 307
//
// ⚠️ THE ONE COST, STATED RATHER THAN BURIED. Flooring the half-angle to 12 degrees leaves the
// realised sag at 15 px against the relation's 16 px input, so the ends sit at y 211 rather than
// 212. The alternative that keeps 16 exactly (half width 140, radius 620, 13 degrees) paints
// only 142 either side — x 18..302, visibly short of the reference. The measured painted
// half-extent was 149.0, so the wider form is the closer match and the 1 px of sag is the price.
//
// The bounding box extends far off-canvas, which LVGL clips (§11 Clipping). That is the honest
// consequence of a kHorizonRadius-sized circle on a 240 px display, and it is what makes the
// crown shallow.
// ⚠️ NONE OF THIS HAS BEEN SEEN ON A PANEL. Every figure below is derived — from the accepted
// reference photograph, from the Sources, and from the pinned renderer's own arithmetic. The
// physical appearance of the settled horizon is UNVERIFIED and remains so until a photograph of
// this build is compared against docs/project-sources/assets/Ready Resting F1.jpeg.
inline constexpr std::int32_t kHorizonCentreX = kDisplayWidth / 2;
inline constexpr std::int32_t kHorizonHalfWidth = 148;
inline constexpr std::int32_t kHorizonRadius = 693;
inline constexpr std::int32_t kHorizonApexY = 196;
inline constexpr std::int32_t kHorizonCentreY = kHorizonApexY + kHorizonRadius;

// The nominal path's reach either side of its own centre, after the whole-degree flooring above.
// Still not what is judged: see kHorizonPaintedHalfExtent.
inline constexpr std::int32_t kHorizonHalfExtent = 144;

// ⬅️ THE CONTRACT. The outermost painted pixel either side, caps and inward stroke included.
inline constexpr std::int32_t kHorizonPaintedHalfExtent = 147;

inline constexpr Rect kRegionHorizon{kHorizonCentreX - kHorizonRadius,
                                     kHorizonCentreY - kHorizonRadius, 2 * kHorizonRadius,
                                     2 * kHorizonRadius};

// The crown's angular extent, in LVGL's convention: 0 at three o'clock, increasing clockwise,
// so 270 is the apex. asin(148 / 693) = 12 degrees either side.
// The crown's lowest points, where its ends meet the sag. This — not the apex — is what has
// to clear the content box, and asserting the apex instead was checking the wrong end of the
// arc. The relation is solved for a 16 px sag; flooring the angle realises 15.
// The relation's input. The REALISED sag is one pixel less, because the half-angle floors.
inline constexpr std::int32_t kHorizonSag = 16;
inline constexpr std::int32_t kHorizonRealisedSag = 15;
inline constexpr std::int32_t kHorizonEndY = kHorizonApexY + kHorizonRealisedSag;

inline constexpr std::int32_t kHorizonHalfAngle = 12;
inline constexpr std::int32_t kHorizonStartAngle = 270 - kHorizonHalfAngle;
inline constexpr std::int32_t kHorizonEndAngle = 270 + kHorizonHalfAngle;

static_assert(kHorizonCentreX - kHorizonPaintedHalfExtent >= kCanvasContent.x,
              "the settled horizon's PAINTED ink stays inside the content box: Production UI §7 "
              "constrains content, and content is pixels rather than a nominal path");
static_assert(kHorizonCentreX + kHorizonPaintedHalfExtent < kCanvasContent.right(),
              "Rect::right() is exclusive, so the last content pixel is 307; ink at 308 is one "
              "past the box and the earlier assertion against right() permitted exactly that");
static_assert(kHorizonPaintedHalfExtent > kHorizonHalfExtent,
              "the caps reach beyond the nominal path; if this ever inverts the painted model "
              "has stopped describing the pinned renderer");

static_assert(kHorizonStartAngle >= 0 && kHorizonEndAngle <= 360,
              "lv_arc_set_bg_end_angle() subtracts 360 from anything larger, which collapses "
              "the arc to zero extent; see the note in progress_ring.cpp");
static_assert(kHorizonRadius > kRingDiameter / 2,
              "the horizon is the ring's own circle grown, not a smaller separate arc");
static_assert(kHorizonEndY < kCanvasContent.bottom(),
              "the horizon's crown, including its sag, must sit inside the content box");
static_assert(kHorizonApexY > kRegionOperational.bottom(),
              "the horizon must sit below the operational stack, not through it");

// --- Degraded confidence ----------------------------------------------------
//
// §11.5 permits stale values to be shown only behind an EXPLICIT indicator, and UI Behaviour
// Decision 001 §8 Level 3 requires it to communicate uncertainty "without creating
// unnecessary urgency" — so it is not an Attention Indicator and does not use §8's warning or
// error colours.
//
// Owner 2026-09-07 age-copy direction: Body 14 above the Ready horizon, centred on the
// canvas; other subjects use Caption 12. Owner spacing correction: reserve space between the
// ordinary title/record group and age; keep Preparing's age above its narrowing arc. Recovery
// retires age before revealing the estimate. Native tests require breathing room, not just
// non-overlapping ink. Current-confidence endpoints and Ready placement remain unchanged.
inline constexpr std::int32_t kStaleIndicatorY = 156;
inline constexpr std::int32_t kStalePreparingY = kStaleIndicatorY - kSpace2;
inline constexpr std::int32_t kStaleReadyY = 154;

static_assert(kStaleReadyY + kLineBody < kHorizonApexY,
              "Ready age indicator stays above the resting horizon");
static_assert(kStaleIndicatorY + kLineBody < kRingCentreY + (kRingDiameter / 2),
              "the stale indicator must stay inside the ring");

// Context Region and Peripheral Region are §7 regions with no Phase C occupant.
// They are deliberately NOT given geometry here: the job filename is excluded from
// the Phase C Printing scene (owner decision D9) and the stale and attention
// indicators arrive with the scenes that need them. Inventing rectangles for
// components this phase does not build would be geometry nothing validates.

// --- Derivation invariants -------------------------------------------------

static_assert(kCanvasContent.x == 12 && kCanvasContent.y == 12, "baseline: canvas.content origin");
static_assert(kCanvasContent.width == 296 && kCanvasContent.height == 216,
              "baseline: canvas.content size");
static_assert(kRegionTelemetry.x == 212 && kRegionTelemetry.y == 12,
              "baseline: region.telemetry origin");
static_assert(kRegionTelemetry.width == 96 && kRegionTelemetry.height == 216,
              "baseline: region.telemetry size");
static_assert(kTelemetryRowHeight == 72, "baseline: telemetry.row.height");
static_assert(kRingDiameter == 192,
              "baseline: ring.diameter — under review with the operational family");
static_assert(kRingCentreX == 128 && kRingCentreY == 120, "baseline: ring.centre");
static_assert(kRegionPrimary.x == 32 && kRegionPrimary.y == 24, "baseline: region.primary origin");
static_assert(kRegionPrimary.width == 192 && kRegionPrimary.height == 192,
              "baseline: region.primary size");
// The baseline figures, asserted so a change to them is deliberate and visible in a diff
// rather than incidental. They are not a claim that the values are settled.
static_assert(kRegionOperational.x == 42 && kRegionOperational.y == 86,
              "baseline: region.operational origin");
static_assert(kRegionOperational.width == 172 && kRegionOperational.height == 90,
              "baseline: region.operational size");

// The lift is exactly one spacing step above the centred position, and it moves the whole
// block. Asserted so a later edit cannot turn a deliberate optical adjustment into an
// arbitrary offset, and so the cost below stays visible.
static_assert(kRegionOperational.y + (kLineHero / 2) + kOperationalLift == kRingCentreY,
              "the stack is lifted exactly kOperationalLift above the geometrically centred "
              "position; see the note at kOperationalLift");

// THE LIFT COSTS CHORD CLEARANCE, and this is where that is recorded.
//
// The ring's chord is widest at its centre, so moving the numeral row away from the centre
// narrows the space it has. At 100 % — the only time the value is three digits — the block is
// already the widest thing the composition draws, and it was at zero clearance before the
// lift. It now crosses the arc's advance box by a few pixels for the seconds that a print
// reads 100 %.
//
// Left as a recorded consequence rather than silently absorbed. Resolving it properly means a
// larger ring or a smaller unit, both of which are Phase D geometry decisions.
static_assert(kOperationalLift <= kSpace2,
              "a larger lift would take the three-digit case measurably into the arc");

// The rows divide the content box exactly. A remainder would leave an unowned
// strip whose ownership nothing decides.
static_assert(kTelemetryRowHeight * kTelemetryRows == kCanvasContent.height,
              "the telemetry rows must tile the region exactly");

// §7: no component may touch the display edge.
static_assert(contains(kCanvas, kCanvasContent), "content must lie inside the canvas");
static_assert(kCanvasContent.x > 0 && kCanvasContent.right() < kDisplayWidth,
              "content must not touch the horizontal edges");
static_assert(kCanvasContent.y > 0 && kCanvasContent.bottom() < kDisplayHeight,
              "content must not touch the vertical edges");
static_assert(contains(kCanvasContent, kRegionTelemetry),
              "the telemetry region must lie inside the content box");

// The ring respects the inset even though it is centred on the panel rather than
// on the content box. Checked, not assumed — this is the one region whose origin
// is not derived from kCanvasContent.
static_assert(contains(kCanvasContent, kRegionPrimary), "the ring must lie inside the content box");

// The overlap accepted with D8, asserted so that a later geometry change cannot
// remove it silently and leave the comment above describing something untrue.
static_assert(overlaps(kRegionPrimary, kRegionTelemetry),
              "the baseline has region.primary overlapping region.telemetry; see the note above");
static_assert(kRegionPrimary.right() - kRegionTelemetry.x == 12,
              "baseline: the overlap is 12 px — one of the things the family review settles");

// The text stack sits inside the ring, clear of the stroke.
static_assert(contains(kRegionPrimary, kRegionOperational),
              "the operational region must lie inside the ring");
static_assert(kRegionOperational.x - kRegionPrimary.x > kRingStroke,
              "the text stack must clear the arc stroke");

// The operational text stack must be tall enough for the three lines UIDD 001
// places in it, at their §4 line heights.
static_assert(kLineDisplay + kLineTitle + kLineBody <= kRegionOperational.height,
              "the operational region must fit the primary value, state title and duration");

// Owner-accepted central printer-error composition (2026-09-10).
inline constexpr std::int32_t kPrinterErrorIconTop = 72;
inline constexpr std::int32_t kPrinterErrorIconHalfWidth = 20;
inline constexpr std::int32_t kPrinterErrorIconHeight = 32;
inline constexpr std::int32_t kPrinterErrorTitleY = 116;
inline constexpr std::int32_t kPrinterErrorTitleSize = 22;
inline constexpr std::int32_t kPrinterErrorContextY = 148;
inline constexpr std::int32_t kPrinterErrorAgeY = 156;

// --- The shared message composition (owner-accepted 2026-09-12) -------------
//
// One full-screen statement, drawn by component::MessageForm for every family that states a
// condition in words: the two access failures, the four device-fault statements, the recovery
// statement and the factory-reset question. Production UI §9's access refinement fixed this
// geometry for the access errors; the accepted display atlas extends the same one to the rest,
// so there is one message composition rather than three that drift.
//
// ⛔ THE SYMBOL IS THE OPERATIONAL TRIANGLE, NOT A SECOND ONE. §8's icon rules — "share visual
// construction", "do not introduce stylistic variants" — and the atlas review both land in the
// same place: the 40 × 32 mark the printer-error composition already draws, at the same
// emphasis stroke, with the same glyph. What differs is only where it sits.
//
// The origin below is the LINE PATH's, and the stroke straddles it: a 4 px stroke paints 2 px
// either side, so the painted mark is 44 × 36 beginning at (14, 24) and ending at y 60 — four
// pixels clear of the identity row. Measured against the renderer rather than predicted; the
// same relationship the operational mark's own crop confirmed.
inline constexpr std::int32_t kMessageSymbolPaintedTop = kSpace6;
inline constexpr std::int32_t kMessageSymbolX = kCanvasContent.x + kSpace1;
inline constexpr std::int32_t kMessageSymbolY = kMessageSymbolPaintedTop + (kStrokeEmphasis / 2);
inline constexpr std::int32_t kMessageSymbolWidth = 2 * kPrinterErrorIconHalfWidth;
inline constexpr std::int32_t kMessageSymbolHeight = kPrinterErrorIconHeight;

// The mark sits the same space.2 below the triangle's apex as it does in the ring.
inline constexpr std::int32_t kMessageMarkOffsetY = kSpaceCompact;
inline constexpr std::int32_t kMessageMarkSize = kPrinterErrorTitleSize;

// The acquisition outline this composition morphs from — the accepted Connecting endpoint,
// unchanged. Its own 32 × 28 painted box at the standard stroke.
inline constexpr std::int32_t kAcquisitionSymbolY = kSpace8;

inline constexpr std::int32_t kMessageX = kCanvasContent.x;
inline constexpr std::int32_t kMessageWidth = kCanvasContent.width;
inline constexpr std::int32_t kMessageIdentityY = kSpace8 * 2;
inline constexpr std::int32_t kMessageTitleY = kMessageIdentityY + kSpace6;

// The accepted faces: the existing 24 px semibold title and 20 px semibold explanation. Neither
// is a new font asset — both already ship for the access errors — and 20 px is §4's own
// `font.size.title`, so only the title size is stated as its own figure.
inline constexpr std::int32_t kMessageTitleSize = 24;
inline constexpr std::int32_t kMessageExplanationSize = kFontSizeTitle;

// Their line boxes, measured on the committed faces rather than derived from the sizes.
// fonts::verify() checks every face against its token at start-up for exactly this reason.
inline constexpr std::int32_t kMessageTitleLine = 28;
inline constexpr std::int32_t kMessageExplanationLine = kLineTitle;

// "space.6 below the actual laid-out title box, not a nominal one-line baseline" — §9's access
// refinement, kept verbatim because a title that wrapped would otherwise collide.
inline constexpr std::int32_t kMessageExplanationGap = kSpace6;

// Three lines of explanation, which is what the longest accepted copy needs and what the canvas
// has room for. The fourth would leave the content area, so the budget is stated here and the
// drawn height is measured against it at run time rather than assumed.
inline constexpr std::int32_t kMessageExplanationLines = 3;
inline constexpr std::int32_t kMessageExplanationY =
    kMessageTitleY + kMessageTitleLine + kMessageExplanationGap;

static_assert(kMessageSymbolPaintedTop >= kCanvasContent.y,
              "the painted symbol must start inside the content area");
static_assert(kMessageSymbolY + kMessageSymbolHeight + (kStrokeEmphasis / 2) < kMessageIdentityY,
              "the painted symbol must clear the identity row beneath it");
static_assert(kMessageIdentityY + kLineSection <= kMessageTitleY,
              "the identity row must clear the title beneath it");
static_assert(kMessageExplanationY + (kMessageExplanationLines * kMessageExplanationLine) <=
                  kCanvasContent.bottom(),
              "the explanation must fit three of its own lines inside the content area");
static_assert(kMessageExplanationY == kMessageTitleY + kMessageTitleLine + kMessageExplanationGap,
              "the explanation sits one title line box and the region gap below the title");

// ⛔ AND A FOURTH LINE REALLY WOULD LEAVE THE CANVAS. Stated as its own assertion so the budget
// above is a measured ceiling and not a preference: the negative control for the one above it.
static_assert(kMessageExplanationY + ((kMessageExplanationLines + 1) * kMessageExplanationLine) >
                  kCanvasContent.bottom(),
              "three lines is the ceiling, not a comfortable choice");

// --- Firmware update -------------------------------------------------------
// Owner-accepted native preview, 2026-09-19. This family deliberately does not reuse the
// operational ring: transfer progress is update progress, not printer progress.
inline constexpr Rect kUpdateReadyMark{
    (kDisplayWidth - kSpace8 - kSpace1) / 2,
    kSpace8 * 2 + kSpace2 - kStrokeStandard,
    kSpace8 + kSpace1,
    kSpace8 + kSpace1,
};
inline constexpr Rect kUpdateDownloadMark{kCanvasContent.x, kSpace8 + kSpace2 + kStrokeStandard,
                                          kSpace8 * 2 + kSpace6, kSpace8 * 2 + kSpace6};
inline constexpr Rect kUpdateDownloadMarkLocal{0, 0, kUpdateDownloadMark.width,
                                               kUpdateDownloadMark.height};
inline constexpr std::int32_t kUpdateVersionY = kCanvasContent.y + kStrokeStandard;
inline constexpr std::int32_t kUpdateValueX = kUpdateDownloadMark.right() + kSpace3;
inline constexpr std::int32_t kUpdateValueY = kSpace8 + kSpace5;
inline constexpr std::int32_t kUpdateValueMaxWidth = kSpace8 * 3 + kSpace4;
inline constexpr std::int32_t kUpdateValueSize = 58;
inline constexpr std::int32_t kUpdateUnitY = kSpace8 * 2 + kSpace3;
inline constexpr std::int32_t kUpdateUnitWidth = kSpace8 + kSpace3;
inline constexpr std::int32_t kUpdateUnitSize = kFontSizeDisplay - kSpace1 - kStrokeHairline;
inline constexpr std::int32_t kUpdateTitleY = kSpace8 * 4 + kLineBody;
inline constexpr std::int32_t kUpdateMessageY = kUpdateTitleY + kLineTitle + kSpace4;
inline constexpr std::int32_t kUpdateProcessTitleY = kSpace8 * 3 + kSpace5;

static_assert(contains(kCanvasContent, kUpdateReadyMark),
              "the Ready availability mark stays inside the content box");
static_assert(contains(kCanvasContent, kUpdateDownloadMark),
              "the update download mark stays inside the content box");
static_assert(kUpdateValueX + kUpdateValueMaxWidth + kUpdateUnitWidth <= kCanvasContent.right(),
              "the three-digit download value and unit fit the content box");
static_assert(kUpdateMessageY + kLineBody <= kCanvasContent.bottom(),
              "the download instruction fits the content box");
static_assert(kUpdateProcessTitleY + kLineTitle <= kCanvasContent.bottom(),
              "the process title fits the content box");

} // namespace prd::ui::theme
