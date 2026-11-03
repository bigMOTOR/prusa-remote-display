// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>

#include "scenes/operational_energy.hpp"
#include "theme/layout.hpp"
#include "ui/ui.hpp"

// The operational form — UI Design Decision 004 §3.
//
// INTERNAL to the ui component. LVGL-free, so the whole derivation is host-testable — which is
// the point. This is the only non-trivial arithmetic in Phase E, and this repository has
// already paid for geometry that was wrong in a way no build check could see: the first Phase C
// ring drew nothing at all and passed a clean build, 350 host tests, clang-tidy, the token
// guard and the flush cross-check. Arithmetic that can be tested on the host is tested there.
//
// §3.1: "There shall be one operational form. Its shape is energy: at maximum energy it is
// closed and dominant, describing a complete cycle of work; as energy falls it opens, flattens
// and sinks; at minimum energy it is a shallow resting curve low in the frame. Every
// intermediate shape is reachable and legitimate. A form that exists only at its extremes is
// two forms."
//
// This file is what makes "every intermediate shape is reachable" true. Nothing here chooses a
// shape for a state; a state has an energy (operational_energy.hpp) and an energy has a shape.
//
// EVERY INPUT IS AN EXISTING layout.hpp CONSTANT, referenced by name, so the derivation cannot
// drift away from the accepted geometry.
//
// ⛔ THE PANEL CONTROL IS NOW PRINTING ALONE. This used to read "Printing and Ready render as
// they do today… any change seen on those two states is a defect". That is no longer true of
// Ready: the owner's 2026-08-29 ruling against the F1 reference moved the settled horizon from
// the ring's column and width to the canvas's, so a visible change there is the correction
// landing. Printing, and the four states between it and Ready, are unchanged and remain the
// controls.

namespace prd::ui::scene {

// The existing resting interval, evaluated before integer pixel/degree quantisation. The
// stroke anchor is the affine continuation of the two endpoint anchors, avoiding the old
// inward-stroke side flip while retaining the fixed endpoint frames.
struct RestingStroke {
    double centre_x{0.0};
    double midpoint_y{0.0};
    double path_radius{0.0};
    double half_chord{0.0};
    int curvature_sign{0};
};

[[nodiscard]] RestingStroke resting_stroke(Energy energy);

// The form at one energy, in the terms LVGL is actually given.
//
// `extent` and `rotation` are here rather than being computed at the draw site ON PURPOSE. The
// arc-angle collapse defect is a property of the ARGUMENTS — lv_arc_set_bg_end_angle() contains
// a single `if (end > 360) end -= 360;`, so an end angle of 361 becomes 1 and an end angle of
// 630 becomes 270, either of which can silently produce a zero-extent arc that draws nothing.
// Keeping the arguments in the pure layer means the guard against that class of defect is a
// host test over the whole energy range rather than a comment somebody has to remember.
struct FormGeometry {
    // The circle the form is drawn on.
    std::int32_t radius{0};
    std::int32_t centre_y{0};

    // ⛔ THE FORM CARRIES ITS OWN HORIZONTAL CENTRE. Every consumer used to assume
    // theme::kRingCentreX, which was true only while one width served the whole axis. The
    // settled horizon is centred on the canvas and the operational states are centred beside
    // the telemetry column, so a global assumed centre would now be wrong for one end of the
    // axis whichever value it held. Asking the form is the only answer that stays true.
    std::int32_t centre_x{0};

    // The half-width the sagitta relation was solved for. NOT what the arc reaches and NOT what
    // it paints: half_extent() gives the nominal path after the whole-degree flooring, and
    // paint_left_offset()/paint_right_offset() give the renderer's own column bounds once the
    // inward stroke and rounded caps are taken into account. All three differ, and it is the
    // last pair that §7 constrains.
    std::int32_t half_width{0};

    // Degrees either side of the arc's own midpoint. 12 at minimum energy, 180 at maximum.
    std::int32_t half_angle{0};

    // The signed depth of the form's bowl — the axis's single parameter.
    //
    // Positive: the circle's centre is ABOVE the drawn arc, so the arc hugs the bottom of the
    // frame and its ends rise either side. Negative: the centre is below, and the arc is the
    // crown of UI Design Decision 002's resting expression. Zero is never produced; see the
    // note on the flat crossing in the derivation below.
    std::int32_t sagitta{0};

    // The angular span, in the form lv_arc_set_bg_angles() is given: (0, extent). Never
    // exceeds 360 and never reaches 0 — see the note above.
    std::int32_t extent{0};

