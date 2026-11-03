// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "components/download_mark.hpp"

#include "theme/tokens.hpp"

namespace prd::ui::component {
bool DownloadMark::build(lv_obj_t* const parent, const theme::Rect& bounds,
                         const theme::Colour colour) {
    root_ = theme::bare_object(parent);
    if (root_ == nullptr) {
        return false;
    }
    theme::place(root_, bounds);
    colour_ = colour;
    width_ = bounds.width;
    height_ = bounds.height;
    level_ = theme::opacity(theme::Opacity::Full);
    // One custom-drawn object rather than a root plus three lv_line children. The production LVGL
    // pool is fixed at 64 KiB and the update scene exists beside every established scene.
    lv_obj_add_event_cb(root_, draw, LV_EVENT_DRAW_MAIN, this);
    return true;
}

void DownloadMark::show(const bool visible) {
    if (root_ == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DownloadMark::paint(const std::int32_t level) {
    if (root_ == nullptr || level_ == level) {
        return;
    }
    level_ = level;
    lv_obj_invalidate(root_);
}

void DownloadMark::draw(lv_event_t* const event) {
    auto* const self = static_cast<DownloadMark*>(lv_event_get_user_data(event));
    if (self == nullptr || self->root_ == nullptr || self->level_ <= 0) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(self->root_, &area);
    const auto centre_x = area.x1 + (self->width_ / 2);
    const auto arrow_top = area.y1 + (self->height_ / 8);
    const auto arrow_tip = area.y1 + ((self->height_ * 5) / 8);
    const auto head_half = self->width_ / 6;
    const auto head_rise = self->height_ / 6;
    const auto tray_y = area.y1 + ((self->height_ * 7) / 8);
    const auto tray_inset = self->width_ / 5;

    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.color = theme::colour(self->colour_);
    descriptor.width = theme::kStrokeStandard;
    descriptor.opa = static_cast<lv_opa_t>(self->level_);
    descriptor.round_start = true;
    descriptor.round_end = true;

    auto line = [&](const std::int32_t x1, const std::int32_t y1, const std::int32_t x2,
                    const std::int32_t y2) {
        descriptor.p1 = {x1, y1};
        descriptor.p2 = {x2, y2};
        lv_draw_line(lv_event_get_layer(event), &descriptor);
    };
    line(centre_x, arrow_top, centre_x, arrow_tip);
    line(centre_x - head_half, arrow_tip - head_rise, centre_x, arrow_tip);
    line(centre_x, arrow_tip, centre_x + head_half, arrow_tip - head_rise);
    line(area.x1 + tray_inset, tray_y, area.x1 + self->width_ - tray_inset, tray_y);
}

} // namespace prd::ui::component
