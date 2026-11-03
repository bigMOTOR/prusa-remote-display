// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly — see the note in ui_render_flow.cpp.
#include "sdkconfig.h"

#include "validation/validation.hpp"

// Milestone 2 Phase B fixture-driven scenario flow.
//
// What it is for: proving on the physical device that the application model, the
// reducer, the store and the projection behave as the host suite says they do, with
// no networking involved and nothing rendered. It logs the resolved presentation
// after every accepted mutation, so a bench observer reads the same state machine the
// tests assert against.
//
// It constructs no LVGL widget and no product scene. Scenes are Phase C.

#if defined(CONFIG_PRD_VALIDATION_SCENARIOS)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "app_model/presentation.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fake_source/fixtures.hpp"
#include "fake_source/scenarios.hpp"
#include "fake_source/source.hpp"

namespace prd::validation {
namespace {

// How long the final state of a scenario is held before the next one loads, so the
// terminal presentation is readable rather than flashing past.
constexpr std::int64_t kScenarioDwellMs = 6000;

// Bench control requested by a physical gesture. Written by app_core's button
// consumer task, read and cleared by the application task.
enum class Control : std::uint8_t {
    None,
    Step,
    TogglePause,
    Restart,
};

std::atomic<Control> g_control{Control::None};

// The source persists between ticks. std::optional rather than a reference member
// held in static storage: the store does not exist until app_core builds it, and an
// unbound source would be a null reference waiting to be used.
std::optional<fake_source::FakeSource> g_source;

const char* yes_no(const bool value) {
    return value ? "yes" : "no";
}

std::string presentation_line(const app_model::Presentation& view) {
    return std::string{" layer="} + std::string{app_model::log_label(view.layer)} +
           " subject=" + std::string{app_model::log_label(view.subject)} +
           " condition=" + std::string{app_model::log_label(view.subject_condition)} +
           " confidence=" + std::string{app_model::log_label(view.confidence)} +
           " access=" + std::string{app_model::log_label(view.access_condition)} +
           " modal=" + std::string{app_model::log_label(view.modal_surface)} +
           " form=" + yes_no(view.show_operational_form) +
           " telemetry=" + yes_no(view.show_telemetry) + " stale=" + yes_no(view.stale_indicator);
}

// The values a scene would draw, reported only when the projection says they may be
// presented. Absence is absence: a field that is not known simply does not appear,
// which is what a log has instead of a placeholder.
std::string values_line(const app_model::Presentation& view) {
    std::string line = " values";

    if (view.printer_name_known) {
        line += " printer=" + std::string{view.printer_name.view()};
    }
    if (view.job_name_known) {
        line += " job=" + std::string{view.job_name.view()};
    }
    if (view.progress.known) {
        line += " progress=" + std::to_string(view.progress.percent) + "%";
    }
    if (view.remaining.known) {
        line += " remaining=" + std::to_string(view.remaining.seconds) + "s";
        if (view.remaining_is_approximate) {
            line += "(approx)";
        }
    }
    if (view.layers.known) {
        line += " layer=" + std::to_string(view.layers.current) + "/" +
                std::to_string(view.layers.total);
    }
    if (view.nozzle.current.known) {
        line += " nozzle=" + std::to_string(static_cast<int>(view.nozzle.current.celsius));
    }
    if (view.bed.current.known) {
        line += " bed=" + std::to_string(static_cast<int>(view.bed.current.celsius));
    }
    if (view.alert.active) {
        line += " alert=" + std::string{app_model::log_label(view.alert.severity)} + ":" +
                std::string{view.alert.title.view()};
    }
    if (view.update_progress.known) {
        line += " update=" + std::to_string(view.update_progress.percent) + "%";
    }

    return line;
}

void log_state(const std::string& prefix, const app_model::AppState& state) {
    const app_model::Presentation view = app_model::project(state);

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     prefix + " rev=" + std::to_string(state.revision) + presentation_line(view));
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     prefix + values_line(view));
}

// The on-device counterpart of the host fixture suite: every accepted MVP
// presentation, resolved from real events through the real reducer, on the real
// target. If a fixture reaches a different presentation here than it does on the
// host, something is target-specific that should not be.
void log_fixture_catalogue() {
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "fixture catalogue: " + std::to_string(fake_source::kFixtureCount) +
                         " presentations");

    // Held in static storage rather than on the stack. state_for() already nests two
    // AppState copies inside the reducer, and a third on this frame — 2 kB each — is
    // what turns a comfortable task into a marginal one. Safe: this runs once, on the
    // application task, and nothing re-enters it.
    static app_model::AppState state;

    for (std::size_t index = 0; index < fake_source::kFixtureCount; ++index) {
        const auto fixture = fake_source::fixture_at(index);
        state = fake_source::state_for(fixture);
        const auto view = app_model::project(state);

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "fixture[" + std::to_string(index) +
                             "]=" + std::string{fake_source::name(fixture)} +
                             " rev=" + std::to_string(state.revision) + presentation_line(view));
    }
}

