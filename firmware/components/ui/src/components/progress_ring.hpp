// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>

#include "app_model/values.hpp"
#include "scenes/operational_accent.hpp"
#include "scenes/operational_form.hpp"
#include "scenes/resting_line.hpp"
#include "theme/theme.hpp"

// Production UI Specification §8 Progress Ring and §8 Ready Horizon, drawn with §6's
// canonical Progress Geometry.
//
// INTERNAL HEADER of the ui component.
//
// ONE PRIMITIVE, ONE CONTINUUM OF SHAPES. §6 is explicit: "The Ready horizon is the terminal
// geometric transformation of the Progress Ring and shall not exist as an independent
// primitive", and §8 repeats it — "Derived from Progress Ring geometry. Independent
// implementation is prohibited." UI Design Decision 004 §3.1 states what that implies:
// the form's shape IS operational energy, "every intermediate shape is reachable and
// legitimate", and "a form that exists only at its extremes is two forms".
//
// One form uses the shared R2 bounded line rasteriser in the temporary resting interval
// and the arc rasteriser at accepted endpoints. Both are retained and never draw together.
// This is the same seam used by UnavailableAcknowledgement, not independent geometry.
//
// §6's Progress Geometry governs the whole axis — stroke, round cap, `color.accent`,
// concentric construction. What varies is the circle they are drawn on.

namespace prd::ui::component {

class ProgressRing {
  public:
    // Builds the arc. Its shape is set by set_form(), which the scene calls with the
    // presentation's resolved content — so a freshly built ring draws nothing until the first
    // presentation says which energy it is at.
    [[nodiscard]] bool build(lv_obj_t* parent);

    // Places the form at an energy, or hides it when the composition draws no form.
    //
    // `draws_record` is the state's job record, NOT its shape (§3.2: "The record shall never
    // determine the form's shape"). It decides only whether the record is drawn ON the form as
    // a sweep — which §3.2 permits "wherever the shape can host it", and a proportion of a
    // cycle can only be hosted faithfully by a cycle. Where the shape cannot host it, §3.2 is
    // explicit that "the record survives as the numeric value", which the scene draws.
    //
    // The transformation is applied to the same object: size, position and angles change
    // together. Idempotent, and cheap enough to call on every application — but the scene
    // gates it behind the field-level diff anyway, because changing an arc's geometry
    // invalidates its whole area (§11 Redraw).
    void set_form(std::optional<scene::Energy> energy, bool draws_record,
                  std::int32_t offset_y = 0);
    void set_form(std::optional<scene::Energy> energy, bool draws_record, std::int32_t offset_y,
                  std::uint8_t cycle_treatment_alpha);

    // §10 Progress Ring: "Sweep follows printer progress. Progress never animates beyond
    // reported values." So the sweep is set from the value and nothing interpolates: what is
    // drawn is what the printer reported.
    //
    // The value is remembered whether or not the current shape can draw it, and re-applied when
    // the shape changes. That makes the pair order-independent: a scene that set the record
    // before the form would otherwise leave the sweep at its previous value for the whole time
    // a state was displayed, with the numeral beside it reading correctly.
    void set_progress(const app_model::ProgressValue& progress);

    [[nodiscard]] bool built() const {
        return arc_ != nullptr;
    }

    // The geometry currently drawn, or nothing when the form is hidden. Used by the clearance
    // diagnostics, which must measure what is on the panel rather than a circle the
    // composition no longer draws.
    [[nodiscard]] const std::optional<scene::FormGeometry>& form() const {
        return form_;
    }

    // The published account of what is on the panel — see the contract at
    // ui::OperationalFormReport. DIAGNOSTIC AND READ-BACK ONLY: nothing in the product path
    // may branch on it.
    //
    // It reads the four values this component retained when it last placed the form and calls
    // no LVGL API, which is why it is safe from a task other than the renderer's — the same
    // basis on which controller::report() is already read cross-task.
    [[nodiscard]] OperationalFormReport report() const {
        return scene::form_report(energy_, form_, accent_, draws_record_, cycle_treatment_alpha_);
    }

  private:
    lv_obj_t* arc_{nullptr};
    lv_obj_t* segment_{nullptr};
    lv_point_precise_t points_[scene::kRestingLineCapacity]{};

    // The energy the form was last placed at, retained beside the geometry it produced.
    //
    // The geometry does not carry it, and recovering it by inverting the derivation would be
    // absurd for a diagnostic. Keeping the two side by side is also what makes it impossible
    // for a report to name one energy and describe the shape of another.
    std::optional<scene::Energy> energy_{};

    std::optional<scene::FormGeometry> form_{};

    // The accent the form was last drawn in, as 0xRRGGBB — the value handed to LVGL, not one
    // derived again at read time (UI Design Decision 004 §5.1; see set_form()). Zero exactly
    // when no form is drawn, which the report treats as part of the all-or-nothing set.
    std::uint32_t accent_{0};

    // Whether an actual printer-supplied record sweep is painted, including while its cycle
    // treatment is entering or leaving. Stable open forms still carry none.
    bool draws_record_{false};

    // The actual blend coordinate handed to the renderer. Retained so diagnostic read-back
    // cannot mislabel an intermediate treatment as one of its stable endpoints.
    std::uint8_t cycle_treatment_alpha_{0};

    // The last record handed over, retained across shape changes — see set_progress().
    app_model::ProgressValue record_{};

    void apply_geometry(const scene::FormGeometry& form);
    void apply_record_treatment(std::uint32_t accent);
    void apply_cycle_transition_treatment(std::uint32_t accent, std::uint8_t alpha,
                                          bool draws_record);
    void apply_form_treatment(std::uint32_t accent);

    // The closed shape with no record to host: the track alone, no accent. Takes no accent
    // argument because it draws none — see the §5.1 ruling recorded at scene::FormTreatment.
    void apply_absent_record_treatment();
    void apply_record_value();
};

} // namespace prd::ui::component
