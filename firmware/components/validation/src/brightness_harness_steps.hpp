// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

// The owner-paced brightness harness's step table.
//
// ⛔ WHY THIS REPLACED AN AUTOMATIC LADDER, WHICH IS THE WHOLE POINT OF THE FILE. The first
// instrument advanced itself every 8-15 seconds and named each step in chat. The announcements
// arrived in a batch, the owner could not tell which level was on screen, and the run was ruled
// INVALID for selection. Chat timing is not a test oracle. So: nothing here advances on its own,
// every step holds until a person presses the button, and the PANEL carries the label.
//
// ⛔ THERE IS NO DWELL FIELD, AND THAT IS THE STRUCTURAL GUARANTEE. `fade_ms` is the duration of
// the TRANSITION INTO a step, never a hold. A step cannot expire because nothing here records how
// long it has been showing, so no timer can advance it — the same discipline display_policy's
// Situation uses, where the defect is prevented by the shape of the type.
//
// ⬅️ DELIBERATELY FREE OF ESP-IDF AND OF sdkconfig, so the sequence is exercised by the host
// suite. The flow that drives it is gated; this table is data and is only ever included by that
// gated flow, so it reaches no production image either.

namespace prd::validation::brightness_harness {

enum class Phase : std::uint8_t {
    // Which On ceiling to use. Judged twice, independently: once as the ACTIVE level for
    // Printing, setup and actionable states, once as the QUIET level for a resting Ready and for
    // a Completed or Unavailable acknowledgement. The two answers may coincide; the harness must
    // not assume it.
    Ceiling,

    // A fraction of the reference ceiling's emitted LIGHT. Judged at one ceiling only, because
    // the gamma correction is precisely what makes a light fraction mean the same thing at every
    // ceiling.
    Dim,

    // The production recession, at the production fade duration, plus the wake.
    Fade,
};

struct Step {
    // Short identifier, drawn large on the panel. ASCII only: the 48 px face is LVGL's built-in
    // Montserrat, whose first cmap covers U+0020-U+007E and nothing below it.
    const char* id;

    // The full label, drawn beneath the identifier and reproduced verbatim in the serial row.
    //
    // ⚠️ THE SEPARATOR IS U+2022 BULLET, NOT U+00B7 MIDDLE DOT. Checked against
    // lv_font_montserrat_14.c rather than assumed: its cmaps are U+0020-U+007E, then a SPARSE
    // list from U+00B0 whose first two entries are U+00B0 and U+2022. U+00B7 is absent, and an
    // absent glyph on an instrument's own label would defeat the instrument.
    const char* label;

    Phase phase;

    // The On ceiling this step is expressed against.
    std::uint8_t ceiling_percent;

    // Fraction of `ceiling_percent`'s emitted light. 100 is the ceiling itself; 0 is true off.
    // Converted to a control percentage by display::percent_for_light_fraction(), which owns the
    // panel's gamma — never by arithmetic here.
    std::uint8_t light_percent;

    // Duration of the transition INTO this step. NOT a hold: see the note above.
    std::uint32_t fade_ms;
};

// The reference ceiling the Dim and Fade steps are expressed against.
inline constexpr std::uint8_t kReferenceCeiling = 80;

// The production recession's fade duration — display_policy::Timings::fade_ms.
inline constexpr std::uint32_t kProductionFadeMs = 2000;

// The sequence, in the order one press walks it.
//
// ⬅️ F3 IS A LABEL, NOT A LEVEL. It sits at F2's brightness and only changes what the panel says,
// because the owner has to be able to READ that the next press will darken the panel before the
// panel goes dark. Pressing at F3 performs the fade to true off; pressing while off wakes to F4.
inline constexpr Step kSteps[] = {
    {"C1", "C1 • CEILING 50%", Phase::Ceiling, 50, 100, 0},
    {"C2", "C2 • CEILING 65%", Phase::Ceiling, 65, 100, 0},
    {"C3", "C3 • CEILING 80%", Phase::Ceiling, 80, 100, 0},
    {"C4", "C4 • CEILING 100%", Phase::Ceiling, 100, 100, 0},

    {"D1", "D1 • DIM 30% LIGHT", Phase::Dim, kReferenceCeiling, 30, 0},
    {"D2", "D2 • DIM 20% LIGHT", Phase::Dim, kReferenceCeiling, 20, 0},
    {"D3", "D3 • DIM 12% LIGHT", Phase::Dim, kReferenceCeiling, 12, 0},
    {"D4", "D4 • DIM 6% LIGHT", Phase::Dim, kReferenceCeiling, 6, 0},

    {"F1", "F1 • 80% • PRESS TO FADE", Phase::Fade, kReferenceCeiling, 100, 0},
    {"F2", "F2 • 15% LIGHT", Phase::Fade, kReferenceCeiling, 15, kProductionFadeMs},
    {"F3", "F3 • PRESSING WILL FADE OFF", Phase::Fade, kReferenceCeiling, 15, 0},
    {"OFF", "OFF", Phase::Fade, kReferenceCeiling, 0, kProductionFadeMs},
    {"F4", "F4 • BACK ON 80%", Phase::Fade, kReferenceCeiling, 100, 0},
};

inline constexpr std::size_t kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);

// Named indices, so the flow and the tests refer to the same steps by meaning rather than by a
// literal that a table edit could silently move.
inline constexpr std::size_t kFirstStep = 0;
inline constexpr std::size_t kIndexF1 = 8;
inline constexpr std::size_t kIndexF2 = 9;
inline constexpr std::size_t kIndexF3 = 10;
inline constexpr std::size_t kIndexOff = 11;
inline constexpr std::size_t kIndexF4 = 12;

// One press, one step. The ONLY way the index ever moves.
//
// ⛔ NO CLOCK, NO INTERVAL, NO ELAPSED TIME. A time argument here is what would reintroduce the
// invalidated instrument, so there is nowhere to pass one.
[[nodiscard]] constexpr std::size_t next_index(const std::size_t current) {
    return current + 1 >= kStepCount ? kFirstStep : current + 1;
}

// Whether a step drives the backlight to true off, in which case the panel shows nothing and the
// label is unreadable by design.
[[nodiscard]] constexpr bool is_off(const std::size_t index) {
    return kSteps[index].light_percent == 0;
}

// Whether reaching a step involves a ramp, and therefore owes a settled row once it lands.
[[nodiscard]] constexpr bool fades_in(const std::size_t index) {
    return kSteps[index].fade_ms > 0;
}

[[nodiscard]] constexpr const char* phase_label(const Phase phase) {
    switch (phase) {
    case Phase::Ceiling:
        return "ceiling";

    case Phase::Dim:
        return "dim";

    case Phase::Fade:
        return "fade";
    }

    return "unknown";
}

// The table's shape, asserted so a dwell cannot be added without the addition being argued.
static_assert(kStepCount == 13, "C1-C4, D1-D4, F1, F2, F3, OFF, F4");
static_assert(next_index(kStepCount - 1) == kFirstStep, "the sequence wraps to C1");
static_assert(is_off(kIndexOff), "the OFF step must be true off");
static_assert(!is_off(kIndexF4), "F4 must be lit");
static_assert(fades_in(kIndexF2) && fades_in(kIndexOff),
              "the two recession transitions carry the production fade");
static_assert(!fades_in(kIndexF1) && !fades_in(kIndexF3) && !fades_in(kIndexF4),
              "a wake and a label change land at once; only a recession fades");
static_assert(kSteps[kIndexF3].light_percent == kSteps[kIndexF2].light_percent,
              "F3 is a label change at F2's brightness, not a level of its own");

} // namespace prd::validation::brightness_harness
