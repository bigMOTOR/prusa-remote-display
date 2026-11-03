// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h FIRST, and explicitly. The CONFIG_* macro this file is gated on comes from there,
// and a #if on an undefined macro is silently false: without this include the whole flow below
// compiles to empty stubs and nothing warns.
#include "sdkconfig.h"

#include "validation/validation.hpp"

#if defined(CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW) &&                                         \
    !defined(CONFIG_PRD_VALIDATION_PRINTING_TIMELINE)
#error "PRINTING_LOSS_REVIEW requires PRINTING_TIMELINE"
#endif

// The Printing telemetry timeline — a fixture-driven run of the WHOLE production path, from a
// `GET /api/v1/status` response body to the panel, with no printer.
//
//   versioned JSON fixture
//     → prusalink::decode_status()        the production decoder
//     → prusalink::status_from()          the production normalizer, both halves at one order
//     → app_model::Event::printer_status  ONE event, so ONE mutation and ONE projection
//     → the real store and reducer        apply_status(), which settles the lifecycle last
//     → app_model::project()
//     → the production UI
//
// The middle three arrows are apply_status_body()'s, shared verbatim with the host replay suite and
// identical to what app_core::apply_poll_report() does with a real poll report.
//
// ⛔ EVERY ARROW AFTER THE FIRST IS PRODUCTION CODE. This flow contributes the bodies, the clock
// and the button; it decodes nothing itself, projects nothing itself and draws nothing itself. A
// simulator that owned any of those stages would be showing the reviewer its own work.
//
// ⛔ IT IS NOT A PRUSALINK SERVER AND OPENS NO SOCKET. The network stack, the worker task, the
// pacing and the HTTP client are all bypassed — this is the boundary below them. What it cannot
// therefore demonstrate is anything about the transport, and it does not claim to.
//
// ⛔ AND IT IS NOT PHYSICAL ACCEPTANCE OF ANYTHING. It exists so the composition, the transitions,
// the percentage, the remaining time, the temperature treatment and the Active brightness can be
// LOOKED AT on the real panel without a print running. What a reviewer decides from it is a design
// judgment on a real rendering; it is not evidence about a real printer, and the compatibility of
// the field set still rests on a future redacted capture.

#ifdef CONFIG_PRD_VALIDATION_PRINTING_TIMELINE

#ifdef CONFIG_PRD_VALIDATION_SCENARIOS
#error                                                                                             \
    "PRD_VALIDATION_PRINTING_TIMELINE and PRD_VALIDATION_SCENARIOS both dispatch into the same store; select one"
#endif

#ifdef CONFIG_PRD_VALIDATION_OPERATIONAL_WALK
#error                                                                                             \
    "PRD_VALIDATION_PRINTING_TIMELINE and PRD_VALIDATION_OPERATIONAL_WALK both dispatch into the same store; select one"
#endif

#ifdef CONFIG_PRD_VALIDATION_BRIGHTNESS_HARNESS
#error                                                                                             \
    "PRD_VALIDATION_PRINTING_TIMELINE needs the product display policy to own the backlight; the brightness harness takes it"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fake_source/scenarios.hpp"
#include "gesture_policy.hpp"
#include "prusalink/prusalink.hpp"
#include "status_timeline.hpp"
#include "ui/ui.hpp"
#include "walk_time_base.hpp"

#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
#include "loss_review.hpp"
#endif

// prusalink::log_label(DecodeError) comes from decode.hpp, a private header of that component
// reached through the src include path this component is compiled with. Nothing here calls the
// decoder — apply_status_body() does that — but a failed decode has to be nameable in the log.
#include "decode.hpp"

