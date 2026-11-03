// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "fonts/motion_fonts.hpp"
#include "theme/layout.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace prd::ui::fonts {

// Bounded A8 coverage for lower_font moving title. No transform layer, runtime font generation,
// extra label, or allocation. Settled text remains the original LVGL label.
class MotionTitleRaster {
  public:
    struct Pose {
        double size;
        double top;
    };
    void attach(lv_obj_t* label) {
        lv_obj_add_event_cb(label, event,
                            static_cast<lv_event_code_t>(LV_EVENT_DRAW_MAIN | LV_EVENT_PREPROCESS),
                            this);
        lv_obj_add_event_cb(label, event, LV_EVENT_REFR_EXT_DRAW_SIZE, this);
    }

    void update(lv_obj_t* label, Pose pose, bool moving) {
        const double size = pose.size;
        const double top = pose.top;
        if (!moving && !active_) {
            return;
        }
        lv_obj_invalidate(label);
        active_ = false;
        const auto text = std::string_view{lv_label_get_text(label)};
        const int lower_size = static_cast<int>(std::floor(size));
        const auto* lower_font = motion_title(lower_size);
        const auto* upper_font = motion_title(lower_size + 1);
        const double fraction = size - lower_size;
        const int lower_width = width(lower_font, text);
        const int upper_width = width(upper_font, text);
        // User-provided/non-ASCII titles retain the ordinary label path. The motion
        // catalogue uses the bundled plain 4bpp ASCII faces checked below.
        if (!moving || lower_width <= 0 || upper_width <= 0 ||
            std::max(lower_width, upper_width) + 4 > kWidth) {
            lv_obj_refresh_ext_draw_size(label);
            return;
        }
        for (unsigned char letter : text) {
            if (letter >= 128) {
                lv_obj_refresh_ext_draw_size(label);
                return;
            }
        }
        const double baseline =
            top + ((lower_font->line_height - lower_font->base_line) * (1.0 - fraction)) +
            ((upper_font->line_height - upper_font->base_line) * fraction);
        // Integer endpoint alignment is intentional: reproduce LVGL's accepted centring.
        const int lower_left = theme::kRingCentreX - (lower_width / 2);
        const int upper_left = theme::kRingCentreX - (upper_width / 2);
        const double left = (lower_left * (1.0 - fraction)) + (upper_left * fraction);
        const double target_width = (lower_width * (1.0 - fraction)) + (upper_width * fraction);
        x_ = static_cast<int>(std::floor(left)) - 1;
        const double ascent =
            std::max((lower_font->line_height - lower_font->base_line) * size / lower_size,
                     (upper_font->line_height - upper_font->base_line) * size / (lower_size + 1));
        y_ = static_cast<int>(std::floor(baseline) - std::ceil(ascent)) - 1;
        mask_.fill(0);
        ink_top_ = y_ + kHeight;
        ink_bottom_ = y_;
        if (!paint(
                lower_font, text,
                {left, baseline, target_width / lower_width, size / lower_size, 1.0 - fraction}) ||
            (fraction > 0.0 && !paint(upper_font, text,
                                      {left, baseline, target_width / upper_width,
                                       size / (lower_size + 1), fraction}))) {
            lv_obj_refresh_ext_draw_size(label);
            return;
        }
        image_.header.magic = LV_IMAGE_HEADER_MAGIC;
        image_.header.cf = LV_COLOR_FORMAT_A8;
        image_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
        image_.header.w = kWidth;
        image_.header.h = kHeight;
        image_.header.stride = kWidth;
        image_.data_size = mask_.size();
        image_.data = mask_.data();
        active_ = true;
        lv_obj_update_layout(label);
        const int label_x = lv_obj_get_x(label);
        const int label_y = lv_obj_get_y(label);
        // Only ink can extend the label's redraw area, not the transparent A8 storage.
        extension_ = static_cast<int>(std::max<std::int32_t>(
            {0, label_x - x_, label_y - ink_top_,
             x_ + static_cast<int>(std::ceil(target_width)) + 2 - label_x - lv_obj_get_width(label),
             ink_bottom_ + 1 - label_y - lv_obj_get_height(label)}));
        lv_obj_refresh_ext_draw_size(label);
        lv_obj_invalidate(label);
    }

    [[nodiscard]] bool active() const {
        return active_;
    }
    [[nodiscard]] std::int32_t ink_top() const {
        return ink_top_;
    }
    [[nodiscard]] std::int32_t ink_bottom() const {
        return ink_bottom_ + 1;
    }

  private:
    static constexpr int kWidth = theme::kRingDiameter + (2 * theme::kSpace1);
    static constexpr int kHeight = theme::kLineOutcome + (2 * theme::kSpace1);
    alignas(4) std::array<std::uint8_t, static_cast<std::size_t>(kWidth* kHeight)> mask_{};
    lv_image_dsc_t image_{};
    int x_{0}, y_{0}, extension_{0}, ink_top_{0}, ink_bottom_{0};
    bool active_{false};

