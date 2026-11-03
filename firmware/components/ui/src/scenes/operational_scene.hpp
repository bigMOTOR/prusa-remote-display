// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// sdkconfig.h first and explicitly: a private method below is gated on a CONFIG_PRD_UI_*
// macro, and a #if on an undefined macro is silently false — which would leave the
// declaration and the definition disagreeing.
#include "sdkconfig.h"

#include "app_model/presentation.hpp"
#include "components/download_mark.hpp"
#include "components/progress_ring.hpp"
#include "components/telemetry_column.hpp"
#include "fonts/motion_title_raster.hpp"
#include "presentation_diff.hpp"
#include "scenes/operational_content.hpp"
#include "scenes/operational_luminance.hpp"
#include "scenes/operational_motion.hpp"
#include "theme/theme.hpp"

// Production UI Specification §9's operational family, as one composition.
//
// INTERNAL HEADER of the ui component.
//
// ONE SCENE FOR SIX STATES. §9 says Paused, Completed and Stopped have "visible components
// identical to Printing"; Ready and Preparing use the same frame with the progress geometry
// in its resting form; and the accepted continuity artwork is titled "one composition, one
// state flow". So the family is a single scene whose content varies:
//
//     region.primary       the Progress Ring, as a ring or as the Ready Horizon
//     region.operational   progress percentage, state title, third line
//     region.telemetry     nozzle, bed, layer
//     below the stack      the degraded-confidence indicator, when §11.5 requires one
//
// Five near-duplicate scene builders would guarantee the drift between states that the
// artwork exists to prevent — and, while the geometry is a working baseline rather than an
// accepted design, one composition also means the family review changes one set of numbers
// instead of five.
//
// THIS SCENE OWNS NO POLICY. What is presentable was decided by the projection; what each
// state shows was decided by operational_content.hpp, which is pure and host-tested; how a
// value reads was decided by the formatters; where anything sits was decided by layout.hpp.
// This file places objects and updates their text.

namespace prd::ui::scene {

class OperationalScene {
  public:
    // Builds the composition under `parent`. Returns false if any LVGL allocation
    // failed. Render task only.
    [[nodiscard]] bool build(lv_obj_t* parent);

    // Applies a presentation, touching only what `delta` reports as changed.
    //
    // The delta is a parameter rather than something this class derives, so the
    // comparison stays in one host-tested place (§17.4) and the scene cannot form a
    // second opinion about whether a value moved.
    void apply(const app_model::Presentation& presentation, const PresentationDelta& delta);

    void set_visible(bool visible);

    // Called by the real controller loop after panel visibility is known.
    [[nodiscard]] bool step(bool panel_visible, std::uint32_t now);
    void resume_from_form(FormPosition form) {
        motion_.resume_from_form(form);
    }
    void resume_from_blank() {
        blank_entry_ = true;
    }
    // Renderer-local continuity seam. Never used for policy or read-back diagnostics.
    [[nodiscard]] std::optional<FormPosition> painted_form() const;

    [[nodiscard]] bool built() const {
        return root_ != nullptr;
    }

    // What this composition currently has on the panel — see the contract at
    // ui::OperationalFormReport. DIAGNOSTIC AND READ-BACK ONLY: nothing in the product path
    // may branch on it, and this scene itself never reads it back.
    [[nodiscard]] OperationalFormReport form_report() const {
        return ring_.report();
    }

  private:
    lv_obj_t* root_{nullptr};

    component::ProgressRing ring_{};
    component::TelemetryColumn telemetry_{};
    component::DownloadMark update_available_{};

    // The centre stack. UIDD 001's "Progress percentage in the center. Status under the
    // percentage. Remaining time below status." — with the percentage absent in the resting
    // states and the third line carrying either a duration or a subtitle.
    lv_obj_t* percent_value_{nullptr};
    lv_obj_t* percent_unit_{nullptr};
    lv_obj_t* state_title_{nullptr};
    fonts::MotionTitleRaster title_raster_{};
    lv_obj_t* third_line_{nullptr};
    lv_obj_t* pause_bars_[2]{};
    OperationalMotion motion_{};
    OperationalContent content_{};
    bool unknown_progress_{false};
    bool blank_entry_{false};
    bool printer_condition_{false};
    lv_obj_t* error_outline_{nullptr};
    lv_obj_t* error_mark_{nullptr};
    void paint_condition();
    void paint_motion();
    [[nodiscard]] std::int32_t painted_body_level() const;

    // §11.5: stale values may be presented only behind an explicit indicator.
    lv_obj_t* stale_indicator_{nullptr};
    OperationalMotion stale_motion_{};
    std::optional<app_model::Timestamp> stale_observed_at_{};
    app_model::BoundedString<32> stale_text_{};
    bool stale_target_{false};
    void paint_stale(std::uint32_t now);

    // Face metrics measured once at build time. The value and its unit are repositioned
    // together whenever the digit count changes, so the pair stays optically centred on the
    // ring — see the note at reposition_value().
    std::int32_t digit_advance_{0};
    std::int32_t unit_advance_{0};
    std::int32_t baseline_offset_{0};

    // The record's row within the region, set by the current arrangement, and the digit count
    // last placed into it. Kept so that a rearrangement can move the pair immediately: the width
    // is a property of the value and the row is a property of the stack, and the two change on
    // different occasions.
    std::int32_t percent_row_top_{0};
    std::size_t percent_digits_{0};

    void reposition_value(std::size_t digits);

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
    // Reports the composition that is actually on the panel: the form's measured geometry, and
    // each drawn row against it. Per applied state, because the form is a function of energy.
    void log_composition();
#endif

    void apply_title(const app_model::Presentation& presentation,
                     const OperationalContent& content);
    void apply_third_line(const app_model::Presentation& presentation,
                          const OperationalContent& content);

    // Scales the whole composition's luminance to the state's energy — UI Design Decision 004
    // §4. One level, applied to every participating element, so the body recedes together and
    // §5's internal hierarchy is preserved rather than flattened.
    //
    // The operational form is not here: §5.1 gives it one accent, `color.accent`, drawn at full
    // opacity at every energy — so this level would be the only thing dimming it, and §4 governs
    // the body, not the form. Neither is the stale indicator, by owner ruling and §10.
    void apply_luminance(const OperationalContent& content);
};

} // namespace prd::ui::scene
