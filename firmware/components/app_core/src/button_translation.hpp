// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>

#include "app_model/events.hpp"
#include "button/button_event.hpp"

// Physical gesture to application fact.
//
// This is the whole of the boundary Architecture Spec §5.6 draws: "Exact product
// actions remain in app_store or app_core, not in the driver." The button reports
// what the finger did; this maps that onto the vocabulary the reducer understands;
// the reducer decides what it means. Three layers, and the driver stays ignorant of
// all product meaning — which is what lets the same driver serve wake, Settings and
// factory reset without knowing any of them exist.
//
// Deliberately in an internal header with no ESP-IDF include, so the host suite
// compiles the same source the firmware does. Both enums are pure.

namespace prd::app_core {

// Returns nullopt for button::Event::None, which is not a gesture: it means "this
// observation produced no event". Mapping it to some gesture would invent an input
// nobody made.
[[nodiscard]] std::optional<app_model::ButtonGesture> to_gesture(button::Event event);

} // namespace prd::app_core
