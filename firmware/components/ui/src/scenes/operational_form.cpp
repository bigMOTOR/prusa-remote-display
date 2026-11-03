// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/operational_form.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace prd::ui::scene {
namespace {

// --- The axis endpoints, every one an existing layout.hpp constant ----------
//
// Named rather than transcribed, so the derivation cannot drift away from the accepted
// geometry it is anchored to: a change to the ring or the horizon in layout.hpp moves this
// function's endpoints with it, and the endpoint-identity tests keep checking the same claim.

// --- The horizontal axis, anchored so only the resting segment moves --------
//
// ⛔ THE WIDTH AND CENTRE ARE CONSTANT ABOVE THE ANCHOR AND VARY ONLY BELOW IT. Settled Ready is
// centred on the canvas at its full content width; every operational state keeps the ring's own
// centre and width, because those states draw a telemetry column the form has to sit beside.
// Interpolating across the whole axis would have moved all six, and Stopped, Preparing,
// Completed, Paused and Printing are accepted geometry that this checkpoint must not touch.
//
// THE ANCHOR IS kEnergyStopped, the lowest energy that still draws an operational composition.
// At and above it both quantities are exactly the values the axis has always used, so the five
// preserved states are bit-identical; below it they travel to the resting endpoint. The join is
// continuous because both segments meet at the anchor's value rather than near it.
constexpr std::int32_t kHalfWidthOperational = theme::kRingDiameter / 2;
constexpr std::int32_t kHalfWidthAtRest = theme::kHorizonHalfWidth;
constexpr std::int32_t kCentreXOperational = theme::kRingCentreX;
constexpr std::int32_t kCentreXAtRest = theme::kHorizonCentreX;
constexpr Energy kHorizontalAnchor = kEnergyStopped;

static_assert(kHorizontalAnchor > kEnergyMin,
              "the resting segment needs a non-zero span to interpolate across");
static_assert(kHalfWidthAtRest > kHalfWidthOperational,
              "the settled horizon is wider than the ring, which is the whole correction");

// The bowl's signed depth at each end of the axis.
//
// NEGATIVE at rest: the accepted crown bows upward, so its circle's centre is BELOW the drawn
// arc. POSITIVE at full energy: the closed ring's centre is above its own lowest point, a full
// diameter away. The sign change between them is where the form passes through a straight
// horizon, and it is what keeps the arc out of the text — see the derivation in the header.
constexpr std::int32_t kSagAtRest = -theme::kHorizonSag;
constexpr std::int32_t kSagAtFullEnergy = theme::kRingDiameter;

// Where the arc crosses the composition's centre column: the resting crown's apex, and the
// ring's own bottom. Twenty pixels apart across the whole axis, and both below
// region.operational — which is the invariant the correction rests on.
constexpr std::int32_t kMidpointAtRest = theme::kHorizonApexY;
constexpr std::int32_t kMidpointAtFullEnergy = theme::kRingCentreY + (theme::kRingDiameter / 2);

// The smallest bow that can be drawn as anything but a straight line. The sagitta is held away
// from zero by this much so the radius stays finite as the form passes through flat; one pixel
// is a derived floor, not a chosen value.
constexpr std::int32_t kFlattestBow = 1;

// The smallest angle LVGL can be given in whole degrees. Typed rather than written as a bare
// literal at the use site: int32_t is `long` on the xtensa target and `int` on the host, so an
// untyped 1 makes std::max ambiguous in the firmware build while compiling cleanly on the host.
constexpr std::int32_t kFlattestAngle = 1;

// Progress zero sits at twelve o'clock — Production UI §6's concentric construction as UI
// Design Decision 001's artwork draws it. Same constant the ring has always used.
constexpr std::int32_t kFullTurn = 360;
constexpr std::int32_t kHalfTurn = kFullTurn / 2;
constexpr std::int32_t kQuarterTurn = kFullTurn / 4;
constexpr std::int32_t kApexAngle = kFullTurn - kQuarterTurn;
constexpr std::int32_t kBaseAngle = kQuarterTurn;

constexpr double kPi = 3.14159265358979323846;

// LVGL's fixed-point scale for lv_trigo_sin(): values are the sine times 2^15.
constexpr std::int32_t kTrigoShift = 15;

static_assert(kSagAtRest < 0 && kSagAtFullEnergy > 0,
              "the sagitta changes sign across the axis; that sign change is the form passing "
              "through a straight horizon, and it is what keeps the arc clear of the text");
static_assert(kMidpointAtRest < kMidpointAtFullEnergy, "the centre-column crossing rises slightly");
static_assert(kMidpointAtRest > theme::kRegionOperational.bottom(),
              "the arc's centre-column crossing must stay below the operational text at every "
              "energy; if this fails the form is being drawn through the composition");
static_assert(kApexAngle >= 0 && kApexAngle < kFullTurn,
              "lv_arc_set_rotation() normalises into [0,360); keep the constant in range so the "
              "value in the source is the value in effect");

[[nodiscard]] std::int32_t round_to_int(const double value) {
    return static_cast<std::int32_t>(std::lround(value));
}

// Downward angle quantisation, retained by the resting arc and deep-arc branches.
//
// ⛔ ROUNDING TO NEAREST OVERSHOOTS THE WIDTH, AND THE WIDTH IS A CONTRACT. The drawn chord is
// radius * sin(half_angle), so half a degree of upward rounding buys real pixels: in the
// resting segment, where the bow is shallow and the radius therefore enormous, energies 8 and 9
// reached x 6..314 — outside kCanvasContent and against Production UI §7's "no component may
// touch the display edge". Flooring makes the drawn chord provably <= the half width the
// sagitta relation was solved for, because sine is increasing over the quarter turn.
//
// It costs nothing at any accepted endpoint: every one of the six sampled energies floors to
// the same whole degree it used to round to.
[[nodiscard]] std::int32_t degrees_floor_of(const double radians) {
    return static_cast<std::int32_t>(std::floor(radians * kHalfTurn / kPi));
}

[[nodiscard]] double radians_of(const std::int32_t degrees) {
    return degrees * kPi / kHalfTurn;
}

} // namespace

