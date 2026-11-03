// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly — see the note in ui_render_flow.cpp.
#include "sdkconfig.h"

#include "validation/validation.hpp"

// Milestone 1 Phase E gesture logging, moved out of app_core.
//
// What changed in the move, and why: in Milestone 1 this flow also owned button
// initialisation and drained the event queue itself, because it was the only consumer
// that existed. It is not any more — app_core consumes gestures as product input and
// translates them into application events. Two consumers draining one queue would
// mean each gesture reached only one of them, at random, which is exactly the class
// of bug the queue's overflow policy exists to make loud.
//
// So this is now a reporting hook: app_core calls it for every gesture it consumes,
// and the bring-up log is unchanged apart from no longer carrying a task-stack line
// that belonged to a task that no longer exists.

#if defined(CONFIG_PRD_BUTTON_EVENT_LOG)

#include <string>

#include "button/button.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

namespace prd::validation {
namespace {

// Running totals, so a bench observer can compare what they pressed against what the
// firmware classified. Touched only from app_core's button consumer task, which is
// the only caller.
unsigned g_short_presses = 0;
unsigned g_long_presses = 0;
unsigned g_very_long_presses = 0;
unsigned g_total = 0;

void log_button_statistics() {
    const auto stats = button::statistics();

    // Both drop counters must stay at zero. Logged at warning level when they are
    // not, so a bench run cannot pass while quietly losing input.
    const bool dropped = stats.raw_edges_dropped != 0 || stats.events_dropped != 0;

    diagnostics::log(dropped ? diagnostics::LogLevel::Warning : diagnostics::LogLevel::Info,
                     diagnostics::tag::kApp,
                     "button stats raw_edges=" + std::to_string(stats.raw_edges) +
                         " raw_dropped=" + std::to_string(stats.raw_edges_dropped) +
                         " events=" + std::to_string(stats.events_emitted) +
                         " events_dropped=" + std::to_string(stats.events_dropped));
}

} // namespace

void log_button_gesture(const button::Event event) {
    ++g_total;

    switch (event) {
    case button::Event::ShortPress:
        ++g_short_presses;
        break;
    case button::Event::LongPress:
        ++g_long_presses;
        break;
    case button::Event::VeryLongPress:
        ++g_very_long_presses;
        break;
    case button::Event::None:
        break;
    }

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "button event=" + std::string{button::event_name(event)} + " total=" +
                         std::to_string(g_total) + " short=" + std::to_string(g_short_presses) +
                         " long=" + std::to_string(g_long_presses) +
                         " very_long=" + std::to_string(g_very_long_presses));

    log_button_statistics();
}

void log_button_idle() {
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "button idle level=" + std::string{button::is_pressed() ? "pressed" : "released"} +
            " total=" + std::to_string(g_total));

    log_button_statistics();
}

} // namespace prd::validation

#else // gesture logging not compiled

namespace prd::validation {

void log_button_gesture(button::Event /*event*/) {}

void log_button_idle() {}

} // namespace prd::validation

#endif
