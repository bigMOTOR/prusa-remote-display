// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fonts/motion_fonts.hpp"
#include <algorithm>
#include <array>

extern "C" {
extern const lv_font_t prd_font_hero_68;
extern const lv_font_t prd_font_display_36;
extern const lv_font_t prd_font_title_20;
extern const lv_font_t prd_font_outcome_28;
extern const lv_font_t prd_font_motion_value_44;
extern const lv_font_t prd_font_motion_value_45;
extern const lv_font_t prd_font_motion_value_46;
extern const lv_font_t prd_font_motion_value_47;
extern const lv_font_t prd_font_motion_value_48;
extern const lv_font_t prd_font_motion_value_49;
extern const lv_font_t prd_font_motion_value_50;
extern const lv_font_t prd_font_motion_value_51;
extern const lv_font_t prd_font_motion_value_52;
extern const lv_font_t prd_font_motion_value_53;
extern const lv_font_t prd_font_motion_value_54;
extern const lv_font_t prd_font_motion_value_55;
extern const lv_font_t prd_font_motion_value_56;
extern const lv_font_t prd_font_motion_value_57;
extern const lv_font_t prd_font_motion_value_58;
extern const lv_font_t prd_font_motion_value_59;
extern const lv_font_t prd_font_motion_value_60;
extern const lv_font_t prd_font_motion_value_61;
extern const lv_font_t prd_font_motion_value_62;
extern const lv_font_t prd_font_motion_value_63;
extern const lv_font_t prd_font_motion_value_64;
extern const lv_font_t prd_font_motion_value_65;
extern const lv_font_t prd_font_motion_value_66;
extern const lv_font_t prd_font_motion_value_67;
extern const lv_font_t prd_font_motion_unit_23;
extern const lv_font_t prd_font_motion_unit_24;
extern const lv_font_t prd_font_motion_unit_25;
extern const lv_font_t prd_font_motion_unit_26;
extern const lv_font_t prd_font_motion_unit_27;
extern const lv_font_t prd_font_motion_unit_28;
extern const lv_font_t prd_font_motion_unit_29;
extern const lv_font_t prd_font_motion_unit_30;
extern const lv_font_t prd_font_motion_unit_31;
extern const lv_font_t prd_font_motion_unit_32;
extern const lv_font_t prd_font_motion_unit_33;
extern const lv_font_t prd_font_motion_unit_34;
extern const lv_font_t prd_font_motion_unit_35;
extern const lv_font_t prd_font_motion_title_21;
extern const lv_font_t prd_font_motion_title_22;
extern const lv_font_t prd_font_motion_title_23;
extern const lv_font_t prd_font_motion_title_24;
extern const lv_font_t prd_font_motion_title_25;
extern const lv_font_t prd_font_motion_title_26;
extern const lv_font_t prd_font_motion_title_27;
}

namespace prd::ui::fonts {
const lv_font_t* motion_value(const std::int32_t size) {
    static const std::array<const lv_font_t*, 25> faces{
        &prd_font_motion_value_44, &prd_font_motion_value_45, &prd_font_motion_value_46,
        &prd_font_motion_value_47, &prd_font_motion_value_48, &prd_font_motion_value_49,
        &prd_font_motion_value_50, &prd_font_motion_value_51, &prd_font_motion_value_52,
        &prd_font_motion_value_53, &prd_font_motion_value_54, &prd_font_motion_value_55,
        &prd_font_motion_value_56, &prd_font_motion_value_57, &prd_font_motion_value_58,
        &prd_font_motion_value_59, &prd_font_motion_value_60, &prd_font_motion_value_61,
        &prd_font_motion_value_62, &prd_font_motion_value_63, &prd_font_motion_value_64,
        &prd_font_motion_value_65, &prd_font_motion_value_66, &prd_font_motion_value_67,
        &prd_font_hero_68};
    return faces[static_cast<std::size_t>(std::clamp<std::int32_t>(size, 44, 68) - 44)];
}
const lv_font_t* motion_unit(const std::int32_t size) {
    static const std::array<const lv_font_t*, 14> faces{
        &prd_font_motion_unit_23, &prd_font_motion_unit_24, &prd_font_motion_unit_25,
        &prd_font_motion_unit_26, &prd_font_motion_unit_27, &prd_font_motion_unit_28,
        &prd_font_motion_unit_29, &prd_font_motion_unit_30, &prd_font_motion_unit_31,
        &prd_font_motion_unit_32, &prd_font_motion_unit_33, &prd_font_motion_unit_34,
        &prd_font_motion_unit_35, &prd_font_display_36};
    return faces[static_cast<std::size_t>(std::clamp<std::int32_t>(size, 23, 36) - 23)];
}
const lv_font_t* motion_title(const std::int32_t size) {
    static const std::array<const lv_font_t*, 9> faces{
        &prd_font_title_20,        &prd_font_motion_title_21, &prd_font_motion_title_22,
        &prd_font_motion_title_23, &prd_font_motion_title_24, &prd_font_motion_title_25,
        &prd_font_motion_title_26, &prd_font_motion_title_27, &prd_font_outcome_28};
    return faces[static_cast<std::size_t>(std::clamp<std::int32_t>(size, 20, 28) - 20)];
}
} // namespace prd::ui::fonts