FormGeometry form_geometry(const Energy energy) {
    const Energy clamped = std::clamp(energy, kEnergyMin, kEnergyMax);

    // Linear in energy, in integer arithmetic so the result is identical on the host and on the
    // target and cannot drift with a library's rounding.
    //
    // A lambda over the resolved energy rather than a three-argument helper: the endpoints are
    // named after the axis they sit on, so a signature carrying the energy alongside them reads
    // as three interchangeable numbers — which is exactly what clang-tidy's
    // readability-suspicious-call-argument said about the earlier version. Binding the energy
    // once removes the ambiguity instead of suppressing the report.
    const auto between = [clamped](const std::int32_t at_rest, const std::int32_t at_full) {
        return at_rest + (((at_full - at_rest) * clamped) / kEnergyMax);
    };

    FormGeometry form;

    // Below the anchor the horizontal axis travels; at and above it, it is pinned. Written as an
    // explicit branch rather than a clamped lerp so the pinned case is visibly exact.
    if (clamped >= kHorizontalAnchor) {
        form.half_width = kHalfWidthOperational;
        form.centre_x = kCentreXOperational;
    } else {
        const auto toward_anchor = [clamped](const std::int32_t at_rest,
                                             const std::int32_t at_anchor) {
            return at_rest + (((at_anchor - at_rest) * clamped) / kHorizontalAnchor);
        };

        form.half_width = toward_anchor(kHalfWidthAtRest, kHalfWidthOperational);
        form.centre_x = toward_anchor(kCentreXAtRest, kCentreXOperational);
    }

    form.sagitta = between(kSagAtRest, kSagAtFullEnergy);

    // The centre-column crossing. Interpolated on its own, because it is the quantity the whole
    // correction exists to pin: it never enters the band region.operational occupies, so the
    // form is never drawn through the middle of the composition.
    const std::int32_t midpoint = between(kMidpointAtRest, kMidpointAtFullEnergy);

    // Held off zero so the radius stays finite as the form passes through flat. The sign is
    // taken before the clamp, so the crossing keeps its direction.
    const bool centre_above = form.sagitta > 0;
    const std::int32_t bow = std::max(std::abs(form.sagitta), kFlattestBow);

    if (bow <= form.half_width) {
        // A shallow arc, no deeper than a semicircle. The sagitta relation gives the circle
        // that passes through the arc's two ends and its midpoint, which is exactly how
        // layout.hpp derived the accepted horizon — and with the resting half width it
        // reproduces radius 693 from a bow of 16.
        const std::int32_t numerator = (form.half_width * form.half_width) + (bow * bow);
        form.radius = round_to_int(static_cast<double>(numerator) / (2.0 * bow));

    } else {
        // Past the widest point. The arc now includes the circle's own left and right extremes,
        // so its horizontal reach IS the radius — which pins the radius at the half width for
        // the rest of the axis.
        form.radius = form.half_width;
    }

#ifdef PRD_OLD_UPPER_ANGLE
    constexpr bool continuous_angle = false;
#else
    const bool continuous_angle = clamped >= kHorizontalAnchor;
#endif
    if (continuous_angle) {
        // The shallow branch evaluates the pixel bow directly instead of inverting a
        // rounded radius near asin(1). Nearest-degree output retains the accepted shallow
        // endpoint spans. The deep branch keeps its accepted floor quantiser but must
        // evaluate the untruncated bow: acos is singular at the closed-ring endpoint.
        const double angular_bow =
            kSagAtRest +
            (static_cast<double>(kSagAtFullEnergy - kSagAtRest) * clamped / kEnergyMax);
        const auto width = static_cast<double>(form.half_width);
        form.half_angle =
            angular_bow <= width
                ? round_to_int(2.0 * std::atan2(static_cast<double>(bow), width) * kHalfTurn / kPi)
                : degrees_floor_of(std::acos(1.0 - (angular_bow / width)));
    } else if (bow <= form.half_width) {
        form.half_angle =
            degrees_floor_of(std::asin(static_cast<double>(form.half_width) / form.radius));
    } else {
        form.half_angle =
            degrees_floor_of(std::acos(1.0 - (static_cast<double>(bow) / form.half_width)));
    }

    form.half_angle = std::max(form.half_angle, kFlattestAngle);

    // ⛔ AND THE RADIUS IS CAPPED SO THAT ANGLE CANNOT OVERSHOOT THE WIDTH.
    //
    // At the flat crossing the bow is held at one pixel, so the radius explodes — and a radius
    // large enough makes even the minimum one-degree half-angle subtend a chord far wider than
    // the width the relation was solved for. With the resting segment's wider forms this put
    // energies 73..75 at x -1, off the canvas entirely. The floor above cannot prevent it,
    // because the overshoot comes from the angle being raised to the minimum rather than from
    // rounding.
    //
    // ⛔ AND IT BOUNDS THE PAINTED ENVELOPE, NOT THE NOMINAL CHORD. Capping the chord alone was
    // not enough: the rounded caps sit outside the path, so a form whose chord was exactly the
    // half width still painted about four pixels further either side, and energies 13..14 reached
    // x 11 — one pixel outside kCanvasContent. The bound is stated on the quantity §7 actually
    // constrains, which is ink:
    //
    //     floor((radius - w/2) * sin(half_angle)) + w/2  <=  half_width
    //
    // It binds only inside the flat band; at every accepted endpoint the sagitta relation's own
    // radius is already below it, so no accepted geometry moves.
    // ⛔ ONLY INSIDE THE QUARTER TURN, where the drawn chord IS radius * sin(half_angle). Past
    // it the arc contains the circle's own widest points and half_extent() returns the radius
    // itself, which the acos branch has already set to the half width — so there is nothing to
    // cap, and sin(180 degrees) is a whole-number zero that would divide the radius to nonsense.
    //
    // ⬅️ AND ONLY BELOW THE ANCHOR. Above it the five accepted states are narrow forms on the
    // ring's column — the widest paints to x 225, less than a hundred pixels inside the box — so
    // the cap has nothing to protect there, while applying it would pull Stopped's radius from
    // 197 to 194 and move accepted geometry. It guards the segment that needs guarding.
    if (clamped < kHorizontalAnchor && form.half_angle < kQuarterTurn) {
        const double span_of_angle = std::sin(radians_of(form.half_angle));
        const std::int32_t thick_half = theme::kRingStroke / 2;
        const std::int32_t widest =
            round_to_int((form.half_width - thick_half) / span_of_angle) + thick_half;

        form.radius = std::min(form.radius, widest);
    }

    // Which side of the arc the circle's centre sits on. Above it while the form is the bottom
    // of a circle — the whole of the axis except its resting end — and below it once the form
    // has become the accepted crown.
    form.centre_y = centre_above ? midpoint - form.radius : midpoint + form.radius;

    // THE ARC-ANGLE COLLAPSE GUARD, in the one place the arguments are produced.
    //
    // lv_arc_set_bg_end_angle() contains a single `if (end > 360) end -= 360;` — a subtraction,
    // not a normalising loop. Passing (270, 270 + 360) therefore sets start equal to end and
    // yields a zero-extent arc that draws NOTHING: no track and no indicator. The first Phase C
    // build did exactly that and every automated check passed. So the span is always expressed
    // as (0, extent) with extent in range, and the placement is done with the rotation, which
    // LVGL normalises properly.
    form.extent = std::min(2 * form.half_angle, kFullTurn);
    form.closed = form.extent >= kFullTurn;

    // WHERE THE SPAN SITS, AND THEREFORE WHERE THE GAP SITS.
    //
    // A closed form has no visible seam, so where its span begins is free — and §6 with UIDD
    // 001's artwork fix it at twelve o'clock, because that is where the record's sweep starts.
    //
    // An open form is centred on its own midpoint, which puts the GAP opposite. While the
    // circle's centre is above the arc that midpoint is the circle's base, so the gap opens at
    // twelve o'clock: the cycle opens at its own seam, and the arc stays low in the frame
    // instead of travelling down through the text. At the resting end the centre has moved
    // below and the arc is the accepted crown about its circle's apex.
    if (form.closed) {
        form.rotation = kApexAngle;
    } else {
        const std::int32_t centred_on = centre_above ? kBaseAngle : kApexAngle;
        form.rotation = ((centred_on - form.half_angle) + kFullTurn) % kFullTurn;
    }

    return form;
}

