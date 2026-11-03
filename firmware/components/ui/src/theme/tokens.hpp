// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

// The canonical token source — Production UI Specification §3 to §6.
//
// THIS FILE IS THE ONLY PLACE A PRODUCTION VALUE MAY APPEAR. §2's Token Rules:
// "Literal production values are prohibited outside their token definitions" and
// "Every reusable measurement shall reference a production token". Enforced
// mechanically by firmware/tools/check-tokens.sh, which fails if a hex colour or a
// bare pixel literal appears in any component or scene source outside this
// directory — so the rule is a build check rather than a review promise.
//
// DELIBERATELY FREE OF LVGL. No lv_color_t, no lv_font_t, no LVGL header. That
// keeps this file compilable by the host test build, so the token invariants below
// are asserted by tests rather than by inspection. The binding from these values to
// LVGL types lives in theme.hpp, which is the only file that needs both.
//
// The values are transcribed from the specification, not derived. Where the
// specification defines a token in terms of another (`space.compact` is `space.2`),
// that relationship is expressed here as a reference and not as a repeated
// literal — a transcription error then cannot make two tokens disagree.
//
// THREE VALUES DIVERGE FROM THE SPECIFICATION, AND ALL THREE ARE PROVISIONAL.
// `font.size.hero` (§4 has no such token), `ring.stroke` in layout.hpp (§6 says 4) and
// `line.micro` (§4 says 12). Each was set against the Printing screen alone, on owner
// instruction to match the accepted artwork, and each is marked at its definition.
//
// None is settled. Owner position, 2026-08-06: the divergences are validated against the
// COMPLETE operational family and the Production UI Specification is updated once, not
// screen by screen. Until then they are the working baseline — do not cite them as accepted,
// and do not propagate them into a new screen without saying that is what is happening.

namespace prd::ui::theme {

// --- §3 Foundation ---------------------------------------------------------

inline constexpr std::int32_t kUnitBase = 4;

inline constexpr std::int32_t kCanvasInset = 12;

// The spacing scale. §3: "No additional spacing values are permitted."
inline constexpr std::int32_t kSpace0 = 0;
inline constexpr std::int32_t kSpace1 = 4;
inline constexpr std::int32_t kSpace2 = 8;
inline constexpr std::int32_t kSpace3 = 12;
inline constexpr std::int32_t kSpace4 = 16;
inline constexpr std::int32_t kSpace5 = 20;
inline constexpr std::int32_t kSpace6 = 24;
inline constexpr std::int32_t kSpace8 = 32;

// Structural tokens. Defined by reference exactly as §3 defines them.
inline constexpr std::int32_t kSpaceCompact = kSpace2;
inline constexpr std::int32_t kSpaceComponent = kSpace3;
inline constexpr std::int32_t kSpaceRegion = kSpace4;

inline constexpr std::int32_t kPaddingCompact = kSpace2;
inline constexpr std::int32_t kPaddingStandard = kSpace3;
inline constexpr std::int32_t kPaddingRegion = kSpace4;

inline constexpr std::int32_t kModuleMin = kSpace4;
inline constexpr std::int32_t kModuleStandard = kSpace8;

// --- §4 Typography ---------------------------------------------------------

// Weights are the specification's numeric values. They select a face rather than
// being applied at run time: LVGL renders a pre-rasterised bitmap font, so the
// weight is chosen when the font is generated (see tools/generate-fonts.py) and
// these exist so the token table and the generator cannot silently disagree.
inline constexpr std::int32_t kFontWeightRegular = 400;
inline constexpr std::int32_t kFontWeightMedium = 500;
inline constexpr std::int32_t kFontWeightSemibold = 600;
inline constexpr std::int32_t kFontWeightBold = 700;

// NOT FROM §4's TABLE. See the note at TextRole::PrimaryValue below and in
// tools/generate-fonts.py: §4's `font.size.display` produces a primary value at under
// half the accepted artwork's visual weight, and owner instruction 2026-08-06 directs
// matching the artwork. This is the artwork-derived size for the primary value; §4's own
// figure is retained immediately below as the size of its unit.
inline constexpr std::int32_t kFontSizeHero = 68;

inline constexpr std::int32_t kFontSizeDisplay = 36;
inline constexpr std::int32_t kFontSizeTitle = 20;
inline constexpr std::int32_t kFontSizeOutcome = 28;
inline constexpr std::int32_t kFontSizeSection = 16;
inline constexpr std::int32_t kFontSizeBody = 14;
inline constexpr std::int32_t kFontSizeCaption = 12;
inline constexpr std::int32_t kFontSizeMicro = 10;

// The hero face's subset is digits and numeric punctuation, so it has no descenders and
// its line box is its cap height — 0.764 em, measured on the generated face rather than
// taken from the font tables. 49 px at 64, rounded up to the next scale step.
//
// fonts::verify() compares every face's actual line_height against its token at start-up
// and reports a face that overflows its row. That check exists because the first version
// of this token was set from a predicted ratio and came out 3 px too small, which would
// have pushed the numeral into the row below it.
inline constexpr std::int32_t kLineHero = 52;

inline constexpr std::int32_t kLineDisplay = 40;
inline constexpr std::int32_t kLineTitle = 24;
// Measured on the owner-reviewed Outcome28 face; not its nominal size plus a guessed gap.
inline constexpr std::int32_t kLineOutcome = 35;
inline constexpr std::int32_t kLineSection = 20;
inline constexpr std::int32_t kLineBody = 18;
inline constexpr std::int32_t kLineCaption = 16;
// §4's table says 12. Inter at 10 px rasterises to a 13 px line box, measured on the
// generated face, so 12 cannot hold it and the text would overflow into whatever sits
// below. The other five sizes match §4 exactly — 14 → 18, 16 → 20, 20 → 24 — which
// suggests the table was derived from Inter and this row is one pixel out. Corrected to
// what the face actually needs, and recorded as a divergence from a binding value rather
// than silently rounded.
inline constexpr std::int32_t kLineMicro = 13;

inline constexpr std::int32_t kTrackingDefault = 0;
inline constexpr std::int32_t kTrackingNumeric = 0;

// The §4 Text Roles table, as an enumeration. A scene names a role; it never
// names a size and a weight separately, because the pairing is what §4 specifies
// and splitting it at a use site is how a screen-specific style gets introduced
// (§2: "Screen-specific styling is prohibited").
enum class TextRole {
    OutcomeTitle,
    StateTitle, // font.size.title    + font.weight.semibold

