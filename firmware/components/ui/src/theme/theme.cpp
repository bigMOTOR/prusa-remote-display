// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "theme/theme.hpp"

#include "fonts/fonts.hpp"

namespace prd::ui::theme {
namespace {

// §5's percentages scaled to LVGL's 0...255. Rounded to nearest so that
// `opacity.full` maps to LV_OPA_COVER exactly and the rest land on the nearest
// representable step: 88% → 224, 64% → 163, 40% → 102, 24% → 61.
[[nodiscard]] constexpr lv_opa_t to_lv_opa(const std::uint8_t percent) {
    return static_cast<lv_opa_t>(((static_cast<std::uint32_t>(percent) * 255U) + 50U) / 100U);
}

static_assert(to_lv_opa(kOpacityFullPercent) == LV_OPA_COVER,
              "opacity.full must be fully opaque, or a background will not cover");

} // namespace

lv_color_t colour(const Colour token) {
    switch (token) {
    case Colour::Background:
        return lv_color_hex(kColorBackground);
    case Colour::Surface:
        return lv_color_hex(kColorSurface);
    case Colour::Primary:
        return lv_color_hex(kColorPrimary);
    case Colour::Secondary:
        return lv_color_hex(kColorSecondary);
    case Colour::Tertiary:
        return lv_color_hex(kColorTertiary);
    case Colour::Accent:
        return lv_color_hex(kColorAccent);
    case Colour::Success:
        return lv_color_hex(kColorSuccess);
    case Colour::Warning:
        return lv_color_hex(kColorWarning);
    case Colour::Error:
        return lv_color_hex(kColorError);
    case Colour::Disabled:
        // §5: "Derived from color.tertiary". The derivation is tertiary at
        // `opacity.disabled`; the hue is tertiary's, and a caller that wants the
        // disabled appearance pairs this with Opacity::Disabled.
        return lv_color_hex(kColorDisabled);
    }

    return lv_color_hex(kColorPrimary);
}

lv_color_t colour_from_rgb(const std::uint32_t rgb) {
    return lv_color_hex(rgb);
}

void apply_text_level(lv_obj_t* const label, const Opacity token, const std::int32_t level) {
    if (label == nullptr) {
        return;
    }

    // Both factors are 0...255, so the product cannot exceed 65,025 and the arithmetic stays
    // inside the int it promotes to on either target.
    const std::int32_t scaled = (opacity(token) * level) / kBodyLevelFull;

    // LVGL refreshes and invalidates even for an identical style value. A resting
    // confidence/estimate row must not keep the display refresh timer awake.
    const auto value = static_cast<lv_opa_t>(scaled);
    if (lv_obj_get_style_text_opa(label, LV_PART_MAIN) != value) {
        lv_obj_set_style_text_opa(label, value, LV_PART_MAIN);
    }
}

lv_opa_t opacity(const Opacity token) {
    switch (token) {
    case Opacity::Full:
        return to_lv_opa(kOpacityFullPercent);
    case Opacity::High:
        return to_lv_opa(kOpacityHighPercent);
    case Opacity::Medium:
        return to_lv_opa(kOpacityMediumPercent);
    case Opacity::Low:
        return to_lv_opa(kOpacityLowPercent);
    case Opacity::Disabled:
        return to_lv_opa(kOpacityDisabledPercent);
    case Opacity::Transparent:
        return LV_OPA_TRANSP;
    }

    return to_lv_opa(kOpacityFullPercent);
}

void apply_text_style(lv_obj_t* const label, const TextRole role, const Colour token,
                      const Opacity alpha) {
    if (label == nullptr) {
        return;
    }

    lv_obj_set_style_text_font(label, fonts::for_role(role), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, colour(token), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, opacity(alpha), LV_PART_MAIN);

    // §4's line heights are absolute values, and LVGL expresses line spacing as the
    // gap *between* lines. The gap is therefore the token's line height minus the
    // face's own, which keeps a two-line label on §4's baseline grid instead of on
    // whatever the rasteriser produced.
    const lv_font_t* const font = fonts::for_role(role);
    const std::int32_t face_height = font == nullptr ? 0 : font->line_height;
    const std::int32_t gap = line_height(role) - face_height;
    lv_obj_set_style_text_line_space(label, gap > 0 ? gap : 0, LV_PART_MAIN);

    // §4 Tracking: both tokens are zero. Set explicitly rather than left to LVGL's
    // default, so a future non-zero tracking token takes effect here and not in
    // twenty call sites.
    lv_obj_set_style_text_letter_space(label, kTrackingDefault, LV_PART_MAIN);
}

void place(lv_obj_t* const object, const Rect& rect) {
    if (object == nullptr) {
        return;
    }

    lv_obj_set_pos(object, rect.x, rect.y);
    lv_obj_set_size(object, rect.width, rect.height);
}

lv_obj_t* bare_object(lv_obj_t* const parent) {
    lv_obj_t* const object = lv_obj_create(parent);
    if (object == nullptr) {
        return nullptr;
    }

    lv_obj_remove_style_all(object);

    // §7 Layout Behaviour: layout does not reflow. A scrollable container would let
    // content move independently of the region that owns it, and §11 requires
    // components never to render outside the canvas — which scrolling defeats.
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);

    // The MVP has no touchscreen (Architecture Spec §2.2), so nothing on screen is a
    // control. Clearing the flag also keeps LVGL from reserving input state per
    // object.
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);

    return object;
}

lv_obj_t* bare_label(lv_obj_t* const parent) {
    lv_obj_t* const label = lv_label_create(parent);
    if (label == nullptr) {
        return nullptr;
    }

    lv_obj_remove_style_all(label);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

} // namespace prd::ui::theme