namespace prd::validation {
namespace {

// How long each step is HELD ON THE PANEL. Bench time, not printer time — the timeline's own
// offsets compress a 67-minute print into 96 seconds of stamps, and this decides how long a
// reviewer looks at each one.
//
// ⬅️ WHY IT AUTO-ADVANCES AT ALL, given that the operational walk deliberately does not. The walk
// answers "is this composition right", which is a question about one frame and needs no clock.
// This answers "does the product move through a print correctly", which is a question about a
// SEQUENCE — a reviewer holding every frame by hand cannot see whether the percentage climbs
// smoothly or the remaining time jumps. So it runs, and the button stops it: long press pauses,
// short press single-steps whether paused or not.
//
// ⛔ EIGHTEEN SECONDS, NOT THE 1500 ms THIS FIRST SHIPPED WITH. At 1500 ms the seventeen states
// went past in 25.5 seconds, which is not a review: nobody reads a percentage, a remaining time,
// two temperature pairs, a state word and a brightness level in a second and a half, let alone
// judges them. Eighteen seconds per state is about 5.1 minutes for the whole run.
//
// ⛔ AND IT IS UNDER THE QUIET RECESSION BOUNDARY ON PURPOSE. The universal recession takes the
// panel from Active to Quiet after 20 seconds without a meaningful change, and every step IS a
// meaningful change — so at 18 seconds each step re-arms that timer before it fires and the panel
// holds the Active level the reviewer is there to judge. A longer hold would dim the panel
// mid-state and put the brightness question and the telemetry question in each other's way.
constexpr std::chrono::milliseconds kHoldPerStep{18000};

static_assert(kHoldPerStep.count() < 20000,
              "a hold at or past the 20-second Quiet recession boundary would dim the panel "
              "mid-state, which is the one thing this instrument must not do to a brightness "
              "review");

// The prelude that makes the device operational at all: cold boot through hardware, storage, a
// valid configuration, boot complete, and a reachable configured printer.
//
// ⬅️ REUSED, NOT REWRITTEN. It is fake_source's own ColdBootToReady scenario, dispatched through
// the same step_at() the Phase B flow uses. A hand-written prelude here would be a second opinion
// about what a booted device looks like, and the two would drift.
constexpr auto kPrelude = fake_source::Scenario::ColdBootToReady;

WalkTimeBase g_time_base;

// The synthetic worker's lease. It advances on every restart, which is what makes a replay a
// genuine re-run rather than a rewind: snapshot_is_current() accepts a new generation's first
// observation whatever watermark the previous run reached. Same reasoning as
// fake_source::FakeSource::reset().
std::uint32_t g_generation = 1;
std::uint32_t g_sequence = 0;

std::size_t g_index = 0;
bool g_started = false;
bool g_paused = false;
app_model::Timestamp g_next_due{};

// Written by the button task, read by the application task. A step count rather than a flag, so
// two presses between ticks are both honoured. std::atomic rather than volatile, for the reason
// the operational walk records: volatile orders nothing between tasks.
std::atomic<int> g_pending{0};
std::atomic<bool> g_toggle_pause{false};
std::atomic<bool> g_restart{false};

#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
LossReview g_loss_review;
bool g_loss_review_started = false;
std::uint32_t g_review_controls = 0;
#endif

std::int64_t as_ms(const app_model::Timestamp timestamp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp).count();
}

// Only the prelude dispatches raw events; every status body goes through apply_status_body().
void dispatch_into(app_store::Store& store, const app_model::Event& event) {
    g_time_base.observe(event.observed_at);
    (void)store.dispatch(event);
}

// A value, or a dash. The dash is load-bearing: "the printer did not report this" and "the printer
// reported zero" are different facts, and a log that printed 0 for both would make the one
// distinction this checkpoint is about invisible in the record.
std::string known_or_dash(const bool known, const std::uint32_t value, const char* const unit) {
    return known ? std::to_string(value) + unit : std::string{"-"};
}

// Runs the prelude, in one go, at `base`. It is dispatched whole rather than paced: nobody is
// reviewing a boot sequence here, and holding each of its eight steps for a second and a half
// would put twelve seconds of setup in front of every run.
void emit_prelude(app_store::Store& store, const app_model::Timestamp base) {
    const std::size_t count = fake_source::step_count(kPrelude);

    for (std::size_t index = 0; index < count; ++index) {
        const fake_source::Step step = fake_source::step_at(kPrelude, index, base, 0);
        dispatch_into(store, step.event);
    }

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "timeline prelude " + std::string{fake_source::name(kPrelude)} + " steps=" +
                         std::to_string(count) + " base=" + std::to_string(as_ms(base)) + "ms");
}

