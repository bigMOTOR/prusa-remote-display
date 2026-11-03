// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>

#include "app_model/presentation.hpp"
#include "scenes/operational_energy.hpp"
#include "text/text.hpp"
#include "theme/tokens.hpp"

// What the operational composition shows for each subject state.
//
// INTERNAL to the ui component. LVGL-free, so the whole per-state content table is
// host-testable — which is the point: this is where every Phase D scene decision lives, and
// a decision that only exists inside an LVGL builder cannot be checked by anything.
//
// ONE COMPOSITION, NOT FIVE SCENES. Production UI §9 says Paused, Completed and Stopped have
// "visible components identical to Printing", and the accepted continuity artwork is titled
// "one composition, one state flow". Ready and Preparing use the same frame with the ring in
// its resting form. So the operational family is a single composition whose *content*
// varies, and this resolves that content. Five near-duplicate scene builders would guarantee
// the drift between them that the artwork exists to prevent.
//
// The geometry is not here and does not vary: every state uses the same regions, so a
// component that is present in two states is in the same place in both (§7: "Components
// shall occupy stable positions whenever equivalent information exists").

namespace prd::ui::scene {

// Where the centre stack's title line gets its text.
//
// Ready is the odd one and the artwork is explicit about it: board 2 panel 1 shows the
// printer's name where every other panel shows a state word. That follows Production UI §9,
// which lists "Printer identity" among Ready's visible components and lists no state title —
// a resting device names the machine, not its condition.
enum class TitleSource : std::uint8_t {
    None,
    Catalogue,   // a TextId: the state's own word
    PrinterName, // the identity carried by the presentation
};

// How the centre stack is ordered — which component holds which row.
//
// §6 fixes the region: "Regions are fixed in position and horizontal extent… No component shall
// leave its region. No region shall move." This decides only what sits where INSIDE it. All
// three arrangements use the same three line heights and fill region.operational exactly, so an
// arrangement change moves text and changes no geometry at all.
enum class Arrangement : std::uint8_t {
    // The record, the state word, the third line. Printing, Paused and Stopped.
    Operational,

    // No record to lead with, so the state word and the third line centre in the region.
    // Ready and Preparing, and any state whose record the machine never supplied.
    Resting,

    // COMPLETED ONLY: the acknowledgement first, the record beneath it.
    //
    // §9 makes Completed's acknowledgement the thing that state exists to say, and §5.2 gives it
    // the family's only chromatic text. Leading with the record instead put the job's historical
    // figure above the outcome — the reader met "100 %" and had to look below it to learn what
    // had happened. This is the adopted outcome-first hierarchy.
    Outcome,
};

struct OperationalContent {
    // The operational form: the energy it is drawn at, or nothing when this composition draws
    // no form at all.
    //
    // ONE FIELD RATHER THAN A FLAG AND A MAGNITUDE, so presence and shape cannot disagree.
    //
    // There is deliberately no enumeration of forms here any more. UI Design Decision 004 §3.1
    // requires one form whose shape IS energy — "every intermediate shape is reachable and
    // legitimate; a form that exists only at its extremes is two forms" — and a two- or
    // three-valued enum is precisely a form that exists only at its extremes. Production UI §6
    // says the same thing from the other side: the Ready Horizon "is the terminal geometric
    // transformation of the Progress Ring and shall not exist as an independent primitive".
    // The shape is now a function (scenes/operational_form.hpp) rather than a choice.
    std::optional<Energy> form;

    // The large numeral. Absent in the resting states, which carry no progress.
    //
    // THIS IS THE RECORD, AND IT IS SEPARATE FROM THE FORM (§3.2: "The record shall never
    // determine the form's shape"). A state's form follows its energy whatever the record
    // does; the record decides only whether the job's figure is shown, and — where the shape
    // can host it — whether the form carries a sweep.
    bool show_progress{false};

    // Whether this composition has a record row at all — as distinct from whether that row has
    // something to draw right now.
    //
    // THE TWO WERE ONE FLAG UNTIL PHASE G.5. `show_progress` answered both, which was correct
    // only while the sole reason for a missing record was that the machine never sent one. UIDD
    // 004 §10 added a second reason — reduced confidence removes a supplied record's values —
    // and a composition that took its shape from `show_progress` would then rearrange itself as
    // the machine became unobservable. §10 forbids exactly that: "Confidence shall change no
    // geometry, luminance, typography, layout or energy."
    //
    // So a degraded Printing keeps its three-line stack with the record's row empty, while a
    // Printing whose record the machine never supplied gets the two-line one — same absent
    // numeral, different reason, different composition.
    bool record_row{false};

    // The order the centre stack is built in. Separate from `show_progress`, which is the
    // record's PRESENCE: presence answers §7, and this answers §6's question of where a present
    // component sits. The two agreed for the whole family until Completed needed to show the
    // record and not lead with it.
    Arrangement arrangement{Arrangement::Operational};

    TitleSource title{TitleSource::None};
    text::TextId title_id{text::TextId::StatePrinting};

    // The third line — UIDD 004 §8: "a single slot holding whichever machine fact is currently
    // true", which "shall not restate the state word, and shall not name an action the product
    // cannot perform".
    //
    // ONE CANDIDATE, SO ONE FLAG. Printing's remaining duration is the only machine fact the
    // family currently has for this slot. Preparing carried a catalogue word until Phase G.4,
    // when it went for failing §7 — the machine does not supply a preparation phase (API
    // Research Report §16.4) — and the second flag and its text id went with it rather than
    // being left set by nobody. A never-true flag is a component retained empty in the source
    // instead of on the panel, and it is the shape a removed line grows back through.
    //
    // §8's precedence — "activity outranks estimate" — is therefore recorded rather than
    // encoded, at the slot's one resolution point in operational_scene.cpp. Ranking one
    // candidate against nothing would require inventing the thing that lost.
    bool show_remaining{false};

    // THE ACCENT HAS ONE REFERENT, AND IT IS NOT THIS (UIDD 004 §5.1: the accent "is applied to
    // the operational form alone", and "all Operational Family text is achromatic, except under
    // 5.2"). Until Phase F the state word was drawn in the accent in five of the six states and
    // Ready's printer name in the sixth, so a word wore the operational colour and competed with
    // the form for the meaning §5.1 assigns to one element.
    //
    // PRIMARY RATHER THAN A LOWER STEP, because this is a role change and not a hierarchy one.
    // The state word is the composition's top-level text — in Ready and Preparing it is the only
    // text in the region — and demoting it to `color.secondary` would be a composition decision
    // that Phase F does not own. §13.1 also fixes text roles across all six states, so the five
    // achromatic states take one role between them rather than one each.
    //
    // §9 Print Completed: "Completion acknowledgement uses `color.success`." That is §5.2's
    // single exception — the state word only, at Completed only — and it is the only chromatic
    // text the family may draw.
    theme::Colour title_colour{theme::Colour::Primary};
};

// Resolves the content for a presentation the operational composition has been selected for.
//
// Takes the whole Presentation rather than just the subject state, because two states differ
// on what is *available* rather than on what they are: Printing without a known duration
// shows no third line, and a printer whose name was never learned shows no title in Ready.
[[nodiscard]] OperationalContent resolve(const app_model::Presentation& presentation);

} // namespace prd::ui::scene
