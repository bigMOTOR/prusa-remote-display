// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "button/button_event.hpp"

// Multifunction button input (Architecture Spec §5.6).
//
// Owns the GPIO input, its interrupt, debouncing and press-duration
// measurement, and publishes typed gestures. It owns no product meaning: it does
// not know about wake, setup, provisioning or factory reset, and it must not
// learn about them. Those belong in app_store or app_core per §5.6.
//
// Pin knowledge stays in board. This component contains no GPIO number and no
// polarity assumption; both arrive through board::button_pins().

namespace prd::button {

struct Config {
    // PROVISIONAL values — see Thresholds in button_event.hpp.
    Thresholds thresholds{};
};

// Configures the input and starts the classification task.
//
// Must be called only after normal application startup has completed. On this
// board the button is the BOOT button on a strapping pin, and the accepted
// initialisation order (Architecture Spec §12) places button setup after the
// board probe and the display. Configuring it as an input cannot affect
// strapping — that is sampled at reset — but the ordering is preserved so the
// pin is never touched while early boot is still deciding anything.
[[nodiscard]] bool init(const Config& config = Config{});

// Stops the task, detaches the interrupt and abandons any press in progress.
// A press that is physically held across deinit()/init() produces no gesture
// when it is finally released: its start time did not survive, so any duration
// would be fabricated.
void deinit();

[[nodiscard]] bool is_initialised();

// Blocks for up to `timeout_ms` waiting for the next classified gesture.
// Returns false on timeout, in which case `out` is untouched. A negative timeout
// waits indefinitely.
//
// Safe to call from any task, and safe across a concurrent deinit(): the event
// queue outlives every init()/deinit() cycle, so a reader blocked here is waiting
// rather than reading freed memory. deinit() posts a wake so a blocked reader
// returns false promptly instead of waiting out its timeout.
[[nodiscard]] bool wait_for_event(Event& out, int timeout_ms);

// Debounced level, as published by the classification task. Exposed for
// diagnostics and for the untouched-line check in hardware validation, not as a
// substitute for events.
//
// This reads a value the task publishes, not the classifier's own state: the
// classifier belongs to that task alone, and reading its fields from another one
// was both a data race and a use-after-free waiting for a deinit() to land between
// the null check and the read.
[[nodiscard]] bool is_pressed();

// Observability for the bounded queues. Both drop counters must stay at zero in
// normal operation; a non-zero value is a defect signal, not routine.
struct Statistics {
    // Edges captured by the interrupt handler.
    std::uint32_t raw_edges{0};

    // Edges the handler could not enqueue. Harmless to correctness — the task
    // re-reads the pin every iteration, so a lost edge costs at most one poll
    // interval of timing precision — but it means bounce outran the task.
    std::uint32_t raw_edges_dropped{0};

    // Classified gestures published.
    std::uint32_t events_emitted{0};

    // Classified gestures lost because the consumer stopped draining the queue.
    // See the overflow policy in button.cpp.
    std::uint32_t events_dropped{0};
};

[[nodiscard]] Statistics statistics();

} // namespace prd::button
