// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/observation.hpp"

// The operational walk's fixture time base — Milestone 2 Phase G.1.
//
// WHY THIS EXISTS. A fixture is a script of events at fixed offsets from a base the caller
// chooses, and the walk replays one script per button press into the product's real store. The
// reducer clamps an event forward when `event.observed_at < previous.last_event_at` (§8.1), so a
// base behind anything the store has already seen does not merely reorder the script — it
// collapses it. Every offset lands on one instant, and a fixture whose whole point is a 30-second
// gap between an observation and its ageing arrives with no gap at all.
//
// That is exactly what the panel showed at E.6. The walk's bases were synthetic, `(index + 1) ×
// 100 s`, while a button gesture carries `platform::monotonic_time()` — real board uptime. Once
// uptime passed a fixture's base, its internal timeline was gone: `PrintingDegraded` sits at walk
// index 6, so it could only work in the first ~11.7 minutes after boot. The reviewer walked past
// three frames of ordinary Printing, and the confidence axis was unreachable on the device.
//
// TWO FLOORS, AND BOTH ARE NECESSARY.
//
//   1. The board's own clock. A gesture stamped with real uptime must never be ahead of the base
//      of the load it triggers, or the script is clamped to that gesture's instant.
//   2. The last event this walk emitted. Real time does not advance between two quick presses,
//      but the previous script's own offsets did — its last event can sit up to its full span
//      ahead of the clock. A base taken from the clock alone would then be behind it.
//
// So the base is the later of the two. Taking the clock alone fixes the case the panel showed and
// reintroduces the same collapse for anyone reviewing at speed; taking the emitted floor alone is
// the synthetic scheme that failed. Neither floor is redundant.
//
// PURE, AND HOST-COMPILABLE. No ESP-IDF, no sdkconfig gate, no clock read — `now` arrives as an
// argument, the way every timestamp in this project does. The walk itself cannot be compiled in
// the host build, so the rule lives here rather than inside the flow, and the replay guard drives
// this type rather than a copy of its arithmetic. Same reason `app_core/src/button_translation`
// is a translation unit of its own.

namespace prd::validation {

class WalkTimeBase {
  public:
    // The base the next fixture load shall use, given the board's clock now.
    [[nodiscard]] app_model::Timestamp base_for(app_model::Timestamp now) const;

    // Records one event the walk has emitted, so a later base clears it.
    void observe(app_model::Timestamp observed_at);

  private:
    // The earliest base a load may still use. Only ever advances.
    app_model::Timestamp earliest_{app_model::Timestamp::zero()};
};

} // namespace prd::validation
