// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "diagnostic_overlay.hpp"

#include <cstdint>

// LVGL headers are not clean under the project warning policy. Suppressed around the include only
// (per architecture spec §12.3); see lvgl_panel.hpp for why the suppression is scoped this tightly
// rather than applied to the whole TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "lvgl.h"
#pragma GCC diagnostic pop

namespace prd::ui::diagnostic_overlay {
namespace {

// ⛔ DIAGNOSTIC ONLY, AND STRUCTURALLY SO, in two independent steps. This file is added to the
// source list only inside the CONFIG_PRD_UI_DIAGNOSTIC_SCENE block of the component's CMakeLists,
// so no ordinary debug or release image contains it at all. And in that profile setup() in ui.cpp
// selects validation::build() and does not call controller::build(), so the overlay is drawn only
// where the product scene is not. It therefore cannot be reached from — or included in — an
// ordinary debug or release image.
//
// ⬅️ NOTE WHAT IS *NOT* CLAIMED: controller.cpp and the rest of the product scene library stay in
// the base PRD_UI_SRCS list and are compiled in every profile, this one included. What the
// diagnostic profile changes is which builder setup() calls, not which sources exist.
//
// ⛔ AND IT IS AN INSTRUMENT, NOT A COMPOSITION PROPOSAL. Nothing here is a candidate for any
// screen. The grey wedge exists so the owner can see whether the dark end of the pixel range
// survives at each backlight level; the labels exist so the panel — not chat — says which step is
// showing.

lv_obj_t* g_id = nullptr;
lv_obj_t* g_label = nullptr;

// A fixed neutral reference: six flat patches from white to black. Deliberately unchanging, so
// the only thing that varies between steps is the backlight.
constexpr std::uint8_t kWedgeGreys[] = {255, 204, 153, 102, 51, 0};
constexpr std::int32_t kWedgeCount = 6;
constexpr std::int32_t kWedgeX = 12;
constexpr std::int32_t kWedgeY = 92;
constexpr std::int32_t kWedgeWidth = 49;
constexpr std::int32_t kWedgeHeight = 96;

[[nodiscard]] lv_obj_t* diagnostic_text(lv_obj_t* const parent, const lv_font_t* const font,
                                        const std::int32_t y) {
    lv_obj_t* const label = lv_label_create(parent);
    if (label == nullptr) {
        return nullptr;
    }

    lv_obj_remove_style_all(label);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, "");
    lv_obj_set_pos(label, kWedgeX, y);

    return label;
}

} // namespace

// Built on the active screen AFTER validation::build(), opaque and full-bleed, so it owns what is
// visible. The diagnostic scene beneath it is retained deliberately: its Kconfig symbol is the
// gate that keeps every product scene out of this image.
bool build() {
    lv_obj_t* const root = lv_obj_create(lv_screen_active());
    if (root == nullptr) {
        return false;
    }

    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 320, 240);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x111315), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    g_id = diagnostic_text(root, &lv_font_montserrat_48, 2);
    g_label = diagnostic_text(root, &lv_font_montserrat_14, 60);

    if (g_id == nullptr || g_label == nullptr) {
        return false;
    }

    for (std::int32_t index = 0; index < kWedgeCount; ++index) {
        lv_obj_t* const patch = lv_obj_create(root);
        if (patch == nullptr) {
            return false;
        }

        const std::uint8_t grey = kWedgeGreys[index];

        lv_obj_remove_style_all(patch);
        lv_obj_set_pos(patch, kWedgeX + (index * kWedgeWidth), kWedgeY);
        lv_obj_set_size(patch, kWedgeWidth, kWedgeHeight);
        lv_obj_set_style_bg_color(patch, lv_color_make(grey, grey, grey), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(patch, LV_OPA_COVER, LV_PART_MAIN);
    }

    lv_obj_t* const footer = diagnostic_text(root, &lv_font_montserrat_14, 200);
    if (footer == nullptr) {
        return false;
    }

    lv_label_set_text(footer, "DIAGNOSTIC INSTRUMENT - NOT A PRODUCT SCREEN");
    return true;
}

void update(const char* const id, const char* const label) {
    // The surface may not exist yet: the queue is created before the overlay is built, so a label
    // posted in that window has nowhere to go. Dropping it is correct — the harness holds every
    // step indefinitely and will post the same label again on the next press.
    if (g_id == nullptr || g_label == nullptr) {
        return;
    }

    lv_label_set_text(g_id, id);
    lv_label_set_text(g_label, label);
}

} // namespace prd::ui::diagnostic_overlay
