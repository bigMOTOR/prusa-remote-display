// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "gesture_policy.hpp"

namespace prd::validation {

GestureDisposition disposition_of(const app_model::ButtonGesture gesture,
                                  const bool timeline_active) {
    if (!timeline_active) {
        return GestureDisposition::ProductFact;
    }

    // Exhaustive over the gesture set with no permissive default, so a gesture added later cannot
    // reach the store while this instrument is compiled in without someone deciding that it may.
    switch (gesture) {
    case app_model::ButtonGesture::ShortPress:
    case app_model::ButtonGesture::LongPress:
    case app_model::ButtonGesture::VeryLongPress:
        return GestureDisposition::DiagnosticOnly;
    }

    return GestureDisposition::DiagnosticOnly;
}

} // namespace prd::validation