    // Where that span is placed, via lv_arc_set_rotation(). Always in [0, 360).
    std::int32_t rotation{0};

    // Whether the form is a complete cycle. §3.1's maximum-energy shape, and — under §3.2 — the
    // only shape that can host the job's record as a sweep, because the record is a proportion
    // of a cycle and only a cycle can carry one faithfully. Consumed in E.3; carried here
    // because it is a property of the shape rather than of the state.
    bool closed{false};
};

// --- The derivation: the cycle opens at its top seam ------------------------
//
// THE PROBLEM THIS SOLVES, because it is the reason the law reads as it does.
//
// The two accepted endpoints put the composition's text in opposite relations to the form. At
// maximum energy the text sits INSIDE a closed ring (apex y = 24, bottom y = 216). At minimum
// energy it sits ABOVE the resting crown (apex y = 196). region.operational occupies y 86..176,
// between the two.
//
// An earlier law kept the arc centred on the circle's apex and let that apex descend from 24 to
// 196. That is continuous and monotone, and it is wrong: a continuous apex travelling 24 -> 196
// must take every value in 86..176, so the form is drawn straight through the words at
// intermediate energies. It is a theorem about the endpoints, not a tuning artefact — no
// spacing of the six magnitudes avoids it, and §11's motion would drag the form through the
// text at every transition even if the six sampled states happened to miss it.
//
// THE CORRECTION. The gap opens at the form's TOP — at twelve o'clock, the seam where the
// record's own cycle begins and ends — and the drawn arc is what remains, hugging the bottom of
// the frame. The quantity that used to sweep the text band is now pinned: the arc's
// centre-column crossing travels 216 -> 196 across the entire axis and never enters 86..176.
// The arc's ends do rise up the sides as the gap widens, but they rise OUTSIDE the width any
// row of text can occupy, because the composition's ink is centred and the gap is centred too.
//
// THE SINGLE PARAMETER IS A SIGNED SAGITTA — the depth of the form's bowl, positive when the
// circle's centre is above the arc and negative when below:
//
//     half_width w = ANCHORED, not constant — see the horizontal axis below. It is
//                    kRingDiameter / 2 at and above kEnergyStopped, and travels to
//                    kHorizonHalfWidth below it. An earlier law fixed it at kRingDiameter / 2
//                    across the whole axis, which is what made the settled horizon the ring's
//                    width instead of the canvas's.
//     sagitta    s = lerp(-kHorizonSag  ->  kRingDiameter)      linear in energy
//     midpoint   m = lerp(kHorizonApexY ->  ring bottom)        the centre-column crossing
//
//     |s| <= w :  radius   = (w^2 + s^2) / 2|s|                 the sagitta relation
//                 half     = asin(w / radius)
//     |s| >= w :  radius   = w                                  past the widest point, the
//                 half     = acos(1 - |s| / w)                  half width pins the radius
//
//     centre_y   = m - radius   when s > 0   (centre above: the arc is the circle's bottom)
//                = m + radius   when s < 0   (centre below: the accepted resting crown)
//
// At s = +192 this is the accepted ring exactly: radius 96, centre 120, a full turn from twelve
// o'clock. At s = -16, with the resting half width, it is the accepted crown: radius 693, centre
// 889, 12 degrees either side of 258. Neither endpoint is recomputed — both fall out of the
// relations.
//
// THE FLAT CROSSING. s passes through zero, where the radius would be infinite. |s| is clamped
// to one pixel, which is the smallest bow that can be drawn as anything but a straight line, so
// the form passes through a straight horizon rather than a singularity. The curvature sign
// changes there, continuously: the sagitta moves by at most one pixel per energy step and the
// drawn ends move with it.
//
// A KNOWN LIMIT OF THAT CROSSING. LVGL takes arc angles in whole degrees, and near the flat the
// form's width subtends only a couple of degrees, so flooring costs width: the form
// narrows to about 160 px at its flattest. The band is narrow and no state currently drawn falls
// inside it, but §11's motion traverses it, so it is a Phase I quality item rather than a
// silent approximation.
//
// WHAT IS NOT DERIVED HERE, and is a Phase H design act under §15: the trajectory itself. Both
// interpolations are linear because that asserts nothing beyond "monotone" — the same stance
// the energy magnitudes take. §15 reserves "the exact geometry of the form at any energy" for
// the panel.

// The form at an energy. Energies outside the scale are clamped to it.
[[nodiscard]] FormGeometry form_geometry(Energy energy);

// The arc's own midpoint — where it crosses the composition's centre column.
//
// THE INVARIANT THE CORRECTION RESTS ON. It stays between the resting crown's apex and the
// ring's bottom for every energy, so it is always below region.operational and the form can
// never be drawn through the centre of the text.
[[nodiscard]] std::int32_t midpoint_y(const FormGeometry& form);

// The y of the arc's two ends. Above the midpoint while the centre is above the arc, below it
// once the form has become the resting crown.
[[nodiscard]] std::int32_t end_y(const FormGeometry& form);

// The square the arc is placed in — the circle's bounding box, centred on THE FORM'S OWN x.
//
// It used to be centred on the ring's fixed x, which was true only while one centre served the
// whole axis. The settled horizon sits on the canvas centre, so a fixed x would place its box
// 32 px off and the widget would draw the arc somewhere the geometry never described.
//
// At low energies this extends far off-canvas, which LVGL clips (§11 Clipping). That is the
// honest consequence of a 693 px radius on a 240 px display and is what makes the crown
// shallow; what must stay inside the content box is the PAINTED arc, which
// paint_left_offset()/paint_right_offset() bound and the host tests check.
[[nodiscard]] theme::Rect bounding_box(const FormGeometry& form);

// Whether the form's stroke passes through a rectangle.
//
// DIAGNOSTIC-GRADE, and deliberately so. It samples the arc at roughly one point per pixel of
// arc length rather than solving the circle/rectangle intersection analytically: it exists to
// answer "does the form cross this row of text on the panel right now", which is a question
// about a composition rather than a rendering decision. Nothing in the product path branches
// on it.
//
// Proximity is measured as the true distance from a sampled point to the rectangle. Inflating
// the box on each axis separately would treat the stroke's reach as a square and report a
// diagonal near-miss at a corner as a collision — the same error that made the corrective
// checkpoint's first envelope model call two clear states struck.
//
// It lives here, beside the geometry, because both clearance gauges need the same answer and
// this project has already been burned by two gauges that measured different things — a
// box-based telemetry gauge once reported a collision that did not exist, and a lying
// diagnostic is worse than none.
[[nodiscard]] bool crosses(const FormGeometry& form, std::int32_t stroke, const theme::Rect& box);

// The outermost painted column either side of the form's own centre, as OFFSETS from it, for the
// pinned LVGL 9.5.0 renderer at the given stroke. Left is negative, right positive.
//
// ⛔ ASYMMETRIC, AND THAT IS NOT A ROUNDING ARTEFACT. LVGL's get_rounded_area() applies a
// half-pixel correction whose direction depends on the sign of the cap's centre, and an even
// stroke adds `thick_corr = 1` to the near side only. The two ends therefore land on integer
// columns that are not mirror images, and a symmetric figure cannot describe them: an earlier
// symmetric approximation here was one pixel short of the renderer's true left edge at 319 of
// the 1001 reachable energies, while being described as the exact lit pixels.
//
// ⛔ FOUR DIFFERENT THINGS, AND ONLY THE LAST IS THE PANEL:
//
//   FormGeometry::half_width   the sagitta relation's input; never drawn
//   half_extent()              the nominal geometric reach: radius * sin(half_angle)
//   these two functions        the pinned renderer's own integer column bounds
//   what the panel shows       UNVERIFIED until a photograph is taken
//
// DERIVED WITH THE VENDOR'S OWN INTEGER SEMANTICS, not a generic centred-stroke model:
// lv_draw_arc.c draws the stroke INWARD (`rin = radius - w`), so `radius` is the outer edge;
// get_rounded_area() places each cap disc at (radius - w/2) on the end angle and rounds with
// `(cir_x -/+ 128) >> 8`. The sine is LVGL's fixed-point lv_trigo_sin, whose table is exactly
// round(sin(deg) * 32768) for all 91 entries — reproduced arithmetically rather than copied, and
// pinned against the vendor's own values by a host test.
//
// Where the drawn span crosses three or nine o'clock the arc contains the circle's own widest
// point, and the outer edge rather than a cap is the extreme on that side.
[[nodiscard]] std::int32_t paint_left_offset(const FormGeometry& form, std::int32_t stroke);
[[nodiscard]] std::int32_t paint_right_offset(const FormGeometry& form, std::int32_t stroke);

// How far the nominal arc PATH reaches either side of the centre line — the geometric
// centreline of the drawn band's outer edge, with no stroke or cap allowance.
//
// Bounded by the form's OWN half width at every energy — no longer one width across the axis,
// which is the property that stops it growing off a 320 px canvas as it flattens. The
// half-angle floors, so the error is one-sided: the path can fall short of that half width by
// up to one degree of chord, and can never exceed it.
[[nodiscard]] std::int32_t half_extent(const FormGeometry& form);

// The extremes of the drawn arc: its midpoint and its ends, whichever way round they are.
[[nodiscard]] std::int32_t drawn_top_y(const FormGeometry& form);
[[nodiscard]] std::int32_t drawn_bottom_y(const FormGeometry& form);

// How the form is painted — ONE RULE, stated once, so that no scene styles a form of its own.
//
// UI Design Decision 004 §5.1, OWNER RULING 2026-08-14, recorded verbatim:
//
//   "Where the form is closed it hosts the record (§3.2), and the accent is drawn over the
//    record's extent. A closed form with no record to host therefore draws the accent over zero
//    extent — track only. §5.1 is unaffected: the accent's value remains energy-derived and its
//    referent remains the operational form alone; only the extent over which it appears is
//    governed by the record, which §13.2 already lists as variable and §15 reserves as rendering
//    behaviour."
//
// ⛔ ONE CLAUSE OF THAT QUOTE IS SUPERSEDED, AND THE QUOTE IS LEFT INTACT BECAUSE IT IS A VERBATIM
// RECORD. "The accent's value remains energy-derived" was true on 2026-08-14 and is REJECTED as of
// the owner ruling of 2026-09-01: the accent is one colour, `color.accent`, at every energy. What
// the 2026-08-14 ruling decides — WHICH ELEMENT the accent is drawn over, and that a closed form
// with no record draws it over zero extent — is untouched and is what this enumeration implements.
//
// WHAT IT CORRECTS. A closed form with no record previously fell through to the OPEN-shape
// treatment and was painted entirely in the accent — the one case that produces a closed accent
// cycle, which on the panel is indistinguishable from a completed record. The 2026-08-12 blind
// test read it as "complete", and the owner read the same frame the same way on 2026-08-14.
// R2 is the standing ruling behind it: the form is not a completion indicator.
//
// PURE AND HOST-TESTED, for the reason the report below gives about transcriptions: a selection
// rule living inside an LVGL translation unit is invisible to every gate this project runs.
enum class FormTreatment : std::uint8_t {
    // Nothing is on the panel.
    None,