// Applies one body and reports what the panel should now be showing.
//
// ⛔ IT DECODES AND DISPATCHES NOTHING ITSELF. apply_status_body() — the shared, host-compiled
// production seam — does both, and app_core's real report ingest reaches the identical
// status_from() + Event::printer_status pair. This function contributes the body, the timestamp
// and the log line, and that is all it may contribute: an instrument that rebuilt the wiring would
// be showing the reviewer its own work rather than the product's.
void emit_status(app_store::Store& store, const StatusStep& step, const app_model::Timestamp now) {
    // The paced flow observes at dispatch time, not the compressed fixture's offset.
    // A manual step or restart may follow a synthetic prelude ahead of the clock; never rewind
    // the store, but do not add a fresh artificial gap to every status observation either.
    const app_model::Timestamp moment = std::max(now, store.state().last_event_at);

    const app_model::SnapshotOrder order{g_generation, g_sequence};
    ++g_sequence;

    const StatusApplication applied = apply_status_body(store, step.body, moment, order);
    g_time_base.observe(moment);

    if (!applied.dispatched) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "timeline[" + std::to_string(g_index + 1) +
                             "] decode=" + std::string{prusalink::log_label(applied.decoded)} +
                             " — nothing dispatched");
        return;
    }

    const auto view = app_model::project(store.state());

    // ⛔ ONE ROW PER STEP, WITH EVERYTHING THE REVIEW NEEDS ON IT. It used to be two lines, which
    // in a capture interleave with the printer, display and stack lines and have to be paired up
    // by adjacency — and during a live review they arrive as two notifications for one state.
    // `step N/17` leads so the row is unmistakable at a glance, and every projected value the
    // reviewer is judging sits on the same line as the label of the state it belongs to.
    //
    // ⬅️ A DASH IS NOT A ZERO. "The printer did not report this" and "the printer reported zero"
    // are different facts, and this row is where a reviewer reads which one is on the panel.
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        (g_index >= status_timeline_step_count() ? "refresh " : "step ") +
            std::to_string(std::min(g_index + 1, status_timeline_step_count())) + "/" +
            std::to_string(status_timeline_step_count()) + " " + std::string{step.label} +
            " | subject=" + std::string{app_model::log_label(view.subject)} +
            " progress=" + known_or_dash(view.progress.known, view.progress.percent, "%") +
            " remaining=" + known_or_dash(view.remaining.known, view.remaining.seconds, "s") +
            " elapsed=" + known_or_dash(view.elapsed.known, view.elapsed.seconds, "s") +
            " nozzle=" +
            known_or_dash(view.nozzle.current.known,
                          static_cast<std::uint32_t>(view.nozzle.current.celsius), "") +
            "/" +
            known_or_dash(view.nozzle.target.known,
                          static_cast<std::uint32_t>(view.nozzle.target.celsius), "C") +
            " bed=" +
            known_or_dash(view.bed.current.known,
                          static_cast<std::uint32_t>(view.bed.current.celsius), "") +
            "/" +
            known_or_dash(view.bed.target.known,
                          static_cast<std::uint32_t>(view.bed.target.celsius), "C") +
            " ring=" + (view.progress.known ? "yes" : "no") +
            " job=" + (applied.job_reported ? "yes" : "no") + " completed=" +
            (store.state().last_completed_job.present
                 ? std::to_string(store.state().last_completed_job.progress.percent) + "%"
                 : std::string{"-"}) +
            " changed=" + (applied.mutated ? "yes" : "no"));
}

// Only the synthetic boot prelude uses this base. Paced status observations use dispatch time.
// Gesture restart retains the emitted-time floor; begin() explicitly resets it for a new run.
// See walk_time_base.hpp for the floor's relationship to the board clock and previous events.
app_model::Timestamp g_base{};

void restart(app_store::Store& store, const app_model::Timestamp now) {
    ++g_generation;
    g_sequence = 0;
    g_index = 0;
    g_paused = false;
    g_base = g_time_base.base_for(now);
#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
    g_loss_review = LossReview{};
    g_loss_review_started = false;
    g_review_controls = 0;
#endif

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "timeline restart generation=" + std::to_string(g_generation) +
                         " provenance=" + std::string{kStatusTimelineProvenance});

    emit_prelude(store, g_base);
    g_next_due = now;
    g_started = true;
}