    // §4 pairs the primary value with `font.size.display`. It is paired with
    // `font.size.hero` instead, on owner instruction 2026-08-06, because 36 px produces a
    // numeral at under half the accepted UIDD 001 artwork's weight and the artwork is what
    // the composition has to reproduce (§7: "preserve the optical balance established by
    // the accepted artwork"). A documented divergence from a binding token, not an
    // oversight.
    PrimaryValue, // font.size.hero     + font.weight.bold

    // The unit that accompanies the primary value — the `%` in UIDD 001's centre stack.
    // §4 names no role for it, and it cannot share the primary value's role: at 72 px a
    // unit would compete with the number it qualifies. §4's own `font.size.display` is
    // used here, which puts the unit at roughly half the numeral's height, as the artwork
    // shows.
    PrimaryUnit, // font.size.display  + font.weight.bold

    Section,  // font.size.section  + font.weight.medium
    Body,     // font.size.body     + font.weight.regular
    Label,    // font.size.caption  + font.weight.medium
    Metadata, // font.size.micro    + font.weight.regular
};

inline constexpr std::size_t kTextRoleCount = 8;

[[nodiscard]] std::int32_t font_size(TextRole role);
[[nodiscard]] std::int32_t line_height(TextRole role);
[[nodiscard]] std::int32_t font_weight(TextRole role);

// --- §5 Colour -------------------------------------------------------------

// 0xRRGGBB. §5's semantic tokens, and the only place these values appear.
inline constexpr std::uint32_t kColorBackground = 0x111315;
inline constexpr std::uint32_t kColorSurface = 0x181B1F;
inline constexpr std::uint32_t kColorPrimary = 0xF2F3F4;
inline constexpr std::uint32_t kColorSecondary = 0xB7BDC5;
inline constexpr std::uint32_t kColorTertiary = 0x7C848E;
inline constexpr std::uint32_t kColorAccent = 0xF77F2F;
inline constexpr std::uint32_t kColorSuccess = 0x5CB85C;
// ⬅️ REVISED AT MILESTONE 3.11 T20 BY OWNER/PM DECISION 2026-08-22, from the specification's
// `#F2B84B` to `#ED9A2E`, on the evidence of on-glass validation.
//
// ⛔ THE DATA PATH WAS AUDITED FIRST AND IS CORRECT. `#F2B84B` quantises to RGB565 `0xF5C9` —
// RGB(246,186,74) — and the framebuffer provably carried exactly that. Byte order
// (lv_draw_sw_rgb565_swap against the panel's big-endian COLMOD/RAMCTRL path), colour order,
// inversion and the Phase C1 gamma table were all checked and left untouched. The token was
// rendered faithfully; it simply read as lime on this panel rather than as a warning.
//
// Candidates were judged on the board against the production background, labelled neutrally, with
// `color.accent` shown for family reference. `#ED9A2E` was chosen: hue 34.2° against accent's
// 24.5°, contrast 8.45:1 on `color.background`, ΔE 19.3 from accent — far enough to read as a
// separate semantic rather than as a poor reproduction of the accent, which is what ruled out the
// warmer candidate beside it.
//
// ⛔ THE SPECIFICATION STILL SAYS `#F2B84B`. This is a recorded divergence pending a Production UI
// revision — see current-status.md. Not a silent correction.
//
// ⚠️ REVALIDATION IS OWED. The judgement was made on a bare module with no cover glass. The final
// enclosure changes both perceived contrast and hue, and this value must be confirmed again then.
inline constexpr std::uint32_t kColorWarning = 0xED9A2E;
inline constexpr std::uint32_t kColorError = 0xE06262;

// §5 specifies `color.disabled` as "Derived from `color.tertiary`" without giving
// the derivation. Implemented as tertiary at `opacity.disabled`, which is the
// only derivation expressible in the token set itself and introduces no new
// value. Recorded here rather than chosen at a use site.
inline constexpr std::uint32_t kColorDisabled = kColorTertiary;

// Percentages as §5 states them. The conversion to LVGL's 0...255 opacity lives in
// theme.hpp; keeping the token in the specification's own unit means a reader can
// check this table against the document line by line.
inline constexpr std::uint8_t kOpacityFullPercent = 100;
inline constexpr std::uint8_t kOpacityHighPercent = 88;
inline constexpr std::uint8_t kOpacityMediumPercent = 64;
inline constexpr std::uint8_t kOpacityLowPercent = 40;
inline constexpr std::uint8_t kOpacityDisabledPercent = 24;

// --- §5.1's accent -----------------------------------------------------------
//
// UI Design Decision 004 §5.1, as ruled by the owner on 2026-09-01: "There shall be one accent
// colour, `color.accent`, and it is applied to the operational form alone ... The accent does not
// vary with energy."
//
// ⛔ THERE IS NO RAMP, AND THAT IS THE POINT. `kColorAccent` above is the whole of the accent.
// There is no second endpoint, no scale between two ends and no level to interpolate, so no
// arithmetic in this project can attenuate the accent — the guarantee is structural rather than a
// convention a call site has to keep.
//
// WHAT WAS HERE, AND WHY IT IS GONE. Until 2026-09-01 this block carried `kAccentLevelFull = 255`,
// a WORKING-BASELINE `kAccentLevelRest = 102` (`opacity.low`'s intensity), and the accent's three
// channels split out so the ramp could scale them. That ramp rendered Ready's form at `#623212`.
// The owner rejected both the endpoint and the mechanism: energy is expressed by the form's shape,
// position and sweep (§3) and by the composition's luminance (§4), and display power is expressed
// by the physical backlight (UI Behaviour Decision 001 §10). A resting form must not be dimmed
// twice — once in RGB and again by the backlight.
//
// ⬅️ §4'S BODY RAMP BELOW IS A DIFFERENT RULE AND IS UNTOUCHED. It governs the composition's
// achromatic text and telemetry, never the form, and the two have always applied to disjoint
// elements. Removing the accent ramp does not put any element under two functions of the axis;
// it leaves the form under none.

// --- §4's composition luminance, as a ramp ---------------------------------
//
// UI Design Decision 004 §4: "The composition recedes as one body as energy falls. In every
// state, luminance is floored by legibility of primary operational information at approximately
// one to two metres. Low energy shall never mean difficult to read."
//
// THE ONLY FUNCTION OF THE AXIS THAT TOUCHES COLOUR OR LUMINANCE, since 2026-09-01. §5.1 gives the
// operational form one accent, `color.accent`, at full opacity and at every energy; §4 governs the
// composition's achromatic body, and the two apply to disjoint elements. The form is under no
// luminance function at all — the accent ramp that used to be the second one is rejected.
inline constexpr std::int32_t kBodyLevelFull = 255;

// ---------------------------------------------------------------------------
// WORKING BASELINE — NOT AN ACCEPTED VALUE
// ---------------------------------------------------------------------------
//
// The composition's luminance at the bottom of the axis. §4 floors it by legibility at one to
// two metres, and §15 places that judgement against the physical panel — so this end is
// provisional and is settled in Phase H, once, against the complete family. It is expressly not
// tuned toward a guess about how the panel will render it.
//
// `opacity.medium`, BECAUSE IT IS §5's OWN MIDDLE STEP rather than a number anyone invented. It
// is also the most recessive step the composition already asks a reader to read — the telemetry
// row labels sit there today and were validated on the panel at E.6 — so the floor is anchored
// to a level that has been seen rather than to an untested one. Conservative on purpose: §4's
// requirement is a floor, and the failure it guards against is illegibility.
//
// IT COMPOUNDS WITH THE ROLE OPACITIES, and that is the whole point of "as one body": a level of
// 64 % applied to a label already at `opacity.medium` leaves it at roughly 41 %. The internal
// hierarchy is preserved exactly because every element is scaled by the same factor. Whether the
// dimmest element in that hierarchy is still legible at Ready is the specific question the
// hardware review has to answer, and it is why the floor is not fixed here.
inline constexpr std::int32_t kBodyLevelRest =
    kBodyLevelFull * kOpacityMediumPercent / kOpacityFullPercent;

static_assert(kBodyLevelRest == 163, "baseline: the resting composition sits at opacity.medium");
static_assert(kBodyLevelRest > 0, "§4: low energy shall never mean difficult to read");
static_assert(kBodyLevelRest < kBodyLevelFull, "§4: the composition recedes as energy falls");

// Semantic colour names, so a component asks for meaning and not for a value.
//
// Here rather than in theme.hpp because this file has no LVGL include: pure code needs to
// name a colour — the operational content table chooses `color.success` for Completed — and
// it must be able to do that without depending on the renderer. theme.hpp maps these to
// lv_color_t and is the only file that does.
enum class Colour : std::uint8_t {
    Background,
    Surface,
    Primary,
    Secondary,
    Tertiary,
    Accent,
    Success,
    Warning,
    Error,
    Disabled,
};

// --- §6 Geometry -----------------------------------------------------------

inline constexpr std::int32_t kRadiusNone = 0;
inline constexpr std::int32_t kRadiusSmall = 4;
inline constexpr std::int32_t kRadiusStandard = 8;
inline constexpr std::int32_t kRadiusLarge = 12;

inline constexpr std::int32_t kStrokeHairline = 1;
inline constexpr std::int32_t kStrokeStandard = 2;
inline constexpr std::int32_t kStrokeEmphasis = 4;

// --- Token invariants ------------------------------------------------------
//
// Compile-time, so a transcription error cannot reach a build. These are not
// restatements of the values: each one is a property the specification asserts
// about the set, which a single mistyped number would break.

// §3: the scale is built on `unit.base`.
static_assert(kSpace1 == kUnitBase, "space.1 is one base unit");
static_assert(kSpace2 == 2 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kSpace3 == 3 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kSpace4 == 4 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kSpace5 == 5 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kSpace6 == 6 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kSpace8 == 8 * kUnitBase, "the spacing scale is a multiple of unit.base");
static_assert(kCanvasInset == kSpace3, "canvas.inset coincides with space.3");

// §4: every line height exceeds its font size, and both descend monotonically
// through the role table. A swapped pair would still compile and would clip text.
static_assert(kLineDisplay > kFontSizeDisplay, "line height must exceed font size");
static_assert(kLineTitle > kFontSizeTitle, "line height must exceed font size");
static_assert(kLineSection > kFontSizeSection, "line height must exceed font size");
static_assert(kLineBody > kFontSizeBody, "line height must exceed font size");
static_assert(kLineCaption > kFontSizeCaption, "line height must exceed font size");
static_assert(kLineMicro > kFontSizeMicro, "line height must exceed font size");

// THE HERO ROLE IS THE ONE EXCEPTION TO "line height exceeds font size".
//
// That rule holds for prose faces, where descenders sit below the baseline and the line box
// has to contain them. The hero face's subset is digits and numeric punctuation only, so
// nothing descends and its line box IS its cap height — 0.7275 em, or 52 px at 72. A line
// box of 76 px would reserve 24 px of vertical space that no glyph can ever occupy, and
// inside a 192 px ring that space is not available to waste.
//
// The floor asserted here is the cap height, which is what the row must actually hold. The
// generated face's own line_height is reported at start-up by fonts::verify(), so the
// figure below is checked against the rasteriser rather than trusted.
static_assert(kLineHero >= kFontSizeHero * 72 / 100,
              "the hero line box must hold the cap height of its face");
static_assert(kLineHero < kFontSizeHero,
              "the hero face has no descenders; a line box larger than its em would reserve "
              "space no glyph can occupy");
static_assert(kFontSizeHero > kFontSizeDisplay, "the primary value must exceed its unit");
static_assert(kFontSizeDisplay > kFontSizeTitle, "§4 sizes descend");
static_assert(kFontSizeTitle > kFontSizeSection, "§4 sizes descend");
static_assert(kFontSizeSection > kFontSizeBody, "§4 sizes descend");
static_assert(kFontSizeBody > kFontSizeCaption, "§4 sizes descend");
static_assert(kFontSizeCaption > kFontSizeMicro, "§4 sizes descend");

static_assert(kFontWeightRegular < kFontWeightMedium, "§4 weights ascend");
static_assert(kFontWeightMedium < kFontWeightSemibold, "§4 weights ascend");
static_assert(kFontWeightSemibold < kFontWeightBold, "§4 weights ascend");

// §5: opacity descends through the scale, and `opacity.full` is full.
static_assert(kOpacityFullPercent == 100, "opacity.full is 100%");
static_assert(kOpacityHighPercent < kOpacityFullPercent, "§5 opacity descends");
static_assert(kOpacityMediumPercent < kOpacityHighPercent, "§5 opacity descends");
static_assert(kOpacityLowPercent < kOpacityMediumPercent, "§5 opacity descends");
static_assert(kOpacityDisabledPercent < kOpacityLowPercent, "§5 opacity descends");

// §6: strokes ascend, radii ascend, and every value is a whole pixel (§11:
// "Geometry aligns to whole pixels"). The types make the last one structural.
static_assert(kStrokeHairline < kStrokeStandard, "§6 strokes ascend");
static_assert(kStrokeStandard < kStrokeEmphasis, "§6 strokes ascend");
static_assert(kRadiusNone < kRadiusSmall, "§6 radii ascend");
static_assert(kRadiusSmall < kRadiusStandard, "§6 radii ascend");
static_assert(kRadiusStandard < kRadiusLarge, "§6 radii ascend");

// §12 Contrast: "Primary typography shall maintain maximum contrast against the
// background." Asserted as an ordering of luminance rather than a ratio, because
// §5's own rule is "Hierarchy shall primarily be established through luminance" —
// so the property to protect is that the hierarchy never inverts.
inline constexpr std::uint32_t relative_luminance(const std::uint32_t rgb) {
    // Integer approximation of Rec. 601 luma, scaled by 1000. Exact enough to
    // order six greys and cheap enough to evaluate at compile time.
    const std::uint32_t red = (rgb >> 16U) & 0xFFU;
    const std::uint32_t green = (rgb >> 8U) & 0xFFU;
    const std::uint32_t blue = rgb & 0xFFU;
    return ((299U * red) + (587U * green) + (114U * blue)) / 255U;
}

static_assert(relative_luminance(kColorPrimary) > relative_luminance(kColorSecondary),
              "§5 luminance hierarchy: primary is brighter than secondary");
static_assert(relative_luminance(kColorSecondary) > relative_luminance(kColorTertiary),
              "§5 luminance hierarchy: secondary is brighter than tertiary");
static_assert(relative_luminance(kColorTertiary) > relative_luminance(kColorSurface),
              "§5 luminance hierarchy: tertiary is brighter than the surface");
static_assert(relative_luminance(kColorSurface) > relative_luminance(kColorBackground),
              "§5: the surface is raised above the background");

} // namespace prd::ui::theme
