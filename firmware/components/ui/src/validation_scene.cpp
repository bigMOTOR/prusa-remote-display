// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "validation_scene.hpp"

#include <cstdint>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

// Milestone 1 Phase F LVGL foundation validation scene.
//
// WHAT THIS IS: a checklist made visible. Each element proves one rendering
// capability the accepted Production UI Specification depends on — anti-aliased
// round-capped arc geometry (§6), large and small typography (§4), numeric
// updates that must stay stable (§4 numeric rules), edge clipping (§11), and
// reproduction of the accepted background and accent colours (§5).
//
// WHAT THIS IS NOT: a product screen. The composition is deliberately wrong for
// the product — the arc sits in a corner with nothing inside it, the large
// numeral is unrelated to it, and the screen says so in text. UI Design Decision
// 001 puts the progress ring centred with the percentage inside it; nothing here
// may be allowed to drift toward that, because a validation scene that resembles
// the accepted artwork is a validation scene that gets mistaken for it.
//
// The two colours below are the accepted Project tokens used here purely as
// colour-reproduction probes, exactly as the display component's pattern
// generator uses them. Nothing here defines, alters or re-specifies a token, and
// no token layer is created: that belongs to Milestone 2, where the Production UI
// Specification's token rules take effect.

namespace prd::ui::validation {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

constexpr std::uint32_t kColourBackground = 0x111315; // color.background
constexpr std::uint32_t kColourAccent = 0xF77F2F;     // color.accent
constexpr std::uint32_t kColourPrimary = 0xF2F3F4;    // color.primary
constexpr std::uint32_t kColourTertiary = 0x7C848E;   // color.tertiary

// 20 updates a second. Fast enough that a torn frame, a buffer reused under DMA
// or a dropped flush completion shows up as visible corruption within seconds
// rather than needing a long soak to appear once.
constexpr std::uint32_t kCounterPeriodMs = 50;

lv_obj_t* g_counter_label = nullptr;
std::uint32_t g_counter = 0;

void counter_timer_cb(lv_timer_t* /*timer*/) {
    if (g_counter_label == nullptr) {
        return;
    }

    ++g_counter;

    // Fixed width, so the label's own area never changes size. A value whose
    // width changed would move the surrounding pixels and make "is this tearing
    // or is this reflow?" unanswerable by eye.
    lv_label_set_text_fmt(g_counter_label, "%06lu", static_cast<unsigned long>(g_counter));
}

// Strips the theme so every pixel is one this file asked for. The default theme
// adds radii, borders and background opacity, any of which would make a
// colour-reproduction or clipping observation ambiguous.
lv_obj_t* bare_object(lv_obj_t* const parent) {
    lv_obj_t* const object = lv_obj_create(parent);
    if (object == nullptr) {
        return nullptr;
    }

    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

} // namespace

bool build(lv_display_t* const display_handle) {
    lv_obj_t* const screen = lv_display_get_screen_active(display_handle);
    if (screen == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "no active screen");
        return false;
    }

    // Solid near-black canvas.
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kColourBackground), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    // Scrolling is disabled deliberately. The clipped object below extends past
    // the right edge, which LVGL would otherwise treat as scrollable content —
    // and content that can scroll cannot demonstrate clipping.
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // 1. Anti-aliased arc, 4 px stroke, round caps.
    //
    // Corner-placed and hollow on purpose: this is the Production UI
    // Specification's progress geometry (stroke.emphasis, round cap, accent) being
    // proved as a primitive, not the accepted Printing composition being built.
    lv_obj_t* const arc = lv_arc_create(screen);
    if (arc == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "arc creation failed");
        return false;
    }

    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 96, 96);
    lv_obj_set_pos(arc, 12, 12);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 62);

    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kColourTertiary), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(kColourAccent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    // 2. Small text label. Also the sign that says what this screen is.
    lv_obj_t* const caption = lv_label_create(screen);
    if (caption == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "caption creation failed");
        return false;
    }

    lv_label_set_text(caption, "LVGL FOUNDATION CHECK\nnot a product screen");
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(caption, lv_color_hex(kColourTertiary), LV_PART_MAIN);
    lv_obj_set_pos(caption, 124, 20);

    // 3. Large numeric label. Round and straight strokes at 48 px, which is where
    //    glyph anti-aliasing is easiest to judge by eye.
    lv_obj_t* const large = lv_label_create(screen);
    if (large == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "large label creation failed");
        return false;
    }

    lv_label_set_text(large, "80369");
    lv_obj_set_style_text_font(large, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(large, lv_color_hex(kColourPrimary), LV_PART_MAIN);
    lv_obj_set_pos(large, 14, 124);

    // 4. Rapidly updating numeric value.
    g_counter_label = lv_label_create(screen);
    if (g_counter_label == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "counter creation failed");
        return false;
    }

    lv_label_set_text(g_counter_label, "000000");
    lv_obj_set_style_text_font(g_counter_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_counter_label, lv_color_hex(kColourAccent), LV_PART_MAIN);
    lv_obj_set_pos(g_counter_label, 200, 124);

    if (lv_timer_create(counter_timer_cb, kCounterPeriodMs, nullptr) == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "counter timer creation failed");
        return false;
    }

    // 5. Flat accent colour sample, for comparison against the Phase C1 colour
    //    chart. Flat rather than anti-aliased, so what is being judged is the
    //    panel's colour reproduction and not the renderer's edge blending.
    lv_obj_t* const swatch = bare_object(screen);
    if (swatch == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "swatch creation failed");
        return false;
    }

    lv_obj_set_size(swatch, 72, 24);
    lv_obj_set_pos(swatch, 14, 196);
    lv_obj_set_style_bg_color(swatch, lv_color_hex(kColourAccent), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, LV_PART_MAIN);

    // 6. Clipped object. 96 px wide starting at x=264 on a 320 px display, so
    //    exactly 40 px must be discarded at the right edge — and the discarded
    //    part must not wrap, corrupt the next row, or move anything.
    lv_obj_t* const clipped = bare_object(screen);
    if (clipped == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "clipped object creation failed");
        return false;
    }

    lv_obj_set_size(clipped, 96, 24);
    lv_obj_set_pos(clipped, 264, 196);
    lv_obj_set_style_bg_color(clipped, lv_color_hex(kColourPrimary), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clipped, LV_OPA_COVER, LV_PART_MAIN);

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "validation scene built: arc4px_rounded large48 small14 "
                     "counter50ms swatch_f77f2f clipped_right_edge");
    return true;
}

} // namespace prd::ui::validation
