// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

// Static display-validation patterns (Milestone 1 Phase C).
//
// These are diagnostics owned by the display component, not product UI: they
// carry no state, no layout system and no product vocabulary. They exist only
// to expose panel defects that a product screen would hide — orientation,
// offsets, clipping, colour order, byte order, inversion, near-black
// reproduction and pixel alignment.
//
// Compiled only into the hardware-validation profile; the whole translation
// unit is excluded from debug and release builds.

namespace prd::display::patterns {

enum class Pattern {
    CornerMarkers, // orientation, offsets, clipping, pixel alignment
    ColourBars,    // RGB/BGR order and RGB565 byte order
    Checkerboard,  // 1 px alignment and tearing
    Gradient,      // banding and gamma
    Black,         // uniformity, inversion, backlight bleed
    White,         // uniformity
    NearBlack,     // #111315 — the accepted UI background
    Orange,        // #F77F2F — the accepted UI accent

    // Phase C1 colour A/B chart: every reference swatch on one screen so the
    // panel can be photographed against the same colours on a reference
    // display. Carries a mode-marker strip so the build under test is never
    // ambiguous in a photograph.
    ComparisonChart,

    // Phase D backlight sweep: a white field, because maximum light output makes
    // flicker and camera banding as visible as they can be, plus a marker strip
    // identifying which PWM frequency is on screen. Without it an observer
    // watching an unattended sweep cannot tie what they see to a frequency.
    //
    // Reached only through draw_marked_white(); it is not part of the cycle.
    MarkedWhite,
};

[[nodiscard]] std::string_view name(Pattern pattern);

// Renders one pattern over the full transformed area. Returns false if any
// blit failed.
[[nodiscard]] bool draw(Pattern pattern);

// Renders the comparison chart with `marker_count` marker squares in the
// bottom strip, identifying which tuning mode produced the image (A=1 .. D=4).
[[nodiscard]] bool draw_comparison_chart(int marker_count);

// Renders the white field with `marker_count` marker squares, identifying which
// entry in the PWM frequency sweep is on screen (1 = the first frequency).
[[nodiscard]] bool draw_marked_white(int marker_count);

// Advances to the next pattern in the cycle.
[[nodiscard]] Pattern next(Pattern pattern);

} // namespace prd::display::patterns
