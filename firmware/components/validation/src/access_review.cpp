// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

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
using app_model::ConnectionPhase;
using app_model::Event;
constexpr std::array<ConnectionPhase, AccessReview::kStepCount> kPhases{
    ConnectionPhase::Probing,
    ConnectionPhase::Unauthorized,
    ConnectionPhase::Probing,
    ConnectionPhase::UnsupportedApi,
    ConnectionPhase::Probing,
    ConnectionPhase::PrinterUnavailable,
    ConnectionPhase::Probing,
    ConnectionPhase::Connected,
    ConnectionPhase::PrinterUnavailable,
    ConnectionPhase::Connected};
} // namespace

void AccessReview::begin(app_store::Store& store, const app_model::Timestamp now) {
    if (started_) {
        return;
    }
    started_ = true;
    // Existing boot/config/network facts, but deliberately no successful printer snapshot.
    // Stamp every fact at real dispatch time; never advance the model into the future.
    for (std::size_t index = 0; index < 6; ++index) {
        auto event =
            fake_source::step_at(fake_source::Scenario::ColdBootToReady, index, now, 0).event;
        event.observed_at = now;
        (void)store.dispatch(event);
    }
    publish(store, now);
}

void AccessReview::ready(app_store::Store& store, const app_model::Timestamp now) {
    (void)store.dispatch(Event::printer_snapshot(
        now, fake_source::snapshot(app_model::PrinterState::Ready, sequence_++)));
}

void AccessReview::publish(app_store::Store& store, const app_model::Timestamp now) {
    // Corrected configuration is an in-memory observation only, never a stored key edit.
    if (step_ == 2 || step_ == 4) {
        (void)store.dispatch(Event::configuration_observed(
            now, fake_source::valid_configuration(static_cast<std::uint32_t>(step_))));
    }
    (void)store.dispatch(
        Event::connection_observed(now, fake_source::connection(kPhases.at(step_))));
    if (step_ == kStepCount - 1) {
        ready(store, now);
    }
    const auto view = app_model::project(store.state());
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kReview,
                     "access step " + std::to_string(step_ + 1) + "/" + std::to_string(kStepCount) +
                         " phase=" + std::string{app_model::log_label(kPhases.at(step_))} +
                         " acquiring=" + (view.acquiring_connection ? "yes" : "no") +
                         " loss-generation=" + std::to_string(view.observation_lost_generation));
    next_ = now + (step_ == kStepCount - 1 ? 5s : 18s);
}

void AccessReview::tick(app_store::Store& store, const app_model::Timestamp now) {
    if (!started_) {
        begin(store, now);
        return;
    }
    (void)store.dispatch(Event::confidence_aged(now));
    if (now < next_) {
        return;
    }
    if (step_ + 1 < kStepCount) {
        ++step_;
        publish(store, now);
        return;
    }
    // Stable final source, not another job/connection or display wake. Normal sleep remains.
    ready(store, now);
    next_ = now + 5s;
}

} // namespace prd::validation