RestingStroke resting_stroke(const Energy energy) {
    constexpr std::int32_t stroke = theme::kRingStroke;
    const auto clamped = static_cast<double>(std::clamp(energy, kEnergyMin, kHorizontalAnchor));
    const double position = clamped / static_cast<double>(kHorizontalAnchor);
    const double width = kHalfWidthAtRest + ((kHalfWidthOperational - kHalfWidthAtRest) * position);
    const double sagitta = kSagAtRest + ((kSagAtFullEnergy - kSagAtRest) * clamped / kEnergyMax);
    const double midpoint =
        kMidpointAtRest + ((kMidpointAtFullEnergy - kMidpointAtRest) * clamped / kEnergyMax);
    const double half_stroke = stroke / 2.0;

    RestingStroke result;
    result.centre_x = kCentreXAtRest + ((kCentreXOperational - kCentreXAtRest) * position);

    // The renderer's stroke anchor changes: lv_arc's inward stroke places
    // the centreline at midpoint +/- half_stroke. Its side flips at sagitta zero, which
    // moves the visible body by a full stroke (8 px). The unique affine continuation
    // between the two existing endpoint anchors removes that discontinuity without an
    // extra time phase or any origin-specific value. The exact endpoints still use lv_arc.
    result.midpoint_y = midpoint + half_stroke - (stroke * position);
#ifdef PRD_OLD_STROKE_ANCHOR
    // Firing control: restore the discontinuity while retaining all precision fixes.
    result.midpoint_y = midpoint + (sagitta > 0.0 ? -half_stroke : half_stroke);
#endif
    result.curvature_sign = static_cast<int>(sagitta > 0.0) - static_cast<int>(sagitta < 0.0);
    if (result.curvature_sign == 0) {
        // A zero-curvature line: no enormous circle is constructed or allocated.
        result.half_chord = width;
        return result;
    }

    // The existing sagitta relation, without the artificial one-pixel bow or the radius
    // cap required solely by lv_arc's minimum whole-degree span.
    const double radius = ((width * width) + (sagitta * sagitta)) / (2.0 * std::abs(sagitta));
    result.path_radius = radius - half_stroke;
    result.half_chord = result.path_radius * width / radius;
    return result;
}

