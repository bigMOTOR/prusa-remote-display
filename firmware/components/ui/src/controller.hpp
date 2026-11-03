// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/presentation.hpp"
#include "theme/theme.hpp"
#include "ui/ui.hpp"

// The scene controller — Architecture Spec §5.12 `ui_controller`, §17.3's persistent
// root.
//
// INTERNAL HEADER of the ui component.
//
// §17.3's recommended structure:
//
//     root screen
//     ├── background
//     ├── scene container
//     ├── connection/stale indicator
//     └── transient overlay
//
// Phase C builds the first two. The stale indicator and the overlay arrive with the
// scenes that need them; a container for a component that does not exist would be an
// empty object in the tree that nothing draws and nothing tests.
//
// §17.3: "Switch scene content without recreating the entire display driver." So the
// screen and the background are built once at start-up and never rebuilt, and a scene
// change hides one container and shows another. Scenes are constructed once rather
// than on entry: LVGL object creation allocates from the fixed pool, and building on
// every transition would make the pool's occupancy depend on how many transitions had
// happened — which is a leak that looks like fragmentation.
//
// EVERY FUNCTION HERE IS RENDER-TASK ONLY.

namespace prd::ui::controller {

// Builds the persistent root and every scene on the display's active screen.
[[nodiscard]] bool build(lv_display_t* display_handle);

// Applies a presentation: selects the scene, switches if it changed, and updates only
// what moved. Does nothing when the presentation is identical to the last applied one,
// which is the case §11's "full-screen redraws shall be avoided" is really about — a
// 2-second poll that reports the same values must not redraw anything at all.
void apply(const app_model::Presentation& presentation);

// Advances the transient lost-observation acknowledgement by one loop iteration. Returns true while
// its motion is still running, so the loop can shorten its wait — the same contract, and the same
// reasoning, as ui.cpp's own step_backlight().
//
// ⛔ IT DECIDES NOTHING. Whether an acknowledgement exists at all is AppState::loss_episode, settled
// by the reducer under §12's precedence and published through the projection. This advances a clock
// for a presentation that has already been decided, and returns false the moment there is nothing
// left to move — including when no acknowledgement is being drawn at all.
[[nodiscard]] bool step_acknowledgement();

// Whether the panel can currently be SEEN — the wake-order safeguard's one input.
//
// ⛔ SUPPLIED, NOT INFERRED. The render task reads it from the display component's own applied
// brightness, which is the same seam drain_display_intent() already consults, and forwards it here
// after the backlight has been applied for this tick. Nothing in this file derives visibility from
// confidence, from the selected scene or from elapsed wall time.
//
// ⬅️ WHY IT IS PUSHED RATHER THAN PULLED. The backlight and its ramp are owned by ui.cpp; the
// scene layer is a consumer of that fact, not a second owner of it. Passing it in keeps the one
// owner intact and adds no parallel policy.
void note_panel_visible(bool visible);

// Hands the provisioning composition its session credential — Milestone 3.6.
//
// SEPARATE FROM apply() BECAUSE Presentation STRUCTURALLY CANNOT CARRY IT: a presentation
// is projected from AppState, and §24.3 keeps secrets out of AppState. See the contract at
// ui::SetupSession.
void set_setup_session(const SetupSession& session);

// Ends the session, overwriting the credential here and in LVGL's own label buffer.
void clear_setup_session();

// What the controller last resolved and drew, for the boot report and the validation
// record.
struct Report {
    std::uint32_t applied{0};   // presentations handed to apply()
    std::uint32_t redundant{0}; // identical to the previous one, so nothing drawn
    std::uint32_t scene_changes{0};
    std::uint32_t operational{0};  // presentations drawn as the operational composition
    std::uint32_t printing{0};     // presentations drawn as the Printing composition
    std::uint32_t provisioning{0}; // presentations drawn as the provisioning composition

    // Milestone 3.10: presentations drawn as Production UI §9's Factory Reset screen.
    // Counted like its siblings, so a destructive prompt appearing is visible in
    // diagnostics rather than inferred from a photograph of a panel.
    std::uint32_t factory_reset{0};
    std::uint32_t firmware_update{0};
    std::uint32_t unimplemented{0}; // presentations with no composition yet

    // Whether the transient acknowledgement of lost observation is drawn right now — Production
    // UI §9, UIAD 001 §11.1. A state rather than a counter: it is not a scene, so it is not one of
    // the tallies above, and what a test or a capture needs to know is whether it is on the panel.
    bool observation_lost_acknowledged{false};

    std::string_view scene{};
};

[[nodiscard]] Report report();

// What the operational composition currently has on the panel — see the contract at
// ui::OperationalFormReport. DIAGNOSTIC AND READ-BACK ONLY: nothing in the product path
// may branch on it.
//
// THE SECOND EXCEPTION TO THE RENDER-TASK-ONLY RULE ABOVE, and for the same reason as
// report(): it copies plain values the render task retained and calls no LVGL API.
[[nodiscard]] OperationalFormReport operational_form_report();

// The same read-back contract for the transient form. Never a product control input.
[[nodiscard]] OperationalFormReport observation_lost_form_report();

} // namespace prd::ui::controller
