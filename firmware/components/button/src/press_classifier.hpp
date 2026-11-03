// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>

#include "button/button_event.hpp"

// Debounce and press-duration classification (Architecture Spec §5.6).
//
// Deliberately free of ESP-IDF and of FreeRTOS so the whole state machine can be
// exercised by the host test suite against a fake clock. Bounce behaviour and
// threshold boundaries are correctness properties of this code; they should be
// protected by tests, not only by a finger on a bench.
//
// ---------------------------------------------------------------------------
// Debounce rule — settle, not first-edge
// ---------------------------------------------------------------------------
//
// A level change is believed only once the line has *held* the new level for the
// debounce interval. The naive alternative — accept the first edge, then ignore
// everything for 25 ms — has a dangerous failure mode on this board: a single
// noise glitch that returns to idle inside the window would leave the driver
// believing the button is down, and it would then invent a VeryLongPress eight
// seconds later. Settling cannot do that, because a level that returns to the
// stable value simply cancels the pending change.
//
// The press timestamp recorded is the *first* observation of the settled run,
// not the moment the debounce interval expired, so a measured duration is the
// physical duration and does not carry the debounce delay.
//
// ---------------------------------------------------------------------------
// Emission rule — which event, and when
// ---------------------------------------------------------------------------
//
//   released before `long_press`        -> ShortPress, on release
//   released at/after `long_press`      -> LongPress,  on release
//   held to `very_long_press`           -> VeryLongPress, immediately, while
//                                          still held; the following release
//                                          produces nothing
//
// Exactly one event per gesture, always. The two shorter gestures resolve on
// release because until the finger lifts they are indistinguishable from a
// longer one. The very-long gesture is the exception on purpose: the accepted
// design (§5.6) uses it for a factory reset with an on-screen confirmation, and
// a destructive gesture that gives no feedback until the user lets go is a trap.
// Reporting it at the threshold also guarantees LongPress is never emitted first
// and then contradicted.
//
// ---------------------------------------------------------------------------
// Observation source
// ---------------------------------------------------------------------------
//
// observe() takes the *current logical level*, not an edge. Callers feed it both
// interrupt-captured edges (which carry accurate timestamps) and periodic polls
// of the pin (which carry the truth). That split is what makes a lost interrupt
// harmless: an edge improves timing precision, a poll establishes reality.

namespace prd::button {

class PressClassifier {
  public:
    explicit PressClassifier(Thresholds thresholds);

    // Feeds one observation of the input line. `pressed` is the logical level,
    // already normalised for active-low wiring by the caller. `now` must not go
    // backwards. Returns the event this observation produced, if any.
    //
    // At most one event per call: a release resolves a gesture, and a very-long
    // crossing can only happen while still held, so the two cannot coincide.
    [[nodiscard]] Event observe(bool pressed, std::chrono::microseconds now);

    // Abandons any press in progress and any pending level change. A press that
    // was down across a reset produces no event when it is eventually released:
    // its start time is gone, so any duration would be invented.
    void reset();

    // Debounced level, not the raw pin.
    [[nodiscard]] bool is_pressed() const {
        return stable_pressed_;
    }

    // True while a level change is being settled.
    [[nodiscard]] bool has_pending_change() const {
        return candidate_valid_;
    }

  private:
    Thresholds thresholds_;

    bool stable_pressed_{false};

    // Pending level change being settled.
    bool candidate_valid_{false};
    bool candidate_pressed_{false};
    std::chrono::microseconds candidate_since_{0};

    // Start of the press currently held, in the same timebase as observe().
    std::chrono::microseconds press_started_{0};
    bool press_active_{false};

    // Set once VeryLongPress has been reported for the current press, so it is
    // reported once however long the button is then held.
    bool very_long_reported_{false};
};

} // namespace prd::button
