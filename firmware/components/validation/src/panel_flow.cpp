// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "validation/validation.hpp"

#include "panel_flows.hpp"

namespace prd::validation {

void start_panel_flow() {
    // Both of these are no-ops unless their flag is compiled in, so there is no
    // precedence rule left to state here. There used to be: until Milestone 2 Phase C
    // the LVGL flow and the pattern flow both brought the panel up and only one could
    // own it, so this function chose.
    //
    // Product display and renderer bring-up is now app_core's, and the pattern flow's
    // exclusivity with the renderer is settled at compile time — app_core does not
    // start ui when the patterns are compiled in. What is left is two independent
    // instruments that can both run.
    internal::start_display_patterns();
    internal::start_render_report();
}

bool display_patterns_own_panel() {
    return internal::display_patterns_compiled();
}

} // namespace prd::validation
