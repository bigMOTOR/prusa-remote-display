// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fake_source/fixtures.hpp"

#include <array>
#include <chrono>
#include <cstdint>

#include "app_model/reducer.hpp"
#include "fake_source/observations.hpp"

namespace prd::fake_source {
namespace {

using app_model::Event;
using app_model::Timestamp;

// One schedule for every fixture, so an offset means the same thing everywhere and
// two fixtures built from the same prelude produce the same timestamps for it.
constexpr std::int64_t kStartingMs = 0;
constexpr std::int64_t kHardwareMs = 10;
constexpr std::int64_t kStorageMs = 20;
constexpr std::int64_t kConfigurationMs = 30;
constexpr std::int64_t kBootReadyMs = 40;
constexpr std::int64_t kNetworkMs = 50;
constexpr std::int64_t kConnectionMs = 60;
constexpr std::int64_t kIdentityMs = 70;
constexpr std::int64_t kSnapshotMs = 80;
constexpr std::int64_t kJobMs = 90;

// Fixture-specific events start here, so a prelude can never collide with them.
constexpr std::int64_t kSpecificMs = 100;

// Past the default expiry window (60 s), which is what makes confidence Unavailable
// rather than merely degraded.
constexpr std::int64_t kExpiredMs = 120000;

// Inside the stale window and past the aging one, so the observation classifies as
// DataFreshness::Stale and confidence becomes Degraded rather than Unavailable.
//
// Not an arbitrary number: the freshness policy's boundaries are 4 s, 15 s and 60 s,
// and 30 s sits in the middle of the third band — far enough from both edges that a
// later revision of those provisional thresholds cannot silently move this fixture
// into a different confidence.
constexpr std::int64_t kDegradedMs = 30000;

// Observation sequences. Fixtures are single-shot, so a fixed pair is enough.
constexpr std::uint32_t kSnapshotSequence = 1;
constexpr std::uint32_t kJobSequence = 1;

// Carries the base timestamp and the sink, so each emit line reads as "this fact,
// at this offset" and nothing else.
struct Script {
    Timestamp base;
    EventSink sink;
    void* context;

    [[nodiscard]] Timestamp at(const std::int64_t offset_ms) const {
        return base + std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{offset_ms});
    }