    // The form itself carries the accent — `color.accent`, the same at every energy. Every open
    // shape, which is every state but Printing, since only a cycle can host a record.
    FormAccent,

    // Silent track, with the accent drawn over the record's extent. A closed shape hosting a
    // record: Printing with a reading.
    RecordSweep,

    // Silent track and no accent at all — the accent over zero extent. A closed shape with no
    // record to host: Printing whose reading the machine never supplied, or whose reading
    // reduced confidence has decayed away.
    TrackOnly,
};

[[nodiscard]] FormTreatment form_treatment(bool drawn, bool closed, bool draws_record);

// The published account of a drawn form — see the contract at ui::OperationalFormReport, which
// this fills. DIAGNOSTIC AND READ-BACK ONLY: nothing in the product path may branch on it.
//
// PURE, AND HERE RATHER THAN AT THE PUBLISHING SITE ON PURPOSE. It is a fourteen-field
// transcription out of FormGeometry and the accessors above, and a transcription living inside
// an LVGL translation unit is invisible to every gate this project runs: it would compile, it
// would print entirely plausible numbers, and the first thing to notice a swapped pair would be
// a reviewer trusting a wrong figure on a photograph. Here it is checked field by field across
// the whole axis by the host tests, which is the only reason this report can be relied on.
//
// `energy`, `form` and `accent` are all what the drawing component retained when it last placed
// the form. They are passed together rather than being re-derived from the energy: a report that
// recomputed the shape or the colour would agree with the derivation instead of with the panel,
// which is the one thing it exists not to do. The accent matters most here — a wrong number is
// obvious against a photograph and a wrong colour is not.
[[nodiscard]] OperationalFormReport form_report(std::optional<Energy> energy,
                                                const std::optional<FormGeometry>& form,
                                                std::uint32_t accent, bool draws_record,
                                                std::uint8_t cycle_treatment_alpha = 0);

} // namespace prd::ui::scene
