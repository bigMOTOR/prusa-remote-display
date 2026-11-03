// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/display_policy.hpp"

#include "app_model/events.hpp"
#include "app_model/observation.hpp"
#include "app_store/store.hpp"
#include "button/button_event.hpp"

// Hardware-validation support flows.
//
// WHY THIS COMPONENT EXISTS. Every flow here used to live in app_core, which grew to
// roughly 880 lines of which about 750 were bring-up code compiled out of three
// profiles. The Milestone 1 architecture audit flagged that size, and it was
// blocking: app_core is the composition root (Architecture Spec §4.1, §5.3), and a
// composition root nobody can read is one nobody can safely change.
//
// The split is by responsibility, not by convenience. app_core starts services and
// connects boundaries. This component holds bench instruments: things that exist to
// make hardware observable, that are compiled only when a validation flag selects
// them, and that no product image contains.
//
// RULES THIS COMPONENT KEEPS:
//
//   - no product logic. Nothing here decides what the product does; the scenario
//     flow drives the real store through real events and reads the result, and that
//     is the closest it comes.
//   - no new architecture. The flows are the accepted Milestone 1 flows, moved.
//     Their display, backlight, button and LVGL behaviour is unchanged, because it
//     was signed off on the physical unit.
//   - compile-time gating stays strict. Every entry point below is a no-op when its
//     flag is off, so app_core calls them unconditionally and carries no #if of its
//     own — and the linker discards what nothing reaches.

