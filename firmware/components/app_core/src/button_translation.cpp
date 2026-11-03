// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "button_translation.hpp"

namespace prd::app_core {

std::optional<app_model::ButtonGesture> to_gesture(const button::Event event) {
    switch (event) {
    case button::Event::ShortPress:
        return app_model::ButtonGesture::ShortPress;

    case button::Event::LongPress:
        return app_model::ButtonGesture::LongPress;

    case button::Event::VeryLongPress:
        return app_model::ButtonGesture::VeryLongPress;

    case button::Event::None:
        break;
    }

    return std::nullopt;
}

} // namespace prd::app_core
