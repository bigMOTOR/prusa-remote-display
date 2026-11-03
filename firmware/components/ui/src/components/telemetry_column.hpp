// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// sdkconfig.h first and explicitly: a method below is gated on a CONFIG_PRD_UI_* macro, and a
// #if on an undefined macro is silently false — which would leave this declaration and its
// definition disagreeing.
#include "sdkconfig.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "components/telemetry_slots.hpp"
#include "scenes/operational_form.hpp"
#include "theme/theme.hpp"

// Production UI Specification §8 Telemetry Column, containing §8's Temperature Row
// entries.
//
// INTERNAL HEADER of the ui component.
//
// §8 Telemetry Column: "Rows maintain stable vertical positions." §8 Temperature
// Row: "Numeric alignment shall remain fixed." §7 Layout Rules: "Components shall
// occupy stable positions whenever equivalent information exists" and "Hidden
// components shall not cause unrelated layout movement."
//
// Those four sentences together are why a row is a fixed slot rather than an entry
// in a list: the slots are allocated once at build time from
// layout.hpp's kTelemetryRowHeight, and showing or hiding a value moves nothing.
//
// THIS COMPONENT OWNS NO USER-VISIBLE STRING. Each row's label is requested from the
// text resource boundary by id (text/scene_text.hpp). A literal here would have to be
// edited to add a language, and this is a shared component, so it would be edited for
// every scene at once.
//
// ABSENCE. A row whose value is unknown draws NOTHING — its label included. The rule is
// row_is_present() in telemetry_slots.hpp, and UI Design Decision 004 §7 is why: "No
// component shall be retained empty to imply anything." A label standing over no value
// implies that the value merely went unreported, which is the implication §7 prohibits.
//
// There is no placeholder either: UI Design Decision 003 requires that "information that
// does not yet exist shall remain absent rather than represented by placeholders", and
// Production UI §9 forbids simulated and placeholder data three more times. The accepted
// artwork shows `---` in these positions; that is the one place the artwork and the written
// specifications disagree outright, and the specifications are followed (owner decision,
// 2026-08-06).
//
// CHANGED IN MILESTONE 2 PHASE G.3, AND IT REVERSED A PHASE D DECISION. Until then the row
// kept its label and dropped only the numeral. The owner confirmed that reading on hardware,
// then — having seen the whole family — became unsure it did not simply look unfinished, and
// UI Design Decision 004 was accepted after that and settles it. The no-dashes rule was never
// in question and is untouched.
//
// THE SLOT IS NOT RECLAIMED. An absent row hides in place: the rows below it do not move up,
// because §8 owns this component's row positions ("Rows maintain stable vertical positions")
// and §7 makes the resulting negative space structural. Whether a column's remaining rows
// should re-centre on the extent they occupy is UIDD 004 §6's question and is deferred.
//
// Owner-reviewed 2026-09-06: exact 16x18 A8 nozzle and heated-bed assets accompany
// reported temperature values. Icon, value and label share one presence decision.

namespace prd::ui::component {

// TelemetrySlot and kTelemetrySlotCount live in components/telemetry_slots.hpp, which
// carries no LVGL include. That lets the slot-to-label bindings in text/scene_text.hpp be
// typed by slot and host-tested without compiling this widget.

class TelemetryColumn {
  public:
    // Builds all three slots at fixed positions derived from `region`. Returns false
    // if any LVGL allocation failed.
    [[nodiscard]] bool build(lv_obj_t* parent, const theme::Rect& region);

    // Sets a slot's value. An empty view makes the whole row absent — value and label
    // together, hidden in place — which is how absence is rendered; see the note above.
    void set_value(TelemetrySlot slot, std::string_view text);

    void set_visible(bool visible);

    // Scales every row's opacity by a 0...255 body level — UI Design Decision 004 §4.
    //
    // TELEMETRY RECEDES WITH THE COMPOSITION. §9 says so directly: "It recedes with the
    // composition, but never on the job's schedule." Owner ruling, 2026-08-08: the second clause
    // governs presence and content — which §7 already rules — and not luminance, so this column
    // is part of §4's one body rather than an exception to it.
    //
    // Each row's two elements keep their own §5 steps and are scaled by the same factor, so the
    // value stays above its label by exactly the proportion §5 established.
    void apply_level(std::int32_t level);

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
    // Reports each row's measured ink and whether the operational form's stroke reaches it.
    //
    // D8 accepted that the form's bounding box overlaps this region on the promise that drawn
    // content stays clear of the arc, and this is where that promise is measured.
    //
    // TAKES THE CURRENT FORM. Before Phase E the form was one of two fixed shapes and this
    // gauge could assume the ring's own circle; it is now a function of the state's energy, so
    // a gauge holding kRingDiameter and kRingCentreY would measure a circle that is not on the
    // panel. This component has already produced one lying gauge — a box-based version
    // reported a collision that did not exist once the values were ranged right — and a lying
    // diagnostic is worse than none.
    void log_geometry(const std::optional<scene::FormGeometry>& form) const;
#endif

    [[nodiscard]] bool built() const {
        return container_ != nullptr;
    }

  private:
    struct Row {
        lv_obj_t* value{nullptr};
        lv_obj_t* label{nullptr};
        lv_obj_t* icon{nullptr};
    };

    lv_obj_t* container_{nullptr};
    std::array<Row, kTelemetrySlotCount> rows_{};
};

} // namespace prd::ui::component
