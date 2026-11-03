// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Internal seam between the panel flows and the one place that chooses between
// them. Not a public interface: app_core calls validation::start_panel_flow() and
// does not know these exist.
//
// Each starter is a no-op when its flow is not compiled, so panel_flow.cpp contains
// the precedence rule and nothing else.

namespace prd::validation::internal {

// Milestone 1 Phase C/D: the display pattern, clock-sweep, colour A/B and
// backlight flows. Compiled only when PRD_DISPLAY_TEST_PATTERNS is set.
//
// These drive the panel directly, bypassing the renderer, so they are mutually
// exclusive with it: app_core does not start ui when they are compiled in.
void start_display_patterns();

[[nodiscard]] bool display_patterns_compiled();

// Milestone 1 Phase F, narrowed in Milestone 2 Phase C: the independent cross-check
// between LVGL's flush accounting and the panel's transfer accounting, plus the
// driver-log quietening the continuous-redraw soak needs. Compiled only when
// PRD_UI_RENDER_REPORT is set.
//
// Does not bring the panel up. Product display and renderer initialisation is
// app_core's, per Architecture Spec §15.1 steps 8 and 14.
void start_render_report();

[[nodiscard]] bool render_report_compiled();

} // namespace prd::validation::internal
