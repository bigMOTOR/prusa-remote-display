// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "scenes/operational_content.hpp"

namespace prd::ui::scene {

OperationalContent resolve(const app_model::Presentation& presentation) {
    OperationalContent content;

    // THE FORM'S SHAPE IS THE STATE'S ENERGY (UIDD 004 §3.1), and nothing else. Its PRESENCE is
    // the projection's single §8 flag, which is true for every member of the family (§13.1's
    // constant inventory). Neither is the job's record: §3.2, "the record shall never determine
    // the form's shape".
    //
    // Two conditions rather than one, and they are genuinely different questions. A state with
    // no energy is not in the family at all (operational_energy.hpp), and the scene selector
    // does not route such a state here — the guard exists so that "outside the family" can
    // never be silently rendered as "at the bottom of the axis".
    if (const std::optional<Energy> energy = energy_for(presentation.subject); energy.has_value()) {
        if (presentation.show_operational_form) {
            content.form = energy;
        }
    }

    switch (presentation.subject) {
    case app_model::SubjectState::Ready:
        // §9 Ready: "Ready Horizon, Printer identity, Current temperatures, Status summary",
        // and explicitly hidden — "Print progress, Remaining time, Layer information". The
        // artwork agrees: board 2 panel 1 shows the printer's name where the operational
        // panels show a state word.
        //
        // ⛔ AND AT VISUAL REST IT CARRIES NOTHING AT ALL — owner ruling, 2026-08-29, with the
        // accepted physical reference at assets/Ready Resting F1.jpeg: stable healthy Ready is
        // the horizon and nothing else. No name, no address, no temperatures, no third line.
        //
        // THIS IS THE CONFLICT THIS CASE USED TO RESOLVE THE OTHER WAY. UI Behaviour Decision 001
        // §2 forbids normal Idle from permanently displaying temperatures and forbids showing
        // information "merely because it exists"; §9 forbids telemetry dashboards; UI Design
        // Decision 002 accepts F1 as the minimum-energy resting expression and rules out
        // placeholder telemetry. Production UI §9 nevertheless lists printer identity,
        // temperatures and a status summary for Ready. The owner's ruling resolves it in favour
        // of the arc, and Production UI §9 is the Source that needs the correction — recorded,
        // not edited here.
        //
        // Owner 2026-09-07: degraded Ready communicates last-observation age, not a printer
        // alias. An independent attention/error condition retains its existing identity
        // treatment; reduced confidence alone does not invent one.
        if (!presentation.at_visual_rest &&
            (!presentation.stale_indicator ||
             presentation.subject_condition != app_model::SubjectCondition::None)) {
            content.title =
                presentation.printer_name_known ? TitleSource::PrinterName : TitleSource::None;
        }
        break;

    case app_model::SubjectState::Preparing:
        // §9 Preparing: "State Header, Ready Horizon transitioning toward Progress Ring,
        // Temperatures, Heating progress."
        //
        // HEATING PROGRESS IS NOT DRAWN, because the model does not carry it. `progress` is
        // the job's completion, and a print that has not started has none — showing it would
        // be inventing a value, which §9 forbids in the same breath ("No simulated progress
        // is permitted"). If a later milestone learns a heating fraction from PrusaLink, it
        // becomes a field on Presentation and appears here.
        //
        // AND NEITHER IS AN ACTIVITY WORD. Preparing carried "Heating up" until Milestone 2
        // Phase G.4, and it was invented. API Research Report §16.4 — an Accepted Project
        // Source — lists the possible cases as "heating; homing; mesh probing; pre-print
        // activity; another busy operation" and states that "the API may not identify the
        // exact preparation phase". So the machine does not supply it, and UIDD 004 §7 requires
        // that it do so ("a component is present if and only if the machine supplies its
        // value"), while §8 requires the third line to hold "whichever machine fact is
        // currently true". It failed both, and it was wrong four times in five.
        //
        // THE ROUTE BACK IS NOT A JUDGEMENT CALL, WHICH IS WHY THIS LINE CANNOT SIMPLY BE
        // RESTORED. It needs an accepted source establishing a real activity field, that field
        // reaching Presentation, and this case reading it. Anything short of that is the same
        // invention under a different word.
        content.title = TitleSource::Catalogue;
        content.title_id = text::TextId::StatePreparing;
        break;

    case app_model::SubjectState::Printing:
        content.show_progress = presentation.progress.known;
        content.record_row = presentation.job_record_supplied;
        content.title = TitleSource::Catalogue;
        content.title_id = text::TextId::StatePrinting;
        content.show_remaining = presentation.remaining.known;
        break;

    case app_model::SubjectState::Paused:
        // §9 Paused: "Visible components are identical to Printing", with the header
        // reflecting the paused state, progress stopped and its value unchanged.
        //
        // The remaining duration is deliberately withheld. A paused print has no meaningful
        // time remaining — the estimate is against printing time, and the printer is not
        // printing — so continuing to show it would present a stale figure as current, which
        // UI Behaviour Decision 001 §7 forbids outright.
        //
        // The artwork's "Resume print" subtitle is also absent: it names an action, and the
        // MVP is read-only toward the printer (§2.2, invariant 18).
        content.show_progress = presentation.progress.known;
        content.record_row = presentation.job_record_supplied;
        content.title = TitleSource::Catalogue;
        content.title_id = text::TextId::StatePaused;
        break;

    case app_model::SubjectState::Completed:
        // §9 Print Completed: identical to Printing, the ring at completed progress, and the
        // acknowledgement in `color.success`.
        //
        // §9 also says the acknowledgement "is temporary" and the interface "shall settle
        // toward the Ready presentation". The settling is a transition with a duration, and
        // no accepted source gives one — so what is built is the acknowledgement itself, and
        // the settle is left to the phase that defines its timing. The state is residual in
        // the model either way, so nothing is lost by drawing it statically.
        // OUTCOME FIRST. The acknowledgement takes the top row and the record follows it. §9
        // makes the acknowledgement what this state exists to say, and §5.2 gives it the only
        // chromatic text in the family; leading with the record put the job's historical figure
        // above the outcome, so a reader met "100 %" and had to look below it to learn what had
        // happened.
        //
        // THE RECORD IS NOT REMOVED, ONLY MOVED. §9 keeps Completed's components identical to
        // Printing, and §3.2 requires the record to survive as the numeric value wherever the
        // form cannot host it — which at Completed's energy it cannot, because the shape is
        // open. Nothing is hidden and nothing is resized; the two rows exchange places.
        content.show_progress = presentation.progress.known;
        content.record_row = presentation.job_record_supplied;
        content.arrangement = Arrangement::Outcome;
        content.title = TitleSource::Catalogue;
        content.title_id = text::TextId::StateCompleted;
        content.title_colour = theme::Colour::Success;
        break;

    case app_model::SubjectState::Stopped:
        // §9 Print Stopped: identical to Printing, the header reflecting the interruption,
        // "Progress remains visible" and last known telemetry retained.
        //
        // The accepted artwork disagrees — board 2 panel 6 shows Stopped with no ring and a
        // resting horizon. The written specification is followed because §1 makes it
        // authoritative for composition, and the projection already implements it under a
        // test named for the conflict. Recorded rather than resolved; it remains a carried
        // decision, and flipping it is one line in the projection.
        content.show_progress = presentation.progress.known;
        content.record_row = presentation.job_record_supplied;
        content.title = TitleSource::Catalogue;
        content.title_id = text::TextId::StateStopped;
        break;

    case app_model::SubjectState::AcquiringState:
        // §9 Connecting hides the Progress Ring and the telemetry, and there is no resting
        // composition to show before a first state has ever been acquired. The scene selector
        // does not route this here; the case is listed so that adding a subject state fails
        // to compile until it has been considered.
        break;
    }

    // A COMPOSITION WITH NO RECORD CANNOT LEAD WITH ONE, and cannot follow one either. Ready and
    // Preparing carry none by definition; a state whose record the machine never supplied is in
    // the same position and gets the same two-line stack. Derived here rather than repeated in
    // three cases, so the rule has one place and cannot be half-applied — Completed included.
    //
    // IT READS `record_row`, NOT `show_progress`, AND THE DIFFERENCE IS THE WHOLE OF PHASE G.5.
    // The two were the same fact while "no record" had one cause: the machine never sent one.
    // UIDD 004 §10 gave it a second — reduced confidence removes a supplied record's values —
    // and the two demand opposite answers. §7's absence is a different composition; §10's is
    // the same composition with a row left empty, because "Confidence shall change no geometry,
    // luminance, typography, layout or energy."
    //
    // Reading `show_progress` here would move a degraded Printing's state word from offset 52
    // to 26 and its third line from 72 to 46 — the layout following confidence, which §10
    // forbids outright and the Phase G invariant forbids again. The empty row that remains is
    // the accepted state of affairs; whether it should close up is §6's question, deferred.
    if (!content.record_row) {
        content.arrangement = Arrangement::Resting;
    }

    return content;
}

} // namespace prd::ui::scene
