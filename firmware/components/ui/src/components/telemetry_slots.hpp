// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// The Telemetry Column's slot identity — Production UI Specification §8.
//
// INTERNAL to the ui component, and deliberately in a header of its own with no LVGL
// include. The slots are a pure fact about the composition, and separating them from the
// widget lets the text bindings in text/scene_text.hpp be typed by slot and host-tested
// without compiling LVGL.

namespace prd::ui::component {

// The three slots the accepted artwork shows, in its order, top to bottom.
//
// Chamber temperature is deliberately absent: Architecture Spec §7.9 calls chamber
// values "optional and unused by the current MVP", §8 qualifies the row with "when
// available", and no artwork panel shows one. A fourth slot would reserve a row for data
// the MVP never receives.
enum class TelemetrySlot : std::uint8_t {
    Nozzle = 0,
    Bed = 1,
    Layer = 2,
};

inline constexpr std::size_t kTelemetrySlotCount = 3;

// §7 presence, for one row — UI Design Decision 004.
//
// "A component is present if and only if: 1. the machine supplies its value; and 2. that value
// is true of the machine now… No component shall be retained empty to imply anything."
//
// THE LABEL IS NOT A COMPONENT OF ITS OWN. It names the row rather than carrying data, so it
// has nothing to be present or absent about independently: it is part of the row, and the row
// is present exactly when its value is. A label left standing over no value is a component
// retained empty, and what it implies — that this machine has a nozzle whose temperature is
// merely unreported — is precisely the implication §7 prohibits.
//
// A PREDICATE RATHER THAN A CONDITION AT THE CALL SITE, and pure rather than inside the widget,
// so the rule can be host-tested against the real formatters. The widget is LVGL and cannot be
// compiled in the host build; this is the whole of the decision it makes.
//
// The value arrives already formatted, and the formatters return an empty text for an absent
// value (formatters.cpp: an unknown or non-finite temperature, an unknown or total-less layer
// count). So "the machine supplied it" and "the formatted value is non-empty" are the same
// fact, and the row rule needs no second source of truth.
[[nodiscard]] constexpr bool row_is_present(const std::string_view value) {
    return !value.empty();
}

} // namespace prd::ui::component
