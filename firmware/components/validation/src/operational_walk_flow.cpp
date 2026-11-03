// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h FIRST, and explicitly. The CONFIG_* macro this file is gated on comes from
// there, and a #if on an undefined macro is silently false: without this include the whole
// flow below compiles to empty stubs and nothing warns.
#include "sdkconfig.h"

#include "validation/validation.hpp"

// The operational family walk — Milestone 2 Phase D.
//
// Holds ONE operational state on the panel until the button advances, so a composition can be
// judged by looking at it rather than by racing the scenario sweep's schedule.
//
// It drives the real store through the real fixtures, which are lists of real events folded
// through the real reducer. Nothing here assembles a presentation: what is held is a state the
// product can actually reach.

#if defined(CONFIG_PRD_VALIDATION_OPERATIONAL_WALK)

#if defined(CONFIG_PRD_VALIDATION_SCENARIOS)
#error                                                                                             \
    "PRD_VALIDATION_OPERATIONAL_WALK and PRD_VALIDATION_SCENARIOS both dispatch into the same store; select one"
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "app_model/presentation.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fake_source/fixtures.hpp"
#include "ui/ui.hpp"
#include "walk_time_base.hpp"

namespace prd::validation {
namespace {

// The six states Production UI §9's operational family covers, in the order the accepted
// continuity artwork walks them — resting, warming, running, suspended, finished, interrupted.
// Reviewing them in that order is what makes the family read as one flow rather than six
// screens.
//
// THE LAST THREE ARE NOT PART OF THAT FLOW, and they are appended rather than interleaved for
// exactly that reason. Two are Printing at Printing's own energy — one at reduced confidence
// (UIDD 004 §10, orthogonal to energy) and one with no job record to draw (§3.2) — and one is
// Ready at Ready's, again at reduced confidence. They are here because their compositions have
// never been seen on the panel and no other fixture reaches them, not because the family has
// nine members. Walking the first six still reads as one object at six energies; the last three
// are checked afterwards, as the separate questions they are.
//
// Their order is the order Phase G.6 puts them on its agenda: degraded Printing, degraded Ready,
// then Printing without a record. The two confidence compositions sit together so the reviewer
// can compare them at the axis's two ends with one press between them.
constexpr std::array<fake_source::Fixture, 9> kWalk{
    fake_source::Fixture::Ready,
    fake_source::Fixture::Preparing,
    fake_source::Fixture::Printing,
    fake_source::Fixture::Paused,
    fake_source::Fixture::Completed,
    fake_source::Fixture::Stopped,
    fake_source::Fixture::PrintingDegraded,
    fake_source::Fixture::ReadyDegraded,
    fake_source::Fixture::PrintingUnknownProgress,
};

// Where each load's base comes from. See walk_time_base.hpp: the base is the later of the
// board's own clock and the last event this walk emitted, and neither floor is redundant.
//
// It replaced a synthetic stride of `(index + 1) × 100 s`, which put every fixture's base in the
// board's past within twelve minutes of boot and collapsed each script onto one instant from
// then on.
WalkTimeBase g_time_base;

std::size_t g_index = 0;
bool g_loaded = false;

// A drawn-form report owed for the load at `g_report_index`.
//
// WHY THE GEOMETRY CANNOT GO ON THE LOAD LINE. load() dispatches into the store; the store's
// subscriber projects and hands the presentation to the renderer; the render task draws on its
// own schedule. At the moment load() returns, the panel is still showing the state the walk has
// just replaced — so a geometry printed there would describe the previous form while naming the
// new state. That is precisely the class of lying diagnostic this project has already paid for
// twice.
//
// So the report is owed, not taken: collected on a later tick, once the renderer has been given
// something. Nothing waits and nothing blocks — this runs on the application task, which
// dispatches every event in the system, and a bench instrument does not get to hold it up.
bool g_report_pending = false;
std::size_t g_report_index = 0;
std::uint32_t g_applied_at_load = 0;
std::uint32_t g_redundant_at_load = 0;

// Written by the button task, read by the application task. A step count rather than a flag,
// so two presses between ticks are both honoured instead of one being swallowed.
//
// std::atomic rather than volatile. Milestone 2 Phase B established the same point for the
// button's pressed state: volatile orders nothing between tasks and guarantees no atomicity —
// it prevents the compiler caching the value in a register and stops there. The read-modify-
// write below genuinely crosses tasks, so it needs the real thing.
std::atomic<int> g_pending{0};
std::atomic<bool> g_restart{false};

void dispatch_into(app_store::Store& store, const app_model::Event& event) {
    (void)store.dispatch(event);
}

// Every event the walk emits passes through here, which is what lets the time base know how far
// the last script actually reached. Recording it in the sink rather than computing it from a
// per-fixture span means a fixture script can grow a longer tail without the walk being told.
void sink(const app_model::Event& event, void* const context) {
    g_time_base.observe(event.observed_at);
    dispatch_into(*static_cast<app_store::Store*>(context), event);
}

std::int64_t as_ms(const app_model::Timestamp timestamp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp).count();
}

// Applies the fixture at `g_index` and reports what the panel should now be showing.
//
// The log line is the review's own record: it names the state, what the composition should be
// drawing, and the values behind it, so a photograph can be checked against what the firmware
// believed it was showing rather than against memory.
void load(app_store::Store& store, const app_model::Timestamp now) {
    const auto fixture = kWalk[g_index];
    const auto base = g_time_base.base_for(now);

    // NO EXPLICIT REBOOT HERE, and that is worth stating because it looks like an omission.
    //
    // A fixture is a complete presentation built from a fresh device, and every fixture script
    // opens with its own boot prelude — including BootPhase::Starting, which the reducer treats
    // as the reboot it is: device conditions cleared, observation watermarks reset. So the
    // script reboots the model itself, and a second reboot here would be the same transition
    // dispatched twice.
    //
    // This mattered: before the reducer cleared the watermarks, replaying one fixture after
    // another left the second's observations rejected as stale, and the resulting mixture of
    // two fixtures resolved to no scene — a black panel. The guard is a host test that replays
    // every fixture after every other and requires each to reach its own state.
    fake_source::emit_script(fixture, base, sink, &store);
    g_loaded = true;

    const auto view = app_model::project(store.state());

    // `base` and `now` are both reported, because their relationship is the thing G.1 changed and
    // the only way to see from a capture which of the two floors was in force. base == now is a
    // reviewer who paused; base > now is one who pressed again inside the previous script's own
    // span. base < now would be the defect returning, and cannot happen by construction.
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "walk[" + std::to_string(g_index + 1) + "/" + std::to_string(kWalk.size()) +
                         "] " + std::string{fake_source::name(fixture)} + " base=" +
                         std::to_string(as_ms(base)) + "ms now=" + std::to_string(as_ms(now)) +
                         "ms subject=" + std::string{app_model::log_label(view.subject)} +
                         " confidence=" + std::string{app_model::log_label(view.confidence)} +
                         " form=" + (view.show_operational_form ? "yes" : "no") +
                         " telemetry=" + (view.show_telemetry ? "yes" : "no") +
                         " stale=" + (view.stale_indicator ? "yes" : "no"));

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "walk values printer=" + std::string{view.printer_name.view()} + " progress=" +
            (view.progress.known ? std::to_string(view.progress.percent) + "%" : std::string{"-"}) +
            " remaining=" +
            (view.remaining.known ? std::to_string(view.remaining.seconds) + "s"
                                  : std::string{"-"}) +
            " layer=" +
            (view.layers.known
                 ? std::to_string(view.layers.current) + "/" + std::to_string(view.layers.total)
                 : std::string{"-"}) +
            " nozzle=" +
            (view.nozzle.current.known
                 ? std::to_string(static_cast<int>(view.nozzle.current.celsius))
                 : std::string{"-"}) +
            " bed=" +
            (view.bed.current.known ? std::to_string(static_cast<int>(view.bed.current.celsius))
                                    : std::string{"-"}));

    // The renderer's counters as they stand before this state can possibly have been drawn.
    // What makes the report below a measurement rather than a guess is that it is not emitted
    // until one of them has moved.
    const auto scene = ui::scene_report();
    g_applied_at_load = scene.applied;
    g_redundant_at_load = scene.redundant;
    g_report_index = g_index;
    g_report_pending = true;
}