    static int width(const lv_font_t* font, std::string_view text) {
        int result = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            result += lv_font_get_glyph_width(
                font, static_cast<unsigned char>(text[i]),
                i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0);
        }
        return result;
    }

    static float pixel(const std::uint8_t* data, const lv_font_glyph_dsc_t& glyph, int column,
                       int row) {
        if (column < 0 || row < 0 || column >= glyph.box_w || row >= glyph.box_h) {
            return 0.0F;
        }
        const int bit =
            (glyph.stride != 0 ? row * glyph.stride * 8 : row * glyph.box_w * 4) + (column * 4);
        return static_cast<float>(((data[bit / 8] >> (4 - (bit % 8))) & 15) * 17);
    }

    struct Transform {
        double left;
        double baseline;
        double scale_x;
        double scale_y;
        double weight;
    };

    bool paint(const lv_font_t* font, std::string_view text, Transform transform) {
        const auto [left, baseline, scale_x, scale_y, weight] = transform;
        const auto* descriptor = static_cast<const lv_font_fmt_txt_dsc_t*>(font->dsc);
        if (descriptor->bitmap_format != LV_FONT_FMT_TXT_PLAIN || descriptor->bpp != 4) {
            return false;
        }
        double cursor = 0.0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            lv_font_glyph_dsc_t glyph{};
            if (!lv_font_get_glyph_dsc(font, &glyph, static_cast<unsigned char>(text[i]),
                                       i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1])
                                                           : 0)) {
                return false;
            }
            glyph.req_raw_bitmap = 1;
            const auto* data =
                static_cast<const std::uint8_t*>(font->get_glyph_bitmap(&glyph, nullptr));
            if (glyph.box_w != 0 && glyph.box_h != 0 && data != nullptr) {
                const double glyph_x = left + ((cursor + glyph.ofs_x) * scale_x);
                const double glyph_y = baseline - ((glyph.box_h + glyph.ofs_y) * scale_y);
                const int first_column = static_cast<int>(std::floor(glyph_x)) - 1;
                const int last_column =
                    static_cast<int>(std::ceil(glyph_x + (glyph.box_w * scale_x)));
                const int first_row = static_cast<int>(std::floor(glyph_y)) - 1;
                const int last_row = static_cast<int>(std::ceil(glyph_y + (glyph.box_h * scale_y)));
                if (first_column < x_ || last_column >= x_ + kWidth || first_row < y_ ||
                    last_row >= y_ + kHeight) {
                    return false;
                }
                ink_top_ = std::min(ink_top_, first_row);
                ink_bottom_ = std::max(ink_bottom_, last_row);
                // Geometry/bounds keep their endpoint precision. Coverage only needs
                // eight bits; keep its hot loop in the target's single-precision FPU.
                const auto raster_x = static_cast<float>(glyph_x);
                const auto raster_y = static_cast<float>(glyph_y);
                const float inverse_x = 1.0F / static_cast<float>(scale_x);
                const float inverse_y = 1.0F / static_cast<float>(scale_y);
                const auto raster_weight = static_cast<float>(weight);
                for (int row = first_row; row <= last_row; ++row) {
                    const float sample_y = (static_cast<float>(row) - raster_y) * inverse_y;
                    const int sample_row = static_cast<int>(std::floor(sample_y));
                    const float fraction_y = sample_y - static_cast<float>(sample_row);
                    for (int column = first_column; column <= last_column; ++column) {
                        const float sample_x = (static_cast<float>(column) - raster_x) * inverse_x;
                        const int sample_column = static_cast<int>(std::floor(sample_x));
                        const float fraction_x = sample_x - static_cast<float>(sample_column);
                        const float alpha =
                            (((pixel(data, glyph, sample_column, sample_row) *
                               (1.0F - fraction_x)) +
                              (pixel(data, glyph, sample_column + 1, sample_row) * fraction_x)) *
                             (1.0F - fraction_y)) +
                            (((pixel(data, glyph, sample_column, sample_row + 1) *
                               (1.0F - fraction_x)) +
                              (pixel(data, glyph, sample_column + 1, sample_row + 1) *
                               fraction_x)) *
                             fraction_y);
                        auto& destination =
                            mask_[static_cast<std::size_t>(((row - y_) * kWidth) + column - x_)];
                        destination = static_cast<std::uint8_t>(
                            std::min(255L, std::lround(static_cast<float>(destination) +
                                                       (alpha * raster_weight))));
                    }
                }
            }
            cursor += glyph.adv_w;
        }
        return true;
    }

    static void event(lv_event_t* event_ptr) {
        auto* self = static_cast<MotionTitleRaster*>(lv_event_get_user_data(event_ptr));
        if (lv_event_get_code(event_ptr) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
            if (self->active_) {
                lv_event_set_ext_draw_size(event_ptr, self->extension_);
            }
            return;
        }
        if (!self->active_) {
            return;
        }
        auto* label = static_cast<lv_obj_t*>(lv_event_get_target(event_ptr));
        lv_draw_label_dsc_t style;
        lv_draw_label_dsc_init(&style);
        lv_obj_init_draw_label_dsc(label, LV_PART_MAIN, &style);
        lv_draw_image_dsc_t draw;
        lv_draw_image_dsc_init(&draw);
        draw.src = &self->image_;
        draw.recolor = style.color;
        draw.recolor_opa = LV_OPA_COVER;
        draw.opa = style.opa;
        lv_area_t parent;
        lv_obj_get_coords(lv_obj_get_parent(label), &parent);
        const lv_area_t area{parent.x1 + self->x_, parent.y1 + self->y_,
                             parent.x1 + self->x_ + kWidth - 1, parent.y1 + self->y_ + kHeight - 1};
        lv_draw_image(lv_event_get_layer(event_ptr), &draw, &area);
        lv_event_stop_processing(event_ptr);
    }
};
} // namespace prd::ui::fonts
