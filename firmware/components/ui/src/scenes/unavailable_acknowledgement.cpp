// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/unavailable_acknowledgement.hpp"
#include "scenes/resting_line.hpp"

#include "scenes/operational_form.hpp"

#include "text/text.hpp"
#include <algorithm>
#include <cmath>

#include "theme/layout.hpp"

namespace prd::ui::scene {
namespace {

// The angular span is always given as (0, extent) with the placement done by the rotation — the same
// call shape components/progress_ring.cpp uses, for the reason recorded there.
constexpr std::int32_t kSpanStart = 0;

} // namespace

bool UnavailableAcknowledgement::build(lv_obj_t* const parent) {
    if (parent == nullptr) {
        return false;
    }

    arc_ = lv_arc_create(parent);
    if (arc_ == nullptr) {
        return false;
    }

    // Stripped exactly as components/progress_ring.cpp strips its own, and for the reasons recorded
    // there: an arc is LVGL's slider widget by default, and this is not a control.
    lv_obj_remove_style_all(arc_);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_mode(arc_, LV_ARC_MODE_NORMAL);
    lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);

    // §6's Progress Geometry: the stroke and the cap, shared by every shape on the axis. The COLOUR
    // and the OPACITY are per-frame, because §3.2's treatment is — see apply().
    lv_obj_set_style_arc_width(arc_, theme::kRingStroke, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc_, true, LV_PART_MAIN);