std::int32_t half_extent(const FormGeometry& form) {
    if (form.half_angle >= kQuarterTurn) {
        // Past a quarter turn from the midpoint the arc contains the circle's widest points, so
        // its horizontal reach is the radius itself rather than a chord of it.
        return form.radius;
    }

    return round_to_int(form.radius * std::sin(radians_of(form.half_angle)));
}

namespace {

// LVGL's fixed-point sine, reproduced rather than linked: the widget layer is not available to
// the pure geometry, and lv_trigo_sin()'s table is exactly round(sin(deg) * 32768) at every one
// of its 91 entries — checked against the vendor's own values by a host test rather than
// asserted here. Whole degrees only, which is all this file ever asks for.
[[nodiscard]] std::int32_t trigo_sin(std::int32_t degrees) {
    degrees = ((degrees % kFullTurn) + kFullTurn) % kFullTurn;

    // Folded into the first quadrant the way the vendor's table lookup is, as a chain rather
    // than nested conditionals: the four cases are a partition of the turn, and reading them
    // downwards is how the table is indexed.
    std::int32_t folded = degrees;
    if (degrees >= 270) {
        folded = kFullTurn - degrees;
    } else if (degrees >= 180) {
        folded = degrees - 180;
    } else if (degrees >= 90) {
        folded = 180 - degrees;
    }
    const std::int32_t magnitude = round_to_int(std::sin(radians_of(folded)) * 32768.0);
    const std::int32_t signed_value = degrees < 180 ? magnitude : -magnitude;

    // The vendor's own saturation of the quarter-turn extremes.
    if (signed_value == 32767) {
        return 32768;
    }

    if (signed_value == -32767) {
        return -32768;
    }

    return signed_value;
}

[[nodiscard]] std::int32_t trigo_cos(const std::int32_t degrees) {
    return trigo_sin(degrees + kQuarterTurn);
}

// get_rounded_area()'s x1 and x2 for one cap, as offsets from the arc's centre. Transcribed from
// the pinned lv_draw_sw_arc.c, including the sign-dependent half-pixel correction and the
// even-stroke `thick_corr` that makes the two ends land on columns that are not mirror images.
struct CapColumns {
    std::int32_t left{0};
    std::int32_t right{0};
};

// The radius and the stroke travel together because they are one thing to the renderer — the
// stroke is drawn INWARD from the radius, so neither is meaningful here without the other. Named
// as a pair rather than passed as two bare widths, which is also two adjacent arguments of the
// same type that a caller could silently transpose.
struct StrokedArc {
    std::int32_t radius{0};
    std::int32_t stroke{0};
};

[[nodiscard]] CapColumns cap_columns(const std::int32_t angle, const StrokedArc& arc) {
    const std::int32_t thick_half = arc.stroke / 2;
    const std::int32_t thick_corr = (arc.stroke % 2) != 0 ? 0 : 1;

    std::int32_t centre = ((arc.radius - thick_half) * trigo_cos(angle)) >> (kTrigoShift - 8);

    if (centre > 0) {
        centre = (centre - 128) >> 8;
        return {centre - thick_half + thick_corr, centre + thick_half};
    }

    centre = (centre + 128) >> 8;
    return {centre - thick_half, centre + thick_half - thick_corr};
}

// Whether the drawn span passes through the given whole-degree bearing, so the arc contains the
// circle's own extreme there and the outer edge — not a cap — is what reaches furthest.
[[nodiscard]] bool span_covers(const FormGeometry& form, const std::int32_t bearing) {
    if (form.closed) {
        return true;
    }

    for (std::int32_t step = 0; step <= form.extent; ++step) {
        if (((form.rotation + step) % kFullTurn) == bearing) {
            return true;
        }
    }

    return false;
}

} // namespace