namespace prd::validation {

// Dedicated diagnostic emitter only. Never changes Store power, activity or acknowledgement.
[[nodiscard]] app_model::display_policy::Output
access_review_output(app_model::DisplayPower power, app_model::display_policy::Tier tier,
                     const app_model::display_policy::Levels& levels,
                     const app_model::display_policy::Timings& timings);

// Milestone 1 Phase C/D panel instruments, and the Phase F renderer cross-check.
//
// Starts whichever of the two is compiled in; both may run together. Neither brings
// the panel up any more — product display and renderer initialisation is app_core's,
// per Architecture Spec §15.1 steps 8 and 14. Until Milestone 2 Phase C this function
// carried the precedence rule between two flows that each initialised the panel.
//
// No-op when neither is compiled.
void start_panel_flow();

// Whether the display pattern flows are compiled in.
//
// They write pixels straight to the panel and bypass LVGL, so the composition root
// must not also start the renderer: two writers to one panel interleave frames that
// neither can be held responsible for. app_core asks this instead of testing a
// Kconfig symbol itself, so the exclusivity rule stays in one place.
[[nodiscard]] bool display_patterns_own_panel();

// Milestone 1 Phase E gesture logging.
//
// Called by app_core's button consumer for every classified gesture, so that the
// bring-up log still exists without the button having two consumers draining one
// queue. Keeps its own running counters.
//
// No-op unless PRD_BUTTON_EVENT_LOG.
void log_button_gesture(button::Event event);

// The periodic untouched-line report: a validation run has to be able to show that
// nothing was classified while nobody touched the button.
void log_button_idle();

// Whether the Milestone 2 Phase B scenario flow is compiled in.
[[nodiscard]] bool scenarios_compiled();

// Milestone 2 Phase B fixture-driven scenario flow.
//
// NOTE THE SHAPE: this flow has no task of its own. It is *stepped* by app_core's
// application task, because that task is the store's single consumer (§10.2) and a
// second task dispatching into the same store would break the serialization the
// store's whole thread-safety argument rests on. The flow borrows app_core's store
// rather than owning one, so what it logs is the real authoritative state.
//
// begin() logs the fixture catalogue once — twenty-one presentations resolved on the
// device, which is the on-target counterpart of the host fixture suite — and then
// loads the first scenario.
//
// Both are no-ops unless PRD_VALIDATION_SCENARIOS.
void scenarios_begin(app_store::Store& store, app_model::Timestamp now);

// Advances whatever is due and logs any resulting change. Called on every pass of
// app_core's application task; `now` is that task's monotonic clock reading, so this
// flow reads no clock of its own.
void scenarios_tick(app_store::Store& store, app_model::Timestamp now);

// A bench control request from a physical gesture. Safe to call from another task:
// it records a request, and app_core's application task acts on it inside the next
// scenarios_tick().
//
// Short press advances one step, long press toggles pause, very long press restarts
// the scenario. The gesture is also posted into the store by app_core as the product
// fact it is; this is an additional use of the same press, and only in the
// validation profile.
//
// No-op unless PRD_VALIDATION_SCENARIOS.
void on_gesture(app_model::ButtonGesture gesture);

// --- The operational family walk (Milestone 2 Phase D) ---------------------
//
// Holds ONE operational state on the panel until the button advances. See the Kconfig help
// for why the scenario sweep is the wrong instrument for a composition review.
//
// Same shape as the scenario flow: no task of its own, stepped by app_core's application
// task, which is the store's only dispatcher.
[[nodiscard]] bool walk_compiled();
void walk_begin(app_store::Store& store, app_model::Timestamp now);
void walk_tick(app_store::Store& store, app_model::Timestamp now);
void walk_on_gesture(app_model::ButtonGesture gesture);

// --- The owner-paced brightness harness ------------------------------------
//
// Walks the candidate On ceilings, the candidate Dimmed light fractions and the production
// recession, holding EVERY step indefinitely until a short press advances exactly one.
//
// ⛔ WHY IT IS OWNER-PACED AND WHY THE PANEL CARRIES THE LABEL. The first instrument advanced
// itself every 8-15 seconds while chat named each step. The announcements arrived batched, the
// owner could not tell which level was on screen, and the run was ruled INVALID for selection.
// Chat timing is not a test oracle, so it is not part of this one.
//
// Same shape as the flows above: no task of its own, and it dispatches nothing. It does NOT drive
// the store, so the press keeps its full product meaning; this is an additional use of the same
// press, in the validation profile only.
//
// No-op unless PRD_VALIDATION_BRIGHTNESS_HARNESS.
[[nodiscard]] bool brightness_harness_compiled();

// Whether the harness is driving the backlight, in which case app_core's display policy must not.
// Same question — and same answer shape — as display_patterns_own_panel() above.
[[nodiscard]] bool brightness_harness_owns_panel();

void brightness_harness_begin(app_model::Timestamp now);

// Emits the settled row once an outstanding ramp has landed. Only the two recession transitions
// carry a fade; every other step settles at the instant of its press.
void brightness_harness_tick(app_model::Timestamp now);

void brightness_harness_on_gesture(app_model::ButtonGesture gesture, app_model::Timestamp now);

// --- The Printing telemetry timeline (Milestone 4) -------------------------
//
// Replays a print from JSON `GET /api/v1/status` bodies through the PRODUCTION decoder,
// normalizer, events, store, reducer, projection and UI. See the Kconfig help for what it is and,
// more importantly, what it is not: no socket, no second UI, and no physical acceptance of
// anything — the bodies are shaped to the official contract and are not captured from a printer.
//
// Same shape as the flows above: no task of its own, stepped by app_core's application task, which
// is the store's only dispatcher. It does not touch the backlight, because the Active level is one
// of the things a reviewer is here to look at and it must be the product's own.
//
// No-ops unless PRD_VALIDATION_PRINTING_TIMELINE.
[[nodiscard]] bool printing_timeline_compiled();

// ⛔ WHETHER A DIAGNOSTIC FLOW CONSUMES THIS GESTURE, so it must NOT be posted into the store as a
// product fact. True for every gesture when the Printing timeline is compiled in, and false
// always otherwise — see src/gesture_policy.hpp for why that flow is different from the other
// three, which deliberately keep the press's product meaning.
//
// app_core asks this instead of testing a Kconfig symbol itself, so the rule stays in one place
// and the composition root carries no #if of its own.
[[nodiscard]] bool gesture_is_consumed(app_model::ButtonGesture gesture);
void printing_timeline_begin(app_store::Store& store, app_model::Timestamp now);
void printing_timeline_tick(app_store::Store& store, app_model::Timestamp now);
void printing_timeline_on_gesture(app_model::ButtonGesture gesture);

} // namespace prd::validation
