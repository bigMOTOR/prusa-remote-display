// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "components/status_message.hpp"

#include <string>

namespace prd::ui::component {

bool StatusMessage::build(lv_obj_t* const parent, const std::int32_t width) {
    if (parent == nullptr) {
        return false;
    }

    label_ = theme::bare_label(parent);
    if (label_ == nullptr) {
        return false;
    }

    // §8: "Uses Body Typography." Secondary is the caller's default until set() states otherwise;
    // building with a colour rather than leaving it unset means a message drawn before its first
    // set() call is still legible rather than whatever LVGL last used.
    theme::apply_text_style(label_, theme::TextRole::Body, theme::Colour::Secondary);

    // Wraps within the width it was given rather than growing the layout. §7's layout rules keep
    // components in stable positions, and a message that widened its parent would move everything
    // beside it.
    lv_obj_set_width(label_, width);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label_, "");

    lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;

    return true;
}

void StatusMessage::set(const std::string_view message, const theme::Colour token) {
    if (label_ == nullptr) {
        return;
    }

    if (message.empty()) {
        clear();
        return;
    }

    // The colour is re-applied with the role, not on its own: apply_text_style sets font, colour,
    // opacity, line height and tracking together, and a message that received only the colour
    // would be a screen-specific style that looked almost right.
    theme::apply_text_style(label_, theme::TextRole::Body, token);

    lv_label_set_text(label_, std::string{message}.c_str());
    lv_obj_remove_flag(label_, LV_OBJ_FLAG_HIDDEN);
    visible_ = true;
}

void StatusMessage::clear() {
    if (label_ == nullptr) {
        return;
    }

    // Overwritten, not merely hidden. A hidden label still owns its string, and this component is
    // used by a scene that also draws a credential — so "cleared" means the characters are gone,
    // on the same discipline the session rows follow.
    lv_label_set_text(label_, "");
    lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;
}

} // namespace prd::ui::component