std::int32_t paint_left_offset(const FormGeometry& form, const std::int32_t stroke) {
    if (span_covers(form, kHalfTurn)) {
        return -form.radius;
    }

    const StrokedArc arc{form.radius, stroke};
    const CapColumns start = cap_columns(form.rotation, arc);
    const CapColumns end = cap_columns(form.rotation + form.extent, arc);

    return std::min(start.left, end.left);
}

std::int32_t paint_right_offset(const FormGeometry& form, const std::int32_t stroke) {
    if (span_covers(form, 0)) {
        return form.radius;
    }

    const StrokedArc arc{form.radius, stroke};
    const CapColumns start = cap_columns(form.rotation, arc);
    const CapColumns end = cap_columns(form.rotation + form.extent, arc);

    return std::max(start.right, end.right);
}

std::int32_t midpoint_y(const FormGeometry& form) {
    return form.sagitta > 0 ? form.centre_y + form.radius : form.centre_y - form.radius;
}

std::int32_t end_y(const FormGeometry& form) {
    const std::int32_t leg = round_to_int(form.radius * std::cos(radians_of(form.half_angle)));

    return form.sagitta > 0 ? form.centre_y + leg : form.centre_y - leg;
}

std::int32_t drawn_top_y(const FormGeometry& form) {
    return std::min(midpoint_y(form), end_y(form));
}

