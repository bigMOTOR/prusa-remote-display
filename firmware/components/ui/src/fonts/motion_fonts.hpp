// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "lvgl.h"
#include <cstdint>
namespace prd::ui::fonts {
// Fixed, flash-resident whole-pixel sizes. Never allocate a transformed text layer.
const lv_font_t* motion_value(std::int32_t size);
const lv_font_t* motion_unit(std::int32_t size);
const lv_font_t* motion_title(std::int32_t size);
} // namespace prd::ui::fonts