// Applies the step at g_index and schedules the next.
void apply_step(app_store::Store& store, const app_model::Timestamp now,
                [[maybe_unused]] const bool manual = false) {
#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
    if (manual)
        ++g_review_controls;
#endif
    if (g_index >= status_timeline_step_count()) {
#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
        if (!g_loss_review_started) {
            g_loss_review.begin(store, now, {g_generation, g_sequence}, g_review_controls);
            g_loss_review_started = true;
        } else if (manual) {
            g_loss_review.skip(store, now);
        } else {
            g_loss_review.tick(store, now);
        }
        g_sequence = g_loss_review.next_sequence();
        g_next_due = now; // The existing app task cadence advances the chapter; no new task.
        return;
#endif
        // Refresh the connected synthetic printer, not a new timeline step. Production
        // freshness policy is unchanged and an explicit pause still permits age tests.
        emit_status(store, status_timeline_step_at(status_timeline_step_count() - 1U), now);
        g_next_due = now + std::chrono::seconds{5};
        return;
    }

    emit_status(store, status_timeline_step_at(g_index), now);
    ++g_index;
    g_next_due = now + std::chrono::duration_cast<app_model::Timestamp>(kHoldPerStep);

    if (g_index >= status_timeline_step_count()) {
        g_next_due = now + std::chrono::seconds{5};
        // Print-only mode retains its final Ready refresh. The optional loss review continues
        // after this print chapter and reports its own completion, never a physical PASS.
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "timeline complete steps=" + std::to_string(status_timeline_step_count()) +
                             " — long press pauses, very long press restarts");
#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
        diagnostics::log(
            diagnostics::LogLevel::Info, diagnostics::tag::kReview,
            "printing chapter ended; loss-review follows after 5s, not full completion");
#endif
    }
}

} // namespace

bool printing_timeline_compiled() {
    return true;
}

bool gesture_is_consumed(const app_model::ButtonGesture gesture) {
    return disposition_of(gesture, true) == GestureDisposition::DiagnosticOnly;
}

void printing_timeline_begin(app_store::Store& store, const app_model::Timestamp now) {
    g_time_base = WalkTimeBase{};
    g_pending.store(0);
    g_toggle_pause.store(false);
    g_restart.store(false);
    g_generation = 0;
    restart(store, now);
}

void printing_timeline_tick(app_store::Store& store, const app_model::Timestamp now) {
    if (!g_started) {
        restart(store, now);
        return;
    }

    if (g_restart.exchange(false)) {
        restart(store, now);
        return;
    }

    if (g_toggle_pause.exchange(false)) {
        g_paused = !g_paused;
#ifdef CONFIG_PRD_VALIDATION_PRINTING_LOSS_REVIEW
        ++g_review_controls;
        if (g_loss_review_started)
            g_loss_review.note_manual_control();
#endif
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         std::string{"timeline "} + (g_paused ? "paused" : "resumed") +
                             " at step " + std::to_string(g_index) + "/" +
                             std::to_string(status_timeline_step_count()));
    }

    // A single step ignores both the pause and the schedule: that is what the control is for.
    const int pending = g_pending.exchange(0);
    if (pending > 0) {
        for (int applied = 0; applied < pending; ++applied) {
            apply_step(store, now, true);
        }
        return;
    }

    if (g_paused || now < g_next_due) {
        return;
    }

    apply_step(store, now);
}

void printing_timeline_on_gesture(const app_model::ButtonGesture gesture) {
    switch (gesture) {
    case app_model::ButtonGesture::ShortPress:
        g_pending.fetch_add(1);
        break;

    case app_model::ButtonGesture::LongPress:
        g_toggle_pause.store(true);
        break;

    case app_model::ButtonGesture::VeryLongPress:
        g_restart.store(true);
        break;
    }
}

} // namespace prd::validation

#else // the timeline is not compiled

#include "gesture_policy.hpp"

namespace prd::validation {

bool printing_timeline_compiled() {
    return false;
}

bool gesture_is_consumed(const app_model::ButtonGesture gesture) {
    return disposition_of(gesture, false) == GestureDisposition::DiagnosticOnly;
}

void printing_timeline_begin(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}
void printing_timeline_tick(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}
void printing_timeline_on_gesture(app_model::ButtonGesture /*gesture*/) {}

} // namespace prd::validation

#endif
