// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// Alternative diagnostic producer selected by CMake, never linked with access_review.cpp.
// Synthetic model observations, NOT a captured printer response or decoder test.
#include "access_review.hpp"

#include <array>
#include <chrono>
#include <string>

#include "app_model/presentation.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fake_source/observations.hpp"
#include "fake_source/scenarios.hpp"

namespace prd::validation {
namespace {
using namespace std::chrono_literals;
using app_model::Event;
using app_model::PrinterState;
constexpr std::array<PrinterState, AccessReview::kStepCount> kStates{
    PrinterState::Attention, PrinterState::Printing, PrinterState::Attention,
    PrinterState::Printing,  PrinterState::Paused,   PrinterState::Attention,
    PrinterState::Attention, PrinterState::Paused,   PrinterState::Ready,
    PrinterState::Ready};
} // namespace

void AccessReview::begin(app_store::Store& store, const app_model::Timestamp now) {
    if (started_) {
        return;
    }
    started_ = true;
    for (std::size_t index = 0; index < 6; ++index) {
        auto event =
            fake_source::step_at(fake_source::Scenario::ColdBootToReady, index, now, 0).event;
        event.observed_at = now;
        (void)store.dispatch(event);
    }
    publish(store, now);
}

void AccessReview::ready(app_store::Store& store, const app_model::Timestamp now) {
    (void)store.dispatch(Event::connection_observed(
        now, fake_source::connection(app_model::ConnectionPhase::Connected)));
    auto snapshot = fake_source::snapshot(kStates.at(step_), sequence_++);
    if (step_ == 0) {
        snapshot.nozzle_current = {};
        snapshot.nozzle_target = {};
        snapshot.bed_current = {};
        snapshot.bed_target = {};
    }
    (void)store.dispatch(Event::printer_snapshot(now, snapshot));
    if (step_ > 0 && step_ < 8) {
        auto job = fake_source::job(sequence_++, {78, 0});
        // The owner's beta printer does not supply layers. Keep the panel instrument
        // faithful to that missing field instead of displaying the generic fixture's data.
        job.layers = {};
        (void)store.dispatch(Event::job_observed(now, job));
    }
}

void AccessReview::publish(app_store::Store& store, const app_model::Timestamp now) {
    ready(store, now);
    const auto view = app_model::project(store.state());
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kReview,
                     "printer-attention step=" + std::to_string(step_ + 1) +
                         "/10 subject=" + std::string{app_model::log_label(view.subject)} +
                         " condition=" + std::string{app_model::log_label(view.subject_condition)});
    next_ = now + (step_ == 5 ? 38s : 18s);
}

void AccessReview::tick(app_store::Store& store, const app_model::Timestamp now) {
    if (!started_) {
        begin(store, now);
        return;
    }
    (void)store.dispatch(Event::confidence_aged(now));
    if (step_ + 1 < kStepCount && now >= next_) {
        ++step_;
        publish(store, now);
    } else if (step_ != 5 && now - store.state().printer.observed_at >= 5s) {
        ready(store, now);
    }
}

} // namespace prd::validation
