// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "presentation_diff.hpp"

namespace prd::ui {

bool PresentationDelta::any() const {
    return full || scene || content || printer_name || stale || update || progress || remaining ||
           nozzle || bed || layers || components;
}

PresentationDelta diff(const app_model::Presentation& previous, const app_model::Presentation& next,
                       const bool first_application) {
    PresentationDelta delta;

    delta.scene = select(previous) != select(next);

    // A scene change replaces the composition, so every value in the incoming scene
    // is new to it regardless of whether the projection's field changed. The same is
    // true of the very first application: nothing on screen has been set yet.
    delta.full = first_application || delta.scene;

    if (delta.full) {
        delta.progress = true;
        delta.remaining = true;
        delta.nozzle = true;
        delta.bed = true;
        delta.layers = true;
        delta.components = true;
        delta.content = true;
        delta.printer_name = true;
        delta.stale = true;
        delta.update = true;
        return delta;
    }

    // The content table resolves from the subject state and its condition, so either moving
    // re-resolves everything the state decides. Compared here rather than by calling resolve()
    // twice: the inputs are cheaper to compare than the output, and this file must stay free
    // of the scene layer.
    //
    // `job_record_supplied` joined the list in Phase G.5 because it is the table's other
    // structural input — it decides the stack's arrangement — and it is the one input that can
    // move while every diffed VALUE stands still. At reduced confidence the record's values are
    // already absent (§10), so a job disappearing there changes the arrangement without moving
    // progress, layers or remaining. Omitting it would leave the composition holding a row for
    // a record that no longer exists.
    // `progress.known` joined it on 2026-08-14, and its absence was a drawn defect rather than a
    // theoretical gap. `operational_content.cpp` sets `content.show_progress =
    // presentation.progress.known` at four sites, and the scene passes exactly that to
    // `ProgressRing::set_form()` as the record flag — so it is a table input by construction, and
    // the only one that was not compared here.
    //
    // WHAT IT COST. Printing → reduced confidence → Printing leaves the subject, the condition,
    // the printer name and `job_record_supplied` all unmoved: the machine did supply a record
    // throughout, §10 merely decayed it and then restored it. So `delta.content` stayed false
    // across the return, `set_form()` was never re-called, and the ring kept the treatment it had
    // been left in while the numeral — gated on `delta.progress` — updated normally. The panel
    // drew 62 % over a ring with no sweep at all.
    //
    // `.known` ALONE, NEVER THE WHOLE VALUE. `delta.progress` already carries the percentage.
    // Comparing the value here would re-resolve the content table, the luminance and the form on
    // every one-percent tick — the "reporting too much" failure this file's own header warns
    // against, and a full-composition redraw every poll.
    delta.content = previous.subject != next.subject ||
                    previous.subject_condition != next.subject_condition ||
                    previous.printer_name_known != next.printer_name_known ||
                    previous.job_record_supplied != next.job_record_supplied ||
                    previous.progress.known != next.progress.known;

    delta.printer_name = previous.printer_name != next.printer_name;
    delta.stale = previous.stale_indicator != next.stale_indicator ||
                  previous.stale_observed_at != next.stale_observed_at;

    delta.update = previous.ota_phase != next.ota_phase ||
                   previous.update_version != next.update_version ||
                   previous.update_progress != next.update_progress ||
                   previous.update_notice != next.update_notice;

    delta.progress = previous.progress != next.progress;

    // The approximate marker is part of how the duration reads, so it belongs to the
    // duration's flag rather than to one of its own: a job whose estimate quality
    // improved without its seconds changing still has to redraw the text.
    delta.remaining = previous.remaining != next.remaining ||
                      previous.remaining_is_approximate != next.remaining_is_approximate;

    // Only the current reading is compared. Targets are carried by the projection and
    // no Phase C composition draws them (§9's Printing list has no target, and the
    // artwork shows none), so comparing them would report changes nothing renders.
    delta.nozzle = previous.nozzle.current != next.nozzle.current;
    delta.bed = previous.bed.current != next.bed.current;

    delta.layers = previous.layers != next.layers;

    delta.components = previous.show_operational_form != next.show_operational_form ||
                       previous.show_telemetry != next.show_telemetry;

    return delta;
}

} // namespace prd::ui
