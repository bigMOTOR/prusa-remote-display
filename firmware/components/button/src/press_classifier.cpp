// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "press_classifier.hpp"

namespace prd::button {
namespace {

constexpr std::chrono::microseconds to_us(const std::chrono::milliseconds value) {
    return std::chrono::duration_cast<std::chrono::microseconds>(value);
}

} // namespace

PressClassifier::PressClassifier(const Thresholds thresholds) : thresholds_{thresholds} {}

Event PressClassifier::observe(const bool pressed, const std::chrono::microseconds now) {
    // --- Stage 1: settle the level ------------------------------------------
    bool committed = false;
    std::chrono::microseconds committed_at{0};

    if (pressed == stable_pressed_) {
        // The line is back where it was, so whatever change was pending was
        // bounce or a glitch. Cancelling here is what prevents a transient from
        // being mistaken for a real press.
        candidate_valid_ = false;
    } else if (!candidate_valid_ || candidate_pressed_ != pressed) {
        // A new change to settle. Timestamped now, so the recorded edge is the
        // first sighting of the new level rather than the end of the interval.
        candidate_valid_ = true;
        candidate_pressed_ = pressed;
        candidate_since_ = now;
    } else if (now - candidate_since_ >= to_us(thresholds_.debounce)) {
        // Held long enough to be believed. Repeated observations of the same
        // pending level do not restart the interval, so duplicate edges are
        // harmless.
        stable_pressed_ = pressed;
        candidate_valid_ = false;
        committed = true;
        committed_at = candidate_since_;
    }

    // --- Stage 2: classify --------------------------------------------------
    if (committed) {
        if (stable_pressed_) {
            press_started_ = committed_at;
            press_active_ = true;
            very_long_reported_ = false;
            return Event::None;
        }

        // Released.
        const bool was_active = press_active_;
        const bool already_reported = very_long_reported_;
        const std::chrono::microseconds duration = committed_at - press_started_;

        press_active_ = false;
        very_long_reported_ = false;

        if (!was_active) {
            // Released a press this classifier never saw begin — it started
            // before init() or before reset(). No duration exists, so no
            // gesture is invented.
            return Event::None;
        }

        if (already_reported) {
            // VeryLongPress was reported at the threshold; the release is the
            // tail of a gesture that has already been delivered.
            return Event::None;
        }

        return duration >= to_us(thresholds_.long_press) ? Event::LongPress : Event::ShortPress;
    }

    // --- Stage 3: thresholds crossed while still held ----------------------
    if (press_active_ && stable_pressed_ && !very_long_reported_ &&
        now - press_started_ >= to_us(thresholds_.very_long_press)) {
        very_long_reported_ = true;
        return Event::VeryLongPress;
    }

    return Event::None;
}

void PressClassifier::reset() {
    candidate_valid_ = false;
    press_active_ = false;
    very_long_reported_ = false;
    press_started_ = std::chrono::microseconds{0};

    // stable_pressed_ is deliberately left alone: it is a fact about the wire,
    // not part of the gesture in progress. The next observation reconciles it,
    // and press_active_ = false means a release seen after a reset cannot be
    // classified as a gesture.
}

} // namespace prd::button
