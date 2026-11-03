// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly: this translation unit is gated on a CONFIG_PRD_UI_*
// macro, and a #if on an undefined macro is silently false.
#include "sdkconfig.h"

#include "components/telemetry_column.hpp"
#include "components/telemetry_icons.hpp"

#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "text/scene_text.hpp"
#include "text/text.hpp"

namespace prd::ui::component {
namespace {

lv_image_dsc_t icon_descriptor(const std::uint8_t* alpha) {
    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_A8;
    image.header.w = theme::kTelemetryIconWidth;
    image.header.h = theme::kTelemetryIconHeight;
    image.header.stride = theme::kTelemetryIconWidth;
    image.data_size = theme::kTelemetryIconWidth * theme::kTelemetryIconHeight;
    image.data = alpha;
    return image;
}
const lv_image_dsc_t kNozzleIcon = icon_descriptor(icons::nozzle_a8);
const lv_image_dsc_t kBedIcon = icon_descriptor(icons::bed_a8);

// Owner-reviewed 2026-09-06: icons and temperature values share fixed left columns.
// The Layer slot keeps its full-width numeric capacity; no layer value is invented.

[[nodiscard]] std::size_t index_of(const TelemetrySlot slot) {
    return static_cast<std::size_t>(slot);
}

void set_hidden(lv_obj_t* const object, const bool hidden) {
    if (object == nullptr) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace

bool TelemetryColumn::build(lv_obj_t* const parent, const theme::Rect& region) {
    container_ = theme::bare_object(parent);
    if (container_ == nullptr) {
        return false;
    }

    theme::place(container_, region);

    for (std::size_t index = 0; index < kTelemetrySlotCount; ++index) {
        const std::int32_t row_top = static_cast<std::int32_t>(index) * theme::kTelemetryRowHeight;

        // The value and the label are placed inside the row's own box, so a row's
        // contents cannot drift into its neighbour and the slot boundaries are the
        // only thing that decides vertical position.
        //
        // The pair is vertically centred in the row: value on top at its §4 line
        // height, label directly beneath at its own, the whole block centred so the
        // three rows read as evenly spaced.
        const std::int32_t block_top = theme::kTelemetryFirstY - region.y + row_top;
        const bool temperature = index != index_of(TelemetrySlot::Layer);
        const std::int32_t value_x = temperature ? theme::kTelemetryValueX - region.x : 0;
        const std::int32_t value_width = region.width - value_x;

        lv_obj_t* const value = theme::bare_label(container_);
        if (value == nullptr) {
            return false;
        }

        // §8 Temperature Row's value takes the State title role — §4's largest role
        // below the primary value, which is the proportion the artwork shows against
        // its label.
        // `color.secondary`, not `color.primary`. §5 establishes hierarchy through luminance,
        // and the primary value in the centre has to be the single brightest thing on screen
        // for it to read as focal. With the telemetry values also at full primary, four
        // elements shared the top of the hierarchy and nothing dominated.
        theme::apply_text_style(value, theme::TextRole::StateTitle, theme::Colour::Secondary);
        lv_obj_set_width(value, value_width);
        lv_obj_set_style_text_align(value, temperature ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        lv_obj_set_pos(value, value_x, block_top);
        lv_label_set_text(value, "");

        lv_obj_t* const label = theme::bare_label(container_);
        if (label == nullptr) {
            return false;
        }

        // §4's Label role, at `opacity.medium`: §5 establishes hierarchy through
        // luminance, and the row's name is subordinate to its value.
        theme::apply_text_style(label, theme::TextRole::Label, theme::Colour::Tertiary,
                                theme::Opacity::Medium);
        lv_obj_set_width(label, value_width);
        lv_obj_set_style_text_align(label, temperature ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT,
                                    LV_PART_MAIN);
        lv_obj_set_pos(label, value_x, block_top + theme::kTelemetryLabelOffsetY);
        const std::string_view row_label = text::lookup(text::kTelemetrySlotLabels[index]);
        lv_label_set_text_fmt(label, "%.*s", static_cast<int>(row_label.size()), row_label.data());

        // BUILT ABSENT. No value has been supplied yet, so §7 says the row is not present —
        // label included. The first applied composition sets all three slots (the diff's `full`
        // forces nozzle, bed and layers), so this is the state for a few milliseconds at most;
        // it is the correct starting state rather than a guard against that ordering.
        set_hidden(value, true);
        set_hidden(label, true);

        lv_obj_t* icon = nullptr;
        if (index == index_of(TelemetrySlot::Nozzle) || index == index_of(TelemetrySlot::Bed)) {
            icon = lv_image_create(container_);
            if (icon == nullptr) {
                return false;
            }
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
            lv_image_set_src(icon,
                             index == index_of(TelemetrySlot::Nozzle) ? &kNozzleIcon : &kBedIcon);
            lv_obj_set_pos(icon, theme::kTelemetryIconX - region.x,
                           block_top + theme::kTelemetryIconOffsetY);
            lv_obj_set_style_image_recolor(icon, theme::colour(theme::Colour::Secondary),
                                           LV_PART_MAIN);
            lv_obj_set_style_image_recolor_opa(icon, theme::opacity(theme::Opacity::Full),
                                               LV_PART_MAIN);
            set_hidden(icon, true);
        }
        rows_[index] = Row{value, label, icon};
    }

    // §6 Geometry Rules forbid decorative geometry, so the dividers the artwork shows
    // between rows are deliberately not drawn: they separate rows that §8 already
    // separates by fixed position, and nothing in §8's component list defines them.
    // Their absence is a reported difference from the artwork, not an oversight.

    return true;
}

void TelemetryColumn::set_value(const TelemetrySlot slot, const std::string_view text) {
    const Row& row = rows_[index_of(slot)];
    if (row.value == nullptr || row.label == nullptr) {
        return;
    }

    // ONE DECISION, APPLIED TO BOTH HALVES. The value and its label cannot disagree because
    // there is a single boolean here and both objects are set from it — there is no code path
    // that hides one without the other. §7's rule itself is row_is_present(), which is pure and
    // host-tested against the real formatters; this function only carries it out.
    const bool present = row_is_present(text);

    set_hidden(row.value, !present);
    set_hidden(row.label, !present);
    set_hidden(row.icon, !present);

    // HIDDEN, NOT MOVED, AND THE SLOT IS NOT RECLAIMED. Both objects keep the positions build()
    // gave them, so an absent row leaves its slot empty and the rows below it stay exactly where
    // they were. §8 owns this component's row positions — "Rows maintain stable vertical
    // positions" — and §7 makes that negative space structural rather than something to fill.
    // Whether the column's remaining content should re-centre on the rows it still has is UIDD
    // 004 §6's question, deferred to the UI Freeze Review; nothing here anticipates it.

    if (!present) {
        // Cleared as well as hidden. A hidden label still reports its geometry, and the render
        // report measures ink: leaving the numeral behind would have the gauge describe text
        // that is not on the panel, which is the lying-diagnostic failure this component has
        // already produced once.
        lv_label_set_text(row.value, "");
        return;
    }

    // lv_label_set_text copies, so the caller's buffer does not have to outlive this call.
    lv_label_set_text_fmt(row.value, "%.*s", static_cast<int>(text.size()), text.data());
}

#if defined(CONFIG_PRD_UI_RENDER_REPORT)

void TelemetryColumn::log_geometry(const std::optional<scene::FormGeometry>& form) const {
    if (container_ == nullptr) {
        return;
    }

    lv_obj_update_layout(container_);

    for (std::size_t index = 0; index < kTelemetrySlotCount; ++index) {
        const Row& row = rows_[index];
        if (row.value == nullptr) {
            continue;
        }

        // Diagnostic bounds follow the actual per-slot alignment, not the old right-only layout.
        const std::int32_t ink_width = lv_obj_get_self_width(row.value);
        const std::int32_t box_left = lv_obj_get_x(container_) + lv_obj_get_x(row.value);
        const std::int32_t x = index == index_of(TelemetrySlot::Layer)
                                   ? box_left + lv_obj_get_width(row.value) - ink_width
                                   : box_left;
        const std::int32_t y = lv_obj_get_y(container_) + lv_obj_get_y(row.value);
        const std::int32_t h = lv_obj_get_height(row.value);

        // §7 presence, reported per row. The rule is invisible in a photograph of a state whose
        // rows all carry values, so the log is where a reviewer confirms it fired — and where
        // the pair can be seen holding together, since one flag covers both halves.
        const bool present = !lv_obj_has_flag(row.value, LV_OBJ_FLAG_HIDDEN);

        // Measured against the form that is actually drawn, not against the ring's fixed
        // circle. When no form is present there is nothing for the row to collide with — and
        // neither is there when the row itself is absent, whatever box it still reports.
        const theme::Rect ink{x, y, ink_width, h};
        const bool struck =
            present && form.has_value() && scene::crosses(*form, theme::kRingStroke, ink);

        diagnostics::log(struck ? diagnostics::LogLevel::Warning : diagnostics::LogLevel::Info,
                         diagnostics::tag::kUi,
                         "layout telemetry[" + std::to_string(index) +
                             "] present=" + (present ? "yes" : "no") + " x=" + std::to_string(x) +
                             " y=" + std::to_string(y) + " w=" + std::to_string(ink_width) +
                             " h=" + std::to_string(h) + " crosses=" + (struck ? "YES" : "no"));
    }
}

#endif // CONFIG_PRD_UI_RENDER_REPORT

void TelemetryColumn::apply_level(const std::int32_t level) {
    for (const Row& row : rows_) {
        // The tokens each element was built with, restated here so the scaling is against the
        // role's own step and not against whatever the last level left behind. Re-applying from
        // the base every time is what keeps the recession idempotent.
        theme::apply_text_level(row.value, theme::Opacity::Full, level);
        theme::apply_text_level(row.label, theme::Opacity::Medium, level);
        if (row.icon != nullptr) {
            lv_obj_set_style_image_opa(row.icon, static_cast<lv_opa_t>(level), LV_PART_MAIN);
        }
    }
}

void TelemetryColumn::set_visible(const bool visible) {
    if (container_ == nullptr) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    }
}

} // namespace prd::ui::component
