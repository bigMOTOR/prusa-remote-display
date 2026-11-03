// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/events.hpp"

// What a diagnostic flow does to a button gesture's PRODUCT meaning.
//
// ⛔ THE PROBLEM THIS EXISTS FOR. The button task posts every classified gesture into the store as
// the product fact it is, and then hands the same gesture to whichever bench flow is compiled in.
// For the scenario sweep and the operational walk that is deliberate and documented: those flows
// only read the model, so a press keeps its full product meaning and what the model sees is
// unchanged by the bench control.
//
// ⛔ FOR THE PRINTING TIMELINE IT WAS A DEFECT, AND A DANGEROUS ONE. Its controls are not passive:
//
//   short press       stepped the timeline AND was the manual display-power toggle, so a reviewer
//                     advancing a step could put the panel to sleep — while judging brightness;
//   long press        paused the timeline AND is the physical-presence authorization that raises
//                     the privileged setup surface, i.e. reconfiguration;
//   very long press   restarted the timeline AND is the gesture the factory-reset flow is armed
//                     and confirmed by.
//
// A diagnostic control must not have a simultaneous product effect, and a bench gesture must never
// approach a destructive path. So a flow whose controls are ACTIONS consumes them: the gesture does
// not reach the store at all, and the reducer therefore never sees it.
//
// PURE, AND HOST-COMPILABLE — the same arrangement walk_time_base.hpp uses and for the same reason.
// The flow that owns the controls is behind a Kconfig gate the host suite cannot compile, so the
// POLICY lives here where every case is reachable by a test, and the gated wrapper only supplies
// the one boolean.

namespace prd::validation {

enum class GestureDisposition : std::uint8_t {
    // The gesture keeps its full product meaning. A diagnostic flow may additionally observe it.
    ProductFact,

    // A diagnostic flow consumes it. It must NOT be posted into the store, so it has no product
    // meaning at all — no wake, no sleep, no setup surface, no reset arming.
    DiagnosticOnly,
};

// Whether `gesture` reaches the store, given whether the Printing-timeline instrument is compiled.
//
// ⬅️ ALL THREE GESTURES, NOT JUST THE DANGEROUS ONE. Consuming only the very-long press would leave
// the short press able to sleep the panel mid-review and the long press able to open setup over a
// synthetic print. And a reviewer cannot be asked to remember which of three presses is safe.
//
// ⛔ THE PANEL IS NOT LEFT UNWAKEABLE BY THIS. The timeline publishes a reading every step, and a
// meaningful change wakes the panel through the ordinary display policy — which is the path a
// reviewer judging the Active level needs to be looking at anyway. What is removed is a stray tap's
// power to darken it.
[[nodiscard]] GestureDisposition disposition_of(app_model::ButtonGesture gesture,
                                                bool timeline_active);

} // namespace prd::validation