    // ⛔ THE INDICATOR IS SILENT FOR THE WHOLE LIFE OF THIS OBJECT, because there is no record here
    // to be a proportion of. Silenced rather than given zero extent, for the reason
    // ProgressRing::apply_absent_record_treatment() records: a zero-extent arc with a round cap still
    // paints a dot, and a dot reads as a value where there is none. No range or value is set for the
    // same reason.
    lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Transparent), LV_PART_INDICATOR);

    // ⬅️ THE SECOND RASTERISER FOR THE SAME FORM. Built once, beside the arc and never with it:
    // apply() shows exactly one of the two, so the panel carries one form at every frame.
    segment_ = lv_line_create(parent);
    if (segment_ == nullptr) {
        return false;
    }

    lv_obj_remove_style_all(segment_);
    lv_obj_remove_flag(segment_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(segment_, LV_OBJ_FLAG_SCROLLABLE);

    // ⛔ THE SAME STROKE AND THE SAME CAP AS THE ARC, FROM THE SAME TOKENS. §6's Progress
    // Geometry is one stroke and one cap for every shape on the axis, so the two rasterisers
    // read the same two constants rather than each carrying its own.
    lv_obj_set_style_line_width(segment_, theme::kRingStroke, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(segment_, true, LV_PART_MAIN);

    // Absolute panel coordinates, so the sampled points are the circle's own screen points and no
    // second placement rule exists. The object covers the panel and paints only where the
    // polyline is.
    lv_obj_set_pos(segment_, 0, 0);
    lv_obj_set_size(segment_, theme::kDisplayWidth, theme::kDisplayHeight);

    if (!copy_.build(parent, theme::kCanvasContent.width)) {
        return false;
    }

    // ⛔ POSITIONED HERE, BECAUSE StatusMessage::build() DELIBERATELY DOES NOT. It sets width and
    // wrap and leaves placement to its caller; a label left unpositioned lands at LVGL's (0, 0),
    // which is outside canvas.inset and touching the display edge, and §7 forbids both.
    lv_obj_set_pos(copy_.object(), theme::kCanvasContent.x, kUnavailableCopyBaseY);
    lv_obj_set_style_text_align(copy_.object(), LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    hide();
    return true;
}

void UnavailableAcknowledgement::apply(const UnavailablePhase& phase) {
    if (!built()) {
        return;
    }
    painted_phase_ = phase;
    lv_obj_set_style_opa(arc_, phase.form_opacity, LV_PART_MAIN);
    lv_obj_set_style_opa(segment_, phase.form_opacity, LV_PART_MAIN);

    // The pre-existing resting interval has varying width/centre and passes through flat.
    // Use one bounded stroke throughout that interval, so allocator size and whole-degree
    // quantisation cannot select a discontinuous seam. Accepted axis knots stay on lv_arc.
    if (phase.energy > kEnergyReady && phase.energy < kEnergyStopped) {
        apply_segment(phase);
    } else {
        apply_arc(phase);
    }
}

void UnavailableAcknowledgement::apply_arc(const UnavailablePhase& phase) {
    lv_obj_add_flag(segment_, LV_OBJ_FLAG_HIDDEN);
    rasteriser_ = Rasteriser::Arc;
    point_count_ = 0;

    // The form, on the operational family's own circle. bounding_box() is the same helper the
    // progress ring places itself with; the only difference is the settled downward offset.
    theme::Rect box = bounding_box(phase.form);
    box.y = form_top_y(phase);
    theme::place(arc_, box);
    lv_arc_set_bg_angles(arc_, kSpanStart, phase.form.extent);
    lv_arc_set_rotation(arc_, phase.form.rotation);

    // ⛔ §3.2'S TREATMENT, AS THE LAW DECIDED IT — NOT A SECOND STYLING RULE. The phase carries
    // form_treatment()'s own verdict, so what is painted here is the decision the host tests assert.
    switch (phase.treatment) {
    case FormTreatment::TrackOnly:
        // A CLOSED SHAPE WITH NO RECORD TO HOST — which is what a Printing origin's first frames are,
        // because a lost observation has no reading. ProgressRing::apply_absent_record_treatment()'s
        // own tokens: the silent track, and no accent at all.
        //
        // ⛔ PAINTING THE ACCENT HERE IS FORBIDDEN. A closed cycle in the full accent is exactly what
        // a COMPLETED, 100 % record looks like, and this composition has no record whatsoever. It was
        // drawn that way once; the firing control exists so it cannot be again.
        lv_obj_set_style_arc_color(arc_, theme::colour(theme::Colour::Tertiary), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Low), LV_PART_MAIN);
        break;

    case FormTreatment::FormAccent:
        // An open shape can host no record either way, so the form itself is the drawn element and
        // carries the accent — `color.accent`, the same at every energy (§5.1, owner ruling
        // 2026-09-01) — with ProgressRing::apply_form_treatment()'s own tokens. The SHAPE is the
        // motion; the colour is constant through it, so nothing switches colour on arrival.
        lv_obj_set_style_arc_color(arc_, theme::colour_from_rgb(phase.accent), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc_, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);
        break;

    case FormTreatment::RecordSweep:
    case FormTreatment::None:
        // Unreachable: this composition never hosts a record, and never runs while undrawn. The
        // whole composition goes rather than being left in whatever the previous frame set, so a
        // treatment nobody here handles cannot silently inherit a style.
        hide();
        return;
    }

    lv_obj_remove_flag(arc_, LV_OBJ_FLAG_HIDDEN);
    apply_copy(phase);
}

// Continuous numerical evaluation plus the affine stroke anchor. The signed-sagitta curvature
// remains energy-derived; the renderer's inward anchor no longer flips sides at zero curvature.
void UnavailableAcknowledgement::apply_segment(const UnavailablePhase& phase) {
    lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
    rasteriser_ = Rasteriser::Segment;

    point_count_ = resting_line_points(phase.energy, phase.offset_y, points_);
    lv_line_set_points(segment_, points_, point_count_);

    // §3.2's treatment, applied to whichever object is drawing. A closed form cannot occur here:
    // closed means a full turn, and a full turn is only reachable at radius 96.
    switch (phase.treatment) {
    case FormTreatment::TrackOnly:
        lv_obj_set_style_line_color(segment_, theme::colour(theme::Colour::Tertiary), LV_PART_MAIN);
        lv_obj_set_style_line_opa(segment_, theme::opacity(theme::Opacity::Low), LV_PART_MAIN);
        break;

    case FormTreatment::FormAccent:
        lv_obj_set_style_line_color(segment_, theme::colour_from_rgb(phase.accent), LV_PART_MAIN);
        lv_obj_set_style_line_opa(segment_, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);
        break;

    case FormTreatment::RecordSweep:
    case FormTreatment::None:
        hide();
        return;
    }

    lv_obj_remove_flag(segment_, LV_OBJ_FLAG_HIDDEN);
    apply_copy(phase);
}

void UnavailableAcknowledgement::apply_copy(const UnavailablePhase& phase) {
    // The copy resolves by opacity, and descends with the form rather than independently of it —
    // the composition settles as one thing.
    if (phase.copy_alpha == 0) {
        copy_.clear();
    } else {
        copy_.set(text::lookup(text::TextId::StatusPrinterUnavailable), theme::Colour::Secondary);
        lv_obj_set_style_text_opa(copy_.object(), phase.copy_alpha, LV_PART_MAIN);
        lv_obj_set_y(copy_.object(), copy_top_y(phase));
    }

    visible_ = true;
}

void UnavailableAcknowledgement::hide() {
    if (arc_ != nullptr) {
        lv_obj_add_flag(arc_, LV_OBJ_FLAG_HIDDEN);
    }

    if (segment_ != nullptr) {
        lv_obj_add_flag(segment_, LV_OBJ_FLAG_HIDDEN);
    }

    rasteriser_ = Rasteriser::None;
    point_count_ = 0;

    copy_.clear();
    visible_ = false;
}

} // namespace prd::ui::scene
