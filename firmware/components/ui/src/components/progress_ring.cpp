// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "components/progress_ring.hpp"

namespace prd::ui::component {
namespace {

// The angular span is always given as (0, extent) with the placement done by the rotation.
//
// THIS IS NOT A STYLE CHOICE. lv_arc_set_bg_end_angle() contains `if(end > 360) end -= 360;`.
// A single subtraction, not a normalising loop. So passing (270, 270 + 360) to
// lv_arc_set_bg_angles() sets start = 270 and end = 630 - 360 = 270: an arc whose start equals
// its end, of zero extent, drawing nothing at all — no track and no indicator. The first
// Phase C build did exactly that, and it produced a composition with no ring on it that every
// build check, all the host tests, the token guard and the flush/transfer cross-check passed
// without complaint. It was found by looking at the panel.
//
// scenes/operational_form.hpp produces the arguments and its host tests assert, at every
// energy on the axis, that the end angle never exceeds a full turn and never reaches zero.
// That is where the guard lives now; this is where it is spent.
constexpr std::int32_t kSpanStart = 0;

// The value range the arc is set over. Fixed at the percentage scale rather than the
// job's own layer count, because §6 says the sweep is "proportional to print
// progress" and progress is what the printer reports as a percentage.
constexpr std::int32_t kRangeMinimum = 0;
constexpr std::int32_t kRangeMaximum = 100;
constexpr std::uint8_t kCycleTreatmentFull = 255;

constexpr std::uint32_t blend_channel(const std::uint32_t from, const std::uint32_t to,
                                      const std::uint32_t alpha) {
    return ((from * (255U - alpha)) + (to * alpha) + 127U) / 255U;
}

constexpr std::uint32_t blend_rgb(const std::uint32_t from, const std::uint32_t to,
                                  const std::uint32_t alpha) {
    return (blend_channel((from >> 16U) & 0xFFU, (to >> 16U) & 0xFFU, alpha) << 16U) |
           (blend_channel((from >> 8U) & 0xFFU, (to >> 8U) & 0xFFU, alpha) << 8U) |
           blend_channel(from & 0xFFU, to & 0xFFU, alpha);
}

} // namespace

bool ProgressRing::build(lv_obj_t* const parent) {
    arc_ = lv_arc_create(parent);
    if (arc_ == nullptr) {
        return false;
    }

    lv_obj_remove_style_all(arc_);

    // Not a control. The MVP has no touchscreen and an arc is LVGL's slider-like
    // widget by default, which would make it focusable and give it a knob part.
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);

    // LVGL draws a knob on the indicator's leading edge by default. §6 specifies a
    // stroke and a cap and no knob, and a knob would read as a control.
    lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);

    // §6's Progress Geometry, shared by every shape on the axis: stroke.emphasis-derived
    // width, round cap, `color.accent`. What set_form() changes is the circle these are drawn
    // on.
    lv_obj_set_style_arc_width(arc_, theme::kRingStroke, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_, theme::kRingStroke, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_INDICATOR);

    lv_arc_set_range(arc_, kRangeMinimum, kRangeMaximum);
    lv_arc_set_value(arc_, kRangeMinimum);

    segment_ = lv_line_create(parent);
    if (segment_ == nullptr) {
        return false;
    }
    lv_obj_remove_style_all(segment_);
    lv_obj_remove_flag(segment_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(segment_, LV_OBJ_FLAG_SCROLLABLE);
    theme::place(segment_, theme::kCanvas);
    lv_obj_set_style_line_width(segment_, theme::kRingStroke, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(segment_, true, LV_PART_MAIN);

    // Nothing is drawn until a presentation says which energy this is.
    set_form(std::nullopt, false);
    return true;
}

void ProgressRing::apply_geometry(const scene::FormGeometry& form) {
    theme::place(arc_, scene::bounding_box(form));

    // Concentric construction per §6: one background track carrying the shape, with the
    // indicator drawn over the same centre and radius. Two objects would be two primitives,
    // which §6's "concentric construction" rules out.
    lv_arc_set_bg_angles(arc_, kSpanStart, form.extent);
    lv_arc_set_rotation(arc_, form.rotation);
}

void ProgressRing::apply_record_treatment(const std::uint32_t accent) {
    // THE TRACK IS DELIBERATELY ALMOST SILENT, and this is a composition decision rather
    // than a contrast one.
    //
    // In the accepted UIDD 001 artwork the form reads as an ARC, not as a circle: the
    // unfilled portion is barely separable from the background, so the only thing the eye
    // sees as a stroke is the record itself. A legible track turns the same geometry into a
    // closed graphic object that weighs down the whole frame.
    //
    // It also buys the composition its width. At 320 px a 192 px form and a 96 px telemetry
    // column cannot coexist without overlapping, and the artwork accepts the overlap because
    // the track is invisible where it passes behind the column.
    lv_obj_set_style_arc_color(arc_, theme::colour(theme::Colour::Tertiary), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Low), LV_PART_MAIN);

    // The record's sweep is the drawn element on this shape, so it carries the accent — one
    // colour, `color.accent`, per §5.1. This shape occurs only at maximum energy, and the accent
    // is the token at every energy, so the closed ring renders argument-for-argument as it always
    // has and remains the control on the panel.
    lv_obj_set_style_arc_color(arc_, theme::colour_from_rgb(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Full), LV_PART_INDICATOR);
}