std::int32_t drawn_bottom_y(const FormGeometry& form) {
    return std::max(midpoint_y(form), end_y(form));
}

theme::Rect bounding_box(const FormGeometry& form) {
    return theme::Rect{form.centre_x - form.radius, form.centre_y - form.radius, 2 * form.radius,
                       2 * form.radius};
}

bool crosses(const FormGeometry& form, const std::int32_t stroke, const theme::Rect& box) {
    // Half the stroke either side of the nominal circle: what is drawn is a band, not a line,
    // and a row that clears the centre line by one pixel does not clear the ink.
    const double half_stroke = stroke / 2.0;

    // One sample per pixel of arc length, so the step is finer on a large radius where a fixed
    // angular step would leave gaps a text row could hide in.
    const double arc_length = radians_of(form.extent) * form.radius;
    const auto samples = static_cast<std::int32_t>(arc_length) + 1;

    for (std::int32_t index = 0; index <= samples; ++index) {
        const double angle = radians_of(form.rotation) +
                             (radians_of(form.extent) * index / static_cast<double>(samples));

        // LVGL's convention: zero at three o'clock, increasing clockwise, and screen y grows
        // downward — so a plain sine gives the right sign without a negation.
        const double point_x = form.centre_x + (form.radius * std::cos(angle));
        const double point_y = form.centre_y + (form.radius * std::sin(angle));

        // TRUE DISTANCE TO THE RECTANGLE, not an axis-by-axis inflation.
        //
        // Inflating x and y independently treats the stroke's reach as a square rather than a
        // disc, so a point sitting diagonally off a corner — further away than the half stroke,
        // but within it on each axis separately — reads as a hit. That over-reporting is not
        // theoretical: it is what the corrective checkpoint's first envelope model did, and it
        // flagged two states as struck when the arc was several pixels clear of them.
        //
        // Zero on both axes when the point is inside the box, so the expression covers
        // containment and proximity in one.
        const double gap_x = std::max({box.x - point_x, 0.0, point_x - box.right()});
        const double gap_y = std::max({box.y - point_y, 0.0, point_y - box.bottom()});

        if (std::sqrt((gap_x * gap_x) + (gap_y * gap_y)) < half_stroke) {
            return true;
        }
    }

    return false;
}

