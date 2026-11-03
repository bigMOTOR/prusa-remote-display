// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>
#include <string_view>

// Typed button events (Architecture Spec §5.6).
//
// These name *physical gestures*, not product actions. What a short press does
// is decided in app_store or app_core; this component only reports what the
// finger did. Keeping it that way is what lets the same driver serve the wake,
// setup and factory-reset flows without knowing any of them exist.

namespace prd::button {

enum class Event {
    // No event was produced by this observation.
    None,

    // Press released before the long-press threshold.
    ShortPress,

    // Press released at or after the long-press threshold and before the
    // very-long threshold.
    LongPress,

    // Press held to the very-long threshold. Emitted at the moment the
    // threshold is crossed, while the button is still down — see the emission
    // rule in press_classifier.hpp.
    VeryLongPress,
};

[[nodiscard]] std::string_view event_name(Event event);

// Gesture thresholds.
//
// PROVISIONAL implementation values (Milestone 1 Phase E). No accepted Project
// Source specifies button timing, so these are engineering defaults chosen to be
// validated on hardware, not product behaviour. They must be reviewed when the
// UI behaviour that consumes them is designed.
struct Thresholds {
    // Minimum time the line must hold a new level before the change is
    // believed. C28 is not fitted on this board, so all debouncing is here.
    std::chrono::milliseconds debounce{25};

    std::chrono::milliseconds long_press{1500};

    std::chrono::milliseconds very_long_press{8000};
};

} // namespace prd::button
