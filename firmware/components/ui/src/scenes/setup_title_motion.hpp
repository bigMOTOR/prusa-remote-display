// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "fonts/fonts.hpp"
#include "src/misc/lv_text_private.h"
#include "theme/theme.hpp"

namespace prd::ui::scene {

// A single A8 title mask in scene storage. Use the native LVGL label at both endpoints;
// interpolating corresponding glyphs here keeps one readable title through line reflow.
class SetupTitleMotion {
  public:
    void attach(lv_obj_t* label) {
        lv_obj_add_event_cb(label, draw,
                            static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS),
                            this);
        lv_obj_add_event_cb(label, draw, LV_EVENT_REFR_EXT_DRAW_SIZE, this);
    }

    void stop(lv_obj_t* label) {
        if (active_) {
            lv_obj_invalidate(label);
            active_ = false;
            lv_obj_refresh_ext_draw_size(label);
        }
    }

    void update(lv_obj_t* label, float progress, std::int32_t compact_width, std::int32_t qr_travel,
                std::int32_t action_clearance) {
        const auto text = std::string_view{lv_label_get_text(label)};
        const auto* const body = fonts::for_role(theme::TextRole::Body);
        const auto* const title = fonts::for_role(theme::TextRole::StateTitle);
        lv_text_attributes_t wrap{};
        wrap.max_width = compact_width;
        wrap.letter_space = theme::kTrackingDefault;
        const auto split = lv_text_get_next_line(
            text.data(), static_cast<std::uint32_t>(text.size()), body, nullptr, &wrap);
        const bool wrapped = split < text.size();
        const auto travel = static_cast<float>(qr_travel);
        const auto title_clear = static_cast<float>(theme::kLineTitle) / travel;
        // Lift within the gap between Body lines as the suffix travels sideways. Once the
        // QR clears the lifted line, finish the horizontal move before bringing it up
        // alongside the prefix. Leave a pixel of clearance before the action can appear.
        const auto early_lift = static_cast<float>(theme::kSpace1);
        const auto wrap_clear = (static_cast<float>(kHeight) - early_lift) / travel;
        const auto joined_y = static_cast<float>(action_clearance - 1) / travel;
        const auto joined_x = joined_y - ((early_lift / 2.0F) / travel);
        const auto morph = ease(progress, wrapped ? joined_y : title_clear, 1.0F);
        const auto lift = early_lift * ease(progress, 0.0F, wrap_clear) +
                          (theme::kLineBody - early_lift) * ease(progress, joined_x, joined_y);
        const auto body_join = lv_text_get_width(text.data(), split, body, &wrap);
        const auto suffix_width = lv_text_get_width(
            text.data() + split, static_cast<std::uint32_t>(text.size() - split), body, &wrap);
        const auto side_room =
            std::clamp(compact_width - suffix_width - theme::kSpace1, 0, body_join);
        const auto shift_x =
            mix(0.0F, static_cast<float>(side_room), ease(progress, 0.0F, wrap_clear)) +
            mix(static_cast<float>(side_room - body_join), 0.0F,
                ease(progress, wrap_clear, joined_x)) -
            static_cast<float>(side_room);
        const auto shift_y = static_cast<float>(theme::kLineBody) - lift;

        mask_.fill(0);
        int body_x = 0;
        int title_x = 0;
        std::uint32_t offset = 0;
        while (offset < text.size()) {
            const bool suffix = wrapped && offset >= split;
            std::uint32_t letter = 0, next = 0;
            lv_text_encoded_letter_next_2(text.data(), &letter, &next, &offset);
            const Glyph from{body, letter, next, body_x};
            const Glyph to{title, letter, next, title_x};
            body_x += from.metrics.adv_w + theme::kTrackingDefault;
            title_x += to.metrics.adv_w + theme::kTrackingDefault;
            if (from.bits != nullptr && to.bits != nullptr) {
                paint(from, to, suffix ? shift_x : 0.0F, suffix ? shift_y : 0.0F, morph);
            }
        }

        image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_.header.cf = LV_COLOR_FORMAT_A8;
        image_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        image_.header.w = kWidth;
        image_.header.h = kHeight;
        image_.header.stride = kWidth;
        image_.data_size = mask_.size();
        image_.data = mask_.data();
        if (!active_) {
            active_ = true;
            lv_obj_refresh_ext_draw_size(label);
        }
        lv_obj_invalidate(label);
    }

  private:
    static constexpr int kWidth = theme::kCanvasContent.width;
    static constexpr int kHeight = 2 * theme::kLineBody;
    alignas(4) std::array<std::uint8_t, kWidth * kHeight> mask_{};
    lv_image_dsc_t image_{};
    bool active_{false};