void ProgressRing::apply_cycle_transition_treatment(const std::uint32_t accent,
                                                    const std::uint8_t alpha,
                                                    const bool draws_record) {
    // The form endpoint is full accent; the cycle endpoint is a quiet track carrying only the
    // reported sweep. Interpolate those two EXISTING treatments on the form's own motion clock.
    // Without this, every open intermediate inherited FormAccent until the exact closed frame,
    // so Preparing→Printing and Paused→Printing visibly read as 100% before snapping to the
    // truthful value. The sweep value itself is never interpolated or allowed to exceed the
    // printer's reading — only its treatment is revealed.
    const auto track_opacity = static_cast<std::uint32_t>(theme::opacity(theme::Opacity::Low));
    const auto main_opacity = static_cast<lv_opa_t>(
        blend_channel(theme::opacity(theme::Opacity::Full), track_opacity, alpha));
    lv_obj_set_style_arc_color(
        arc_, theme::colour_from_rgb(blend_rgb(accent, theme::kColorTertiary, alpha)),
        LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, main_opacity, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_, theme::colour_from_rgb(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc_,
                             draws_record ? static_cast<lv_opa_t>(alpha)
                                          : theme::opacity(theme::Opacity::Transparent),
                             LV_PART_INDICATOR);
}

void ProgressRing::apply_absent_record_treatment() {
    // A CLOSED FORM WITH NO RECORD TO HOST — the §5.1 ruling of 2026-08-14. The track alone,
    // exactly as the record treatment draws it, and the accent over zero extent.
    //
    // WHY NOT THE FORM TREATMENT, which this case used until now. That treatment paints the
    // whole shape in the accent, and at maximum energy the whole shape is a closed cycle — so
    // the composition with the LEAST information carried the strongest possible accent
    // statement, and a closed accent cycle is precisely what a completed record looks like.
    // Both the blind 2 m test and the owner read it as "complete".
    //
    // WHY NOT A ZERO-EXTENT SWEEP. `apply_record_value()` never runs for this case, and the
    // indicator is silenced rather than set to zero for the reason recorded there: a zero-extent
    // arc with a round cap still paints a dot, and a dot at twelve o'clock reads as 0 % or as a
    // marker — a value where there is none.
    //
    // The tokens are the record treatment's own, deliberately: the track a reader already sees
    // as the unfilled remainder of a sweep is the same track that now means "no reading at all".
    lv_obj_set_style_arc_color(arc_, theme::colour(theme::Colour::Tertiary), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Low), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Transparent), LV_PART_INDICATOR);
}

void ProgressRing::apply_form_treatment(const std::uint32_t accent) {
    // No record on this shape, so the form itself is the drawn element and carries the accent —
    // `color.accent`, which §5.1 applies "to the operational form alone" and does not vary with
    // energy (owner ruling, 2026-09-01).
    //
    // FULL OPACITY AT EVERY ENERGY, and that is the rule rather than an oversight. §5.1 gives the
    // form one colour at full opacity; energy is expressed by the SHAPE this arc is placed on, and
    // by nothing else here. Reintroducing `opacity.low` at rest, or scaling these channels by the
    // energy, would each be a second attenuation on top of the backlight — which is precisely what
    // the 2026-09-01 ruling removes.
    //
    // ⬅️ REJECTED HISTORY, kept because it is the reasoning a future edit would otherwise repeat.
    // Two earlier mechanisms have been tried at this line and both are rejected: `color.accent` at
    // `opacity.low` for every open shape (one fixed step, so four of the six states were
    // indistinguishable in colour), and then an energy-scaled ramp down to 40 % of the accent
    // (#623212 at rest). Alpha was avoided by the second because a low-opacity accent over the dark
    // field desaturates toward a pale tan rather than reading as a darker orange; the owner then
    // rejected the RGB attenuation too. Neither is the rule. One token, full opacity, always.
    lv_obj_set_style_arc_color(arc_, theme::colour_from_rgb(accent), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);

    // The indicator has nothing to show on this shape. Made transparent rather than set to
    // zero extent, because a zero-extent arc with a round cap still paints a dot.
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Transparent), LV_PART_INDICATOR);
}