// Reports what the renderer actually drew for the state the walk last loaded.
//
// READ BACK, NEVER RECOMPUTED. Every figure comes from ui::operational_form_report(), which is
// the renderer's own account of the form it placed on the panel. The walk could not derive them
// in any case — the energy axis and the form derivation are private to the ui component, which
// is what UI Design Decision 004's Scope requires — and if it could, the line would be a
// prediction of what should have been drawn, printed where a reviewer reads a measurement.
//
// It is the walk index that makes this worth a line of its own. The renderer already logs the
// same geometry under PRD_UI_RENDER_REPORT, but that line names no state, so in a capture it is
// matched to a photograph by adjacency among the telemetry, flush and stack lines. This one
// names both.
// A colour in the form a reviewer can compare against the palette, and against the accent this
// same walk reported for a neighbouring state. Decimal would make the one reported value that
// cannot be judged from a photograph also the one that cannot be read from the log.
// Digits computed rather than tabled, matching the copy in ui's scenes/operational_scene.cpp —
// see the note there. The two are deliberately duplicated: both are diagnostic-only, they sit in
// different components, and the alternative is adding a log formatter to a public interface.
[[nodiscard]] std::string hex_colour(const std::uint32_t rgb) {
    constexpr std::int32_t kBitsPerDigit = 4;
    constexpr std::int32_t kHighestDigitShift = 20;
    constexpr std::uint32_t kNibbleMask = 0xFU;
    constexpr std::uint32_t kDecimalDigits = 10U;

    std::string text = "#";
    for (std::int32_t shift = kHighestDigitShift; shift >= 0; shift -= kBitsPerDigit) {
        const std::uint32_t nibble = (rgb >> static_cast<std::uint32_t>(shift)) & kNibbleMask;
        const std::uint32_t base = nibble < kDecimalDigits
                                       ? static_cast<std::uint32_t>('0')
                                       : static_cast<std::uint32_t>('A') - kDecimalDigits;

        text += static_cast<char>(base + nibble);
    }

    return text;
}