    static float mix(float from, float to, float fraction) {
        return from + (to - from) * fraction;
    }

    static float ease(float progress, float start, float end) {
        const auto t = std::clamp((progress - start) / (end - start), 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    }

    struct Glyph {
        lv_font_glyph_dsc_t metrics{};
        const std::uint8_t* bits{nullptr};
        int x{0};
        int y{0};

        Glyph(const lv_font_t* font, unsigned letter, unsigned next, int cursor) {
            if (!lv_font_get_glyph_dsc(font, &metrics, letter, next)) {
                return;
            }
            x = cursor + metrics.ofs_x;
            y = font->line_height - font->base_line - metrics.box_h - metrics.ofs_y;
            if (metrics.box_w > 0 && metrics.box_h > 0) {
                metrics.req_raw_bitmap = 1;
                bits = static_cast<const std::uint8_t*>(font->get_glyph_bitmap(&metrics, nullptr));
            }
        }

        float pixel(int column, int row) const {
            if (column < 0 || row < 0 || column >= metrics.box_w || row >= metrics.box_h) {
                return 0.0F;
            }
            // The committed Body and StateTitle faces are plain four-bit coverage maps.
            const int stride = metrics.stride != 0 ? metrics.stride * 8 : metrics.box_w * 4;
            const int bit = row * stride + column * 4;
            return static_cast<float>(((bits[bit / 8] >> (4 - bit % 8)) & 15) * 17);
        }

        float sample(float u, float v) const {
            const auto x = u * metrics.box_w - 0.5F;
            const auto y = v * metrics.box_h - 0.5F;
            const auto col = static_cast<int>(std::floor(x));
            const auto row = static_cast<int>(std::floor(y));
            const auto dx = x - static_cast<float>(col);
            const auto dy = y - static_cast<float>(row);
            return mix(mix(pixel(col, row), pixel(col + 1, row), dx),
                       mix(pixel(col, row + 1), pixel(col + 1, row + 1), dx), dy);
        }
    };

    void paint(const Glyph& from, const Glyph& to, float shift_x, float shift_y, float fraction) {
        const auto x =
            mix(static_cast<float>(from.x) + shift_x, static_cast<float>(to.x), fraction);
        const auto y =
            mix(static_cast<float>(from.y) + shift_y, static_cast<float>(to.y), fraction);
        const auto width = mix(static_cast<float>(from.metrics.box_w),
                               static_cast<float>(to.metrics.box_w), fraction);
        const auto height = mix(static_cast<float>(from.metrics.box_h),
                                static_cast<float>(to.metrics.box_h), fraction);
        const auto right = std::min(kWidth, static_cast<int>(std::ceil(x + width + 0.5F)));
        const auto bottom = std::min(kHeight, static_cast<int>(std::ceil(y + height + 0.5F)));
        const auto first_x = std::max(0, static_cast<int>(std::floor(x - 0.5F)));
        const auto first_y = std::max(0, static_cast<int>(std::floor(y - 0.5F)));
        for (int row = first_y; row < bottom; ++row) {
            const auto v = (static_cast<float>(row) + 0.5F - y) / height;
            for (int col = first_x; col < right; ++col) {
                const auto u = (static_cast<float>(col) + 0.5F - x) / width;
                const auto alpha = mix(from.sample(u, v), to.sample(u, v), fraction);
                auto& target = mask_[static_cast<std::size_t>(row * kWidth + col)];
                target = std::max(target, static_cast<std::uint8_t>(alpha + 0.5F));
            }
        }
    }

    static void draw(lv_event_t* event) {
        auto* const self = static_cast<SetupTitleMotion*>(lv_event_get_user_data(event));
        if (!self->active_) {
            return;
        }
        if (lv_event_get_code(event) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
            lv_event_set_ext_draw_size(event, theme::kLineBody);
            return;
        }
        auto* const label = static_cast<lv_obj_t*>(lv_event_get_target(event));
        lv_draw_label_dsc_t style;
        lv_draw_label_dsc_init(&style);
        lv_obj_init_draw_label_dsc(label, LV_PART_MAIN, &style);
        lv_draw_image_dsc_t image;
        lv_draw_image_dsc_init(&image);
        image.src = &self->image_;
        image.recolor = style.color;
        image.recolor_opa = theme::opacity(theme::Opacity::Full);
        image.opa = style.opa;
        lv_area_t area;
        lv_obj_get_content_coords(label, &area);
        area.x2 = area.x1 + kWidth - 1;
        area.y2 = area.y1 + kHeight - 1;
        lv_draw_image(lv_event_get_layer(event), &image, &area);
        lv_event_stop_processing(event);
    }
};

} // namespace prd::ui::scene