FormTreatment form_treatment(const bool drawn, const bool closed, const bool draws_record) {
    if (!drawn) {
        return FormTreatment::None;
    }

    // Only a cycle can host a record faithfully (§3.2), so an open shape never has one to draw
    // and the form itself carries the accent. `draws_record` is ignored here rather than
    // asserted against: the caller already ANDs it with `closed`, and a rule that disagreed with
    // its caller about an impossible combination would be a second rule.
    if (!closed) {
        return FormTreatment::FormAccent;
    }

    return draws_record ? FormTreatment::RecordSweep : FormTreatment::TrackOnly;
}

OperationalFormReport form_report(const std::optional<Energy> energy,
                                  const std::optional<FormGeometry>& form,
                                  const std::uint32_t accent, const bool draws_record,
                                  const std::uint8_t cycle_treatment_alpha) {
    OperationalFormReport report;

    // BOTH OR NEITHER. The drawing component sets the energy and the geometry in the same call
    // and clears them in the same call, so one without the other means they have drifted apart
    // — which is exactly the state a report must not paper over. Reported as "nothing drawn"
    // rather than as a half-filled record.
    //
    // `accent` and `draws_record` deliberately fall back to their defaults here too: a form that
    // is not on the panel was drawn in no colour, and a sweep cannot be carrying the record on
    // one.
    if (!energy.has_value() || !form.has_value()) {
        return report;
    }

    report.drawn = true;
    report.energy = *energy;

    report.radius = form->radius;
    report.centre_y = form->centre_y;
    report.half_angle = form->half_angle;
    report.extent = form->extent;
    report.rotation = form->rotation;
    report.sagitta = form->sagitta;

    report.centre_x = form->centre_x;
    report.half_width = form->half_width;

    report.midpoint_y = midpoint_y(*form);
    report.top_y = drawn_top_y(*form);
    report.bottom_y = drawn_bottom_y(*form);

    // ⛔ TAKEN FROM THE GEOMETRY, NEVER RECOMPUTED HERE. Both reaches and all four endpoints come
    // from the same functions the renderer's arguments were derived from, so a report cannot
    // disagree with what was drawn. A second derivation at the reading site is how a diagnostic
    // starts describing a form that is not on the panel.
    report.half_extent = half_extent(*form);

    report.nominal_left = form->centre_x - report.half_extent;
    report.nominal_right = form->centre_x + report.half_extent;

    // ⛔ ASYMMETRIC ON PURPOSE. The renderer's two ends do not land on mirror columns, so there
    // is no single painted half-extent to report and one would be a fiction.
    report.paint_left = form->centre_x + paint_left_offset(*form, theme::kRingStroke);
    report.paint_right = form->centre_x + paint_right_offset(*form, theme::kRingStroke);

    report.accent = accent;

    report.closed = form->closed;
    report.draws_record = draws_record;
    report.treatment = static_cast<std::uint8_t>(form_treatment(true, form->closed, draws_record));
    report.cycle_treatment_alpha = cycle_treatment_alpha;

    return report;
}

} // namespace prd::ui::scene