void report_drawn() {
    // THE RENDERER MUST HOLD NOTHING OLDER, and this condition is not decoration.
    //
    // A fixture script dispatches many events, and each one submits a presentation. The render
    // task can take an intermediate — including the script's own boot prelude, which draws no
    // form at all — and be preempted before the final one arrives. The counter check below
    // would then be satisfied by a composition the walk never asked for, and the line would
    // report `form=absent` for a state that plainly has one.
    //
    // The intake counters close it exactly. Every submission is accounted for as
    // submitted = received + superseded + whatever is still queued, so an empty queue is
    // precisely received + superseded == submitted.
    const auto intake = ui::intake_statistics();
    if (intake.received + intake.superseded != intake.submitted) {
        return;
    }

    const auto scene = ui::scene_report();

    // Nothing has reached the composition yet. Checked on each tick rather than waited for.
    if (scene.applied == g_applied_at_load && scene.redundant == g_redundant_at_load) {
        return;
    }

    g_report_pending = false;

    const std::string prefix =
        "walk[" + std::to_string(g_report_index + 1) + "/" + std::to_string(kWalk.size()) + "] ";

    if (scene.applied == g_applied_at_load) {
        // The presentation was identical to the one already on the panel, so the controller
        // drew nothing at all. Stated rather than omitted: a line that simply fails to appear
        // is indistinguishable from a report that was lost.
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         prefix + "redraw skipped, presentation unchanged");
        return;
    }

    const auto form = ui::operational_form_report();

    if (!form.drawn) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         prefix + "drew form=absent");
        return;
    }

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        prefix + "drew energy=" + std::to_string(form.energy) +
            " r=" + std::to_string(form.radius) + " cy=" + std::to_string(form.centre_y) +
            " half=" + std::to_string(form.half_angle) + " extent=" + std::to_string(form.extent) +
            " rot=" + std::to_string(form.rotation) + " mid=" + std::to_string(form.midpoint_y) +
            " top=" + std::to_string(form.top_y) + " low=" + std::to_string(form.bottom_y) +
            " sag=" + std::to_string(form.sagitta) +
            // ⛔ THE HORIZONTAL AXIS, AND BOTH KINDS OF REACH. Without cx and hw a walk capture
            // cannot tell whether the settled horizon was drawn on the canvas centre or on the
            // ring's — the exact question the 2026-08-29 correction exists to answer, and one
            // every other field here is blind to. The two reaches are named apart because they
            // are different numbers: `reach` is the nominal path, `paint` is the pinned
            // renderer's own column bounds, and neither is an observation of the panel.
            " cx=" + std::to_string(form.centre_x) + " hw=" + std::to_string(form.half_width) +
            " reach=" + std::to_string(form.half_extent) + " nominal=" +
            std::to_string(form.nominal_left) + ".." + std::to_string(form.nominal_right) +
            " paint=" + std::to_string(form.paint_left) + ".." + std::to_string(form.paint_right) +
            " accent=" + hex_colour(form.accent) + " closed=" + (form.closed ? "yes" : "no") +
            " record=" + (form.draws_record ? "yes" : "no"));
}

} // namespace

bool walk_compiled() {
    return true;
}

void walk_begin(app_store::Store& store, const app_model::Timestamp now) {
    g_index = 0;
    load(store, now);
}

void walk_tick(app_store::Store& store, const app_model::Timestamp now) {
    // Collected before any new load is handled, so the report that is owed is always the one
    // for the state that was on the panel — a load in this same tick supersedes it, and the
    // superseded report is dropped rather than attributed to the wrong index.
    if (g_report_pending) {
        report_drawn();
    }

    if (g_restart.exchange(false)) {
        g_index = 0;
        load(store, now);
        return;
    }

    const int pending = g_pending.exchange(0);
    if (pending == 0) {
        if (!g_loaded) {
            load(store, now);
        }
        return;
    }

    // Wraps in both directions. A review goes back and forth between two states more often
    // than it walks the list once, and hitting a wall at either end would mean a reset.
    const auto count = static_cast<int>(kWalk.size());
    int next = static_cast<int>(g_index) + pending;
    while (next < 0) {
        next += count;
    }

    g_index = static_cast<std::size_t>(next % count);
    load(store, now);
}

void walk_on_gesture(const app_model::ButtonGesture gesture) {
    switch (gesture) {
    case app_model::ButtonGesture::ShortPress:
        g_pending.fetch_add(1);
        break;

    case app_model::ButtonGesture::LongPress:
        g_pending.fetch_sub(1);
        break;

    case app_model::ButtonGesture::VeryLongPress:
        g_restart.store(true);
        break;
    }
}

} // namespace prd::validation

#else // the walk is not compiled

namespace prd::validation {

bool walk_compiled() {
    return false;
}

void walk_begin(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}
void walk_tick(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}
void walk_on_gesture(app_model::ButtonGesture /*gesture*/) {}

} // namespace prd::validation

#endif