    void emit(const Event& event) const {
        sink(event, context);
    }
};

// --- Preludes --------------------------------------------------------------

// The device powering up. Stops short of a resolved boot on purpose: this alone is
// the BootStarting fixture.
void emit_starting(const Script& script, const app_model::HardwareProbeObservation& hardware) {
    script.emit(Event::boot_phase(script.at(kStartingMs), app_model::BootPhase::Starting));
    script.emit(Event::hardware_probed(script.at(kHardwareMs), hardware));
}

void emit_boot_resolved(const Script& script,
                        const app_model::ConfigurationObservation& configuration) {
    script.emit(Event::storage_observed(script.at(kStorageMs), app_model::StorageStatus::Ready));
    script.emit(Event::configuration_observed(script.at(kConfigurationMs), configuration));
    script.emit(Event::boot_phase(script.at(kBootReadyMs), app_model::BootPhase::Ready));
}

// Booted, configured, and ready to look for the printer.
void emit_configured_device(const Script& script) {
    emit_starting(script, healthy_hardware());
    emit_boot_resolved(script, valid_configuration());
}

// The network up and the printer answering.
void emit_connected(const Script& script) {
    script.emit(Event::network_observed(script.at(kNetworkMs), network_connected()));
    script.emit(Event::connection_observed(script.at(kConnectionMs),
                                           connection(app_model::ConnectionPhase::Connected)));
    script.emit(Event::printer_identity(script.at(kIdentityMs), printer_identity()));
}

// A printer reporting a state, with no job.
void emit_subject(const Script& script, const app_model::PrinterState state) {
    script.emit(
        Event::printer_snapshot(script.at(kSnapshotMs), snapshot(state, kSnapshotSequence)));
}

// A printer reporting a state, with the job that state is about.
void emit_subject_with_job(const Script& script, const app_model::PrinterState state,
                           const JobProgress progress) {
    emit_subject(script, state);
    script.emit(Event::job_observed(script.at(kJobMs), job(kJobSequence, progress)));
}

// The canonical operational device: booted, connected, printing, with a job. 62 % at
// layer 156 of 250 — the figures the accepted Printing artwork shows.
void emit_printing_device(const Script& script) {
    emit_configured_device(script);
    emit_connected(script);
    emit_subject_with_job(script, app_model::PrinterState::Printing, {62, 156});
}

// --- Fixture scripts -------------------------------------------------------

void emit_provisioning(const Script& script, const app_model::ProvisioningPhase phase) {
    emit_starting(script, healthy_hardware());
    emit_boot_resolved(script, missing_configuration());

    // ConfigurationObserved(Missing) already produces SetupRequired (§14.1), so the
    // later phases are the only ones that need stating.
    if (phase != app_model::ProvisioningPhase::SetupRequired) {
        script.emit(Event::provisioning_phase(script.at(kSpecificMs), phase));
    }
}

void emit_access_condition(const Script& script, const app_model::ConnectionPhase phase) {
    emit_configured_device(script);
    script.emit(Event::network_observed(script.at(kNetworkMs), network_connected()));
    script.emit(Event::connection_observed(script.at(kConnectionMs), connection(phase)));
}

void emit_fixture(const Fixture fixture, const Script& script) {
    switch (fixture) {
    case Fixture::BootStarting:
        // Only the lifecycle. Nothing about the printer has been established, and
        // §12 puts this above everything that has not been resolved yet.
        emit_starting(script, healthy_hardware());
        return;

    case Fixture::SetupRequired:
        emit_provisioning(script, app_model::ProvisioningPhase::SetupRequired);
        return;

    case Fixture::SetupActive:
        emit_provisioning(script, app_model::ProvisioningPhase::SetupActive);
        return;

    case Fixture::ValidatingConfiguration:
        emit_provisioning(script, app_model::ProvisioningPhase::ValidatingConfiguration);
        return;

    case Fixture::SetupFailed:
        emit_provisioning(script, app_model::ProvisioningPhase::SetupFailed);
        return;

    case Fixture::AcquiringPrinterState:
        // Configured and probing, but no snapshot has ever arrived — §6's "first
        // trustworthy Subject State" has not been acquired.
        emit_configured_device(script);
        script.emit(Event::network_observed(script.at(kNetworkMs), network_connected()));
        script.emit(Event::connection_observed(script.at(kConnectionMs),
                                               connection(app_model::ConnectionPhase::Probing)));
        return;

    case Fixture::Ready:
        emit_configured_device(script);
        emit_connected(script);
        emit_subject(script, app_model::PrinterState::Ready);
        return;

    case Fixture::Preparing:
        emit_configured_device(script);
        emit_connected(script);
        emit_subject(script, app_model::PrinterState::Preparing);
        return;

    case Fixture::Printing:
        emit_printing_device(script);
        return;

    case Fixture::Paused:
        emit_configured_device(script);
        emit_connected(script);
        emit_subject_with_job(script, app_model::PrinterState::Paused, {62, 156});
        return;

    case Fixture::Completed:
        emit_configured_device(script);
        emit_connected(script);
        // The job first, so the completion snapshot has something to persist: §8.1
        // captures the job that finished, not an empty one.
        script.emit(Event::job_observed(script.at(kJobMs), job(kJobSequence, {100, 250})));
        script.emit(Event::printer_snapshot(
            script.at(kSpecificMs),
            snapshot(app_model::PrinterState::Finished, kSnapshotSequence)));
        return;

    case Fixture::Stopped:
        emit_configured_device(script);
        emit_connected(script);
        script.emit(Event::job_observed(script.at(kJobMs), job(kJobSequence, {34, 86})));
        script.emit(Event::printer_snapshot(
            script.at(kSpecificMs), snapshot(app_model::PrinterState::Stopped, kSnapshotSequence)));
        return;

    case Fixture::PrintingDegraded:
        // The same live print, then long enough with no news that §11.5 makes it
        // stale — but not long enough to expire it. UIAD 001 §7's Degraded: "a
        // recent known Subject State may still be represented with reduced
        // confidence", which the projection reports as the explicit indicator.
        //
        // The step this reproduces already exists in the confidence-decay scenario,
        // so nothing new is asserted about the model; what is new is a fixture that
        // HOLDS it, which the scenario sweep's own schedule cannot do.
        emit_printing_device(script);
        script.emit(Event::confidence_aged(script.at(kDegradedMs)));
        return;

    case Fixture::ReadyDegraded:
        // The resting device, then the same silence. Deliberately the identical
        // ageing step as PrintingDegraded rather than a second threshold: what is
        // being varied between the two is the energy the composition is drawn at,
        // and a different age would confound that with the confidence mechanism.
        //
        // UIDD 004 §10 is why one degraded state is not enough. Confidence is a rule
        // over the family, not a Printing behaviour, so it has to be observable at
        // both ends of the axis before it can be judged as one.
        emit_configured_device(script);
        emit_connected(script);
        emit_subject(script, app_model::PrinterState::Ready);
        script.emit(Event::confidence_aged(script.at(kDegradedMs)));
        return;

    case Fixture::PrintingUnknownProgress:
        // A printer reporting that it is printing before any job observation has
        // arrived. Not a contrived state: the printer's status and its job are
        // separate API calls (API Research Report §16), so the status can land
        // first, and `progress.known` exists precisely to model the gap.
        //
        // UI Design Decision 004 §3.2 is what makes this worth holding on the panel:
        // "the record shall never determine the form's shape". The form is therefore
        // present at maximum energy with no record to draw on it.
        emit_configured_device(script);
        emit_connected(script);
        emit_subject(script, app_model::PrinterState::Printing);
        return;

    case Fixture::PrinterUnavailable:
        // A live print, then long enough with no news that §11.5 expires it. The
        // subject is remembered; it simply may no longer be presented as current.
        emit_printing_device(script);
        script.emit(Event::connection_observed(
            script.at(kSpecificMs), connection(app_model::ConnectionPhase::PrinterUnavailable)));
        script.emit(Event::confidence_aged(script.at(kExpiredMs)));
        return;

    case Fixture::AuthenticationRequired:
        emit_access_condition(script, app_model::ConnectionPhase::Unauthorized);
        return;

    case Fixture::UnsupportedInterface:
        emit_access_condition(script, app_model::ConnectionPhase::UnsupportedApi);
        return;

    case Fixture::FirmwareUpdating:
        emit_printing_device(script);
        script.emit(
            Event::ota_observed(script.at(kSpecificMs), ota(app_model::OtaPhase::Installing, 45)));
        return;

    case Fixture::FactoryResetting:
        // Reachable only through the confirmation surface, which is the state
        // machine's own precondition (§5.6) rather than a UI convention.
        emit_printing_device(script);
        script.emit(
            Event::button_gesture(script.at(kSpecificMs), app_model::ButtonGesture::VeryLongPress));
        script.emit(Event::factory_reset_confirmed(script.at(kSpecificMs + 10)));
        return;

    case Fixture::RecoveryMode:
        emit_configured_device(script);
        script.emit(Event::recovery_mode_entered(script.at(kSpecificMs)));
        return;

    case Fixture::DeviceFault:
        // Raised by the §12.4 capacity mismatch rather than asserted directly, so
        // the fixture exercises the only path that can produce it.
        emit_starting(script, mismatched_hardware());
        emit_boot_resolved(script, valid_configuration());
        return;

    case Fixture::FactoryResetConfirmation:
        emit_printing_device(script);
        script.emit(
            Event::button_gesture(script.at(kSpecificMs), app_model::ButtonGesture::VeryLongPress));
        return;
    }
}

constexpr std::array<Fixture, kFixtureCount> kAllFixtures = {
    Fixture::BootStarting,
    Fixture::SetupRequired,
    Fixture::SetupActive,
    Fixture::ValidatingConfiguration,
    Fixture::SetupFailed,
    Fixture::AcquiringPrinterState,
    Fixture::Ready,
    Fixture::Preparing,
    Fixture::Printing,
    Fixture::Paused,
    Fixture::Completed,
    Fixture::Stopped,
    Fixture::PrintingDegraded,
    Fixture::ReadyDegraded,
    Fixture::PrintingUnknownProgress,
    Fixture::PrinterUnavailable,
    Fixture::AuthenticationRequired,
    Fixture::UnsupportedInterface,
    Fixture::FirmwareUpdating,
    Fixture::FactoryResetting,
    Fixture::RecoveryMode,
    Fixture::DeviceFault,
    Fixture::FactoryResetConfirmation,
};

static_assert(kAllFixtures.size() == kFixtureCount, "kFixtureCount must match the fixture table");

} // namespace

