// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "button/button_event.hpp"

namespace prd::button {

std::string_view event_name(const Event event) {
    switch (event) {
    case Event::None:
        return "none";
    case Event::ShortPress:
        return "short-press";
    case Event::LongPress:
        return "long-press";
    case Event::VeryLongPress:
        return "very-long-press";
    }

    return "unknown";
}

} // namespace prd::button