void ProgressRing::set_form(const std::optional<scene::Energy> energy, const bool draws_record,
                            const std::int32_t offset_y) {
    const std::uint8_t stable_alpha = energy.has_value() && scene::form_geometry(*energy).closed
                                          ? kCycleTreatmentFull
                                          : std::uint8_t{0};
    set_form(energy, draws_record, offset_y, stable_alpha);
}

void ProgressRing::set_form(const std::optional<scene::Energy> energy, const bool draws_record,
                            const std::int32_t offset_y, const std::uint8_t cycle_treatment_alpha) {
    if (arc_ == nullptr) {
        return;
    }

    if (!energy.has_value()) {
        energy_.reset();
        form_.reset();
        accent_ = 0;
        draws_record_ = false;
        cycle_treatment_alpha_ = 0;
        lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(segment_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    scene::FormGeometry form = scene::form_geometry(*energy);
    form.centre_y += offset_y;

    // Retained together with the shape it produced, and only ever assigned here. Everything the
    // firmware reports about the form comes from this trio, so they cannot describe different
    // things.
    //
    // THE ACCENT IS RETAINED RATHER THAN RECOMPUTED WHEN THE REPORT IS READ, for the reason
    // ui.hpp gives about the geometry: a figure derived a second time is a PREDICTION of what
    // the renderer should have drawn, printed in a line a reviewer reads as a measurement. This
    // is the value that was handed to LVGL, held for the report to state.
    energy_ = *energy;
    form_ = form;
    accent_ = scene::accent();

    // §3.2: a stable open form cannot host a record. During the renderer-owned journey to or
    // from the cycle, however, the actual supplied sweep is cross-faded with that form so the
    // almost-closed intermediate cannot read as a fabricated 100 %. This changes no value: the
    // indicator is still set only from `record_`, and every stable open endpoint still has zero
    // cycle-treatment alpha and therefore no sweep.
    //
    // "Closed" is a property of the shape, not a threshold anyone chose. Nothing here is tuned.
    draws_record_ = draws_record && cycle_treatment_alpha > 0;
    cycle_treatment_alpha_ = cycle_treatment_alpha;

    if (*energy > scene::kEnergyReady && *energy < scene::kEnergyStopped) {
        draws_record_ = false;
        cycle_treatment_alpha_ = 0;
        const auto count = scene::resting_line_points(*energy, offset_y, points_);
        lv_line_set_points(segment_, points_, count);
        lv_obj_set_style_line_color(segment_, theme::colour_from_rgb(accent_), LV_PART_MAIN);
        lv_obj_set_style_line_opa(segment_, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);
        lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(segment_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(segment_, LV_OBJ_FLAG_HIDDEN);
    apply_geometry(form);

    // ONE RULE, SELECTED ONCE, IN THE PURE LAYER. Nothing here decides how the form is painted;
    // it asks scene::form_treatment() and applies the answer, so a scene cannot acquire a
    // treatment of its own and the three drawn cases are provably the same rule.
    // The interpolated alpha can round to 255 one or two ticks before the geometry rounds to
    // its closed endpoint. Closure is therefore part of the endpoint predicate. Testing alpha
    // alone recreates the exact physical defect: one almost-closed FormAccent frame, then the
    // truthful sweep.
    if (cycle_treatment_alpha > 0 &&
        (cycle_treatment_alpha < kCycleTreatmentFull || !form.closed)) {
        apply_cycle_transition_treatment(accent_, cycle_treatment_alpha, draws_record);
    } else {
        switch (scene::form_treatment(true, form.closed, draws_record_)) {
        case scene::FormTreatment::RecordSweep:
            apply_record_treatment(accent_);
            break;

        case scene::FormTreatment::TrackOnly:
            apply_absent_record_treatment();
            break;

        case scene::FormTreatment::FormAccent:
        case scene::FormTreatment::None:
            apply_form_treatment(accent_);
            break;
        }
    }

    // The shape may have just become able to carry the record, so the retained value is
    // re-applied rather than waiting for the next revision that happens to change it.
    apply_record_value();

    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_HIDDEN);
}

void ProgressRing::apply_record_value() {
    if (arc_ == nullptr || !draws_record_) {
        return;
    }

    // An unknown progress leaves the sweep where it was and the caller hides the record:
    // resetting it to zero on the way out would animate a wrong value during the change.
    if (!record_.known) {
        return;
    }

    lv_arc_set_value(arc_, static_cast<std::int32_t>(record_.percent));
}

void ProgressRing::set_progress(const app_model::ProgressValue& progress) {
    if (arc_ == nullptr) {
        return;
    }

    record_ = progress;
    apply_record_value();
}

} // namespace prd::ui::component