std::string_view name(const Fixture fixture) {
    switch (fixture) {
    case Fixture::BootStarting:
        return "boot-starting";

    case Fixture::SetupRequired:
        return "setup-required";

    case Fixture::SetupActive:
        return "setup-active";

    case Fixture::ValidatingConfiguration:
        return "validating-configuration";

    case Fixture::SetupFailed:
        return "setup-failed";

    case Fixture::AcquiringPrinterState:
        return "acquiring-printer-state";

    case Fixture::Ready:
        return "ready";

    case Fixture::Preparing:
        return "preparing";

    case Fixture::Printing:
        return "printing";

    case Fixture::Paused:
        return "paused";

    case Fixture::Completed:
        return "completed";

    case Fixture::Stopped:
        return "stopped";

    case Fixture::PrintingDegraded:
        return "printing-degraded";

    case Fixture::ReadyDegraded:
        return "ready-degraded";

    case Fixture::PrintingUnknownProgress:
        return "printing-unknown-progress";

    case Fixture::PrinterUnavailable:
        return "printer-unavailable";

    case Fixture::AuthenticationRequired:
        return "authentication-required";

    case Fixture::UnsupportedInterface:
        return "unsupported-interface";

    case Fixture::FirmwareUpdating:
        return "firmware-updating";

    case Fixture::FactoryResetting:
        return "factory-resetting";

    case Fixture::RecoveryMode:
        return "recovery-mode";

    case Fixture::DeviceFault:
        return "device-fault";

    case Fixture::FactoryResetConfirmation:
        return "factory-reset-confirmation";
    }

    return "unknown";
}

Fixture fixture_at(const std::size_t index) {
    if (index >= kFixtureCount) {
        return Fixture::BootStarting;
    }

    return kAllFixtures[index];
}

void emit_script(const Fixture fixture, const Timestamp base, const EventSink sink,
                 void* const context) {
    if (sink == nullptr) {
        return;
    }

    const Script script{base, sink, context};
    emit_fixture(fixture, script);
}

app_model::AppState state_for(const Fixture fixture, const Timestamp base) {
    app_model::AppState state;

    emit_script(
        fixture, base,
        [](const Event& event, void* const context) {
            auto* const target = static_cast<app_model::AppState*>(context);
            *target = app_model::reduce(*target, event);
        },
        &state);

    return state;
}

} // namespace prd::fake_source
