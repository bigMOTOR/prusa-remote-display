// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/presentation.hpp"
#include "scene_selection.hpp"

// Incremental update — Architecture Spec §17.4, Production UI Specification §11
// Redraw.
//
// §17.4: "The UI controller should diff the new UiState against the previous one.
// Update only changed properties." §11: "Only components affected by state changes
// shall be redrawn. Full-screen redraws shall be avoided during normal operation."
//
// LVGL cannot help with this. lv_label_set_text() invalidates the label's area
// whether or not the text differs, so setting every label on every revision would
// redraw the whole composition each time a single temperature moved by a degree —
// which on a 2-second poll is a full-screen redraw every 2 seconds, exactly what §11
// forbids.
//
// So the comparison happens here, on the projection's values rather than on rendered
// text: pure, LVGL-free, and compiled into the host test build. Comparing values
// rather than formatted strings is deliberate — a temperature moving 59.6 → 59.8 °C
// changes the value and not the text, and formatting first would be the only way to
// notice, which would mean formatting everything on every revision to decide whether
// to format anything.
//
// THE FIELD SET IS PHASE C'S. It covers what the Printing composition draws, plus
// the selected scene. It grows with each scene; a field that no scene reads would be
// a comparison nothing acts on.

namespace prd::ui {

struct PresentationDelta {
    // The selected composition changed, so the outgoing scene is hidden and the
    // incoming one is shown and fully applied. Everything else below is then
    // redundant, and `full` says so.
    bool scene{false};

    // Progress drives both the percentage text and the ring's sweep, so they share a
    // flag: they are one value in two presentations and cannot disagree.
    bool progress{false};

    bool remaining{false};
    bool nozzle{false};
    bool bed{false};
    bool layers{false};

    // §8's visibility rules resolved by the projection: show_operational_form and
    // show_telemetry. Which SHAPE the form takes is not here — that follows the state's
    // energy and moves with `content`.
    bool components{false};

    // The subject state or its condition changed, so the operational content table resolves
    // differently: a different title, a different third line, a different ring form, possibly
    // a different stack arrangement. Every state-dependent element is rebuilt on this.
    //
    // Separate from `scene` because the whole operational family is ONE scene: moving from
    // Printing to Paused does not switch compositions, it changes what this one shows. A diff
    // that only knew about scene changes would leave "Printing" on screen after a pause.
    bool content{false};

    // Ready draws the printer's name where other states draw a state word, so the name is a
    // rendered value in that state and has to be compared like one.
    bool printer_name{false};

    // §11.5's explicit indicator for values presented at reduced confidence.
    bool stale{false};

    // Firmware-update phase and download progress. The phase owns the update composition; the
    // numeric value is consumed only while that phase is Downloading.
    bool update{false};

    [[nodiscard]] bool any() const;

    // True when the incoming presentation must be applied in full rather than
    // field by field — a scene change, or the first presentation after a build.
    bool full{false};
};

// `previous` is the presentation last applied. For the first application, pass
// `first_application = true`: a default-constructed Presentation compared against a
// real one would report only the fields that happen to differ from the default, and
// a value that matched the default would never be drawn at all.
[[nodiscard]] PresentationDelta diff(const app_model::Presentation& previous,
                                     const app_model::Presentation& next,
                                     bool first_application = false);

} // namespace prd::ui
