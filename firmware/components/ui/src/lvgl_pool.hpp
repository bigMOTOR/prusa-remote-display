// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

namespace prd::ui::pool {

// Call on the UI task before lv_init(); only the PSRAM board uses this path.
[[nodiscard]] bool reserve();

} // namespace prd::ui::pool
