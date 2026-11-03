// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// LVGL headers are not clean under the project warning policy. Suppressed around
// the include only (per architecture spec §12.3); see lvgl_panel.hpp for why the
// suppression is scoped this tightly rather than applied to the whole TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "lvgl.h"
#pragma GCC diagnostic pop

// INTERNAL HEADER of the ui component. Compiled only when
// CONFIG_PRD_UI_VALIDATION_SCENE is set.
//
// This is NOT a product scene and must not become one. It exists to prove that
// the rendering foundation can draw what the accepted Production UI
// Specification will need, and it is deliberately composed so it cannot be
// mistaken for the accepted Printing or Ready artwork. Product scenes are
// Milestone 2 work, built from the Production UI Specification.

namespace prd::ui::validation {

// Builds the scene on the display's active screen. Render task only, after
// panel::create().
[[nodiscard]] bool build(lv_display_t* display_handle);

} // namespace prd::ui::validation
