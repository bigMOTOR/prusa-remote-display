// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// INTERNAL HEADER of the ui component. Compiled only when CONFIG_PRD_UI_DIAGNOSTIC_SCENE is set.
//
// ⛔ BENCH INSTRUMENTATION, NOT A PRODUCT SCREEN, AND NEVER A CANDIDATE FOR ONE. This is the
// owner-paced brightness harness's own surface: a fixed six-patch grey ramp so a person can see
// whether the dark end of the pixel range survives at each backlight level, plus two labels so the
// PANEL — not chat — says which step is showing. It defines no product token and draws no product
// component.
//
// ⬅️ IT LIVES IN THE UI COMPONENT BECAUSE UI IS THE SOLE LVGL OWNER (Architecture Spec §1 decision
// 6, §5.12). The validation component drives the harness's sequence and owns none of its drawing;
// moving LVGL there would break the single-owner rule the whole build depends on.
//
// ⬅️ AND IT IS ITS OWN TRANSLATION UNIT BECAUSE OF check-tokens.sh. Its greys, its geometry and its
// labels are measurement apparatus, so they are exactly the literals Production UI §2 forbids in
// product sources — the same standing that validation_scene.* already has. Keeping them here rather
// than in ui.cpp is what lets the token gate stay exact instead of being weakened.
//
// The API is deliberately narrow and LVGL-free: ui.cpp keeps the FreeRTOS label queue and drains it
// on the render task, and calls in here only to draw. Nothing about the queue crosses this seam.

namespace prd::ui::diagnostic_overlay {

// Builds the overlay on the active screen. Render task only, after validation::build() — it is
// opaque and full-bleed, so it is built last and owns what is visible.
[[nodiscard]] bool build();

// Writes the two labels. Render task only. A no-op until build() has succeeded, so the caller does
// not have to track whether the surface exists.
//
// ⬅️ THESE ARGUMENTS NEED ONLY OUTLIVE THE CALL. lv_label_set_text() allocates and copies the text
// into the label, so nothing here retains either pointer once update() has returned.
//
// ⛔ THAT IS NOT THE CONTRACT ON THE QUEUE, AND THE TWO MUST NOT BE CONFLATED. The public
// ui::DiagnosticLabel in ui/ui.hpp still requires pointers with STATIC storage duration, because
// the FreeRTOS queue carries the pointer VALUES from the posting task and they are not dereferenced
// until the render task drains them — an unbounded time later. The copy happens here, at the end of
// that journey, not when the label is posted.
void update(const char* id, const char* label);

} // namespace prd::ui::diagnostic_overlay