void log_scenario_loaded() {
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "scenario loaded=" + std::string{fake_source::name(g_source->scenario())} +
                         " gen=" + std::to_string(g_source->generation()) +
                         " steps=" + std::to_string(g_source->step_count()));
}

void apply_control(app_store::Store& store, const app_model::Timestamp now) {
    const Control control = g_control.exchange(Control::None, std::memory_order_relaxed);

    switch (control) {
    case Control::None:
        return;

    case Control::Step: {
        // Single-step ignores the schedule: the point of the control is to advance
        // when the observer says so, not when the clock does.
        const auto advance = g_source->advance();

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         std::string{"control=step applied="} + yes_no(advance.applied) +
                             " step=" + std::to_string(advance.step_index + 1) + "/" +
                             std::to_string(g_source->step_count()) + " \"" +
                             std::string{advance.label.view()} + "\"");

        if (advance.applied) {
            log_state("state", store.state());
        }
        return;
    }

    case Control::TogglePause:
        if (g_source->paused()) {
            g_source->resume();
        } else {
            g_source->pause();
        }
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         std::string{"control=pause paused="} + yes_no(g_source->paused()));
        return;

    case Control::Restart:
        g_source->reset(now);
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "control=restart gen=" + std::to_string(g_source->generation()));
        return;
    }
}

} // namespace

bool scenarios_compiled() {
    return true;
}

void scenarios_begin(app_store::Store& store, const app_model::Timestamp now) {
    log_fixture_catalogue();

    g_source.emplace(store, fake_source::scenario_at(0), now);
    log_scenario_loaded();

    // The scenario replays boot from its own first step, so the presentation drops
    // back to the lifecycle layer before resolving again. That is the flow being
    // honest about what it is doing rather than starting halfway through.
    log_state("initial", store.state());
}

void scenarios_tick(app_store::Store& store, const app_model::Timestamp now) {
    if (!g_source.has_value()) {
        return;
    }

    apply_control(store, now);

    // Captured before the sweep, because the sweep may load the next scenario and
    // these are what the applied steps belong to. Reading them afterwards would label
    // every step of the outgoing scenario with the incoming one's name and generation.
    const std::size_t first_step = g_source->step_index();
    const auto scenario = g_source->scenario();
    const auto base = g_source->base();
    const std::uint32_t sequence_base = g_source->sequence_base();
    const std::size_t step_count = g_source->step_count();

    // The sweep policy — what is due, the dwell at the end, and the move to the next
    // scenario — belongs to FakeSource, where it is host-tested. This flow only
    // reports what it did. The pause-at-a-scenario-boundary defect lived in exactly
    // the code that used to be here.
    const auto swept = g_source->sweep(now, std::chrono::milliseconds{kScenarioDwellMs});

    if (swept.steps_applied != 0) {
        // One line per step, so a step that changed nothing is still visible as a
        // step rather than silently absent.
        for (std::size_t offset = 0; offset < swept.steps_applied; ++offset) {
            const std::size_t index = first_step + offset;
            const auto step = fake_source::step_at(scenario, index, base, sequence_base);

            diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                             "step " + std::to_string(index + 1) + "/" +
                                 std::to_string(step_count) + " " +
                                 std::string{fake_source::name(scenario)} + " \"" +
                                 std::string{step.label.view()} + "\"");
        }

        log_state("state", store.state());
    }

    if (swept.just_finished) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "scenario complete=" + std::string{fake_source::name(scenario)} +
                             " steps_applied=" + std::to_string(g_source->steps_applied()) +
                             " rev=" + std::to_string(store.revision()));
    }

    if (swept.loaded_next) {
        log_scenario_loaded();
    }
}

void on_gesture(const app_model::ButtonGesture gesture) {
    switch (gesture) {
    case app_model::ButtonGesture::ShortPress:
        g_control.store(Control::Step, std::memory_order_relaxed);
        return;

    case app_model::ButtonGesture::LongPress:
        g_control.store(Control::TogglePause, std::memory_order_relaxed);
        return;

    case app_model::ButtonGesture::VeryLongPress:
        g_control.store(Control::Restart, std::memory_order_relaxed);
        return;
    }
}

} // namespace prd::validation

#else // the scenario flow is not compiled

namespace prd::validation {

bool scenarios_compiled() {
    return false;
}

void scenarios_begin(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}

void scenarios_tick(app_store::Store& /*store*/, app_model::Timestamp /*now*/) {}

void on_gesture(app_model::ButtonGesture /*gesture*/) {}

} // namespace prd::validation

#endif
