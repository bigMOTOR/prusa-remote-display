// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fake_source/scenarios.hpp"

#include <array>
#include <chrono>

#include "fake_source/observations.hpp"

namespace prd::fake_source {
namespace {

using app_model::Event;
using app_model::Timestamp;

// Carries the base timestamp so a step reads as "this fact, this many milliseconds
// in", and the sequence offset that makes a replay monotonic.
struct Script {
    Timestamp base;
    std::uint32_t sequence_base;

    [[nodiscard]] Timestamp at(const std::int64_t offset_ms) const {
        return base + std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{offset_ms});
    }

    [[nodiscard]] std::uint32_t sequence(const std::uint32_t ordinal) const {
        return sequence_base + ordinal;
    }
};

Step labelled(const Event& event, const std::string_view label) {
    Step step;
    step.event = event;
    step.label.assign(label);
    return step;
}

// --- Shared preludes -------------------------------------------------------
//
// Almost every scenario starts from a booted device, and most from one already
// watching a printer. Writing those out nine times would guarantee they drifted
// apart.

constexpr std::size_t kBootPreludeSteps = 5;
constexpr std::size_t kReadyPreludeSteps = 8;

// Steps 0-4: cold boot to a resolved, configured device.
Step boot_prelude(const std::size_t index, const Script& script,
                  const app_model::ConfigurationObservation& configuration) {
    switch (index) {
    case 0:
        return labelled(Event::boot_phase(script.at(0), app_model::BootPhase::Starting),
                        "boot starting");
    case 1:
        return labelled(Event::hardware_probed(script.at(10), healthy_hardware()),
                        "hardware validated");
    case 2:
        return labelled(Event::storage_observed(script.at(20), app_model::StorageStatus::Ready),
                        "storage ready");
    case 3:
        return labelled(Event::configuration_observed(script.at(30), configuration),
                        configuration.status == app_model::ConfigurationStatus::Valid
                            ? "configuration valid"
                            : "no configuration");
    case 4:
        return labelled(Event::boot_phase(script.at(40), app_model::BootPhase::Ready),
                        "boot complete");
    default:
        return Step{};
    }
}

// Steps 0-7: the boot prelude, then network, printer connection and a printer
// reporting Ready.
Step ready_prelude(const std::size_t index, const Script& script) {
    if (index < kBootPreludeSteps) {
        return boot_prelude(index, script, valid_configuration());
    }

    switch (index) {
    case 5:
        return labelled(Event::network_observed(script.at(50), network_connected()),
                        "wifi connected");
    case 6:
        return labelled(Event::connection_observed(
                            script.at(60), connection(app_model::ConnectionPhase::Connected)),
                        "printer connected");
    case 7:
        return labelled(
            Event::printer_snapshot(script.at(70),
                                    snapshot(app_model::PrinterState::Ready, script.sequence(1))),
            "printer ready");
    default:
        return Step{};
    }
}

// --- 1. Cold boot to ready -------------------------------------------------

constexpr std::size_t kColdBootSteps = kReadyPreludeSteps;

Step cold_boot_step(const std::size_t index, const Script& script) {
    return ready_prelude(index, script);
}

// --- 2. First setup to ready -----------------------------------------------

constexpr std::size_t kFirstSetupSteps = 12;

Step first_setup_step(const std::size_t index, const Script& script) {
    if (index < kBootPreludeSteps) {
        return boot_prelude(index, script, missing_configuration());
    }

    switch (index) {
    case 5:
        return labelled(
            Event::provisioning_phase(script.at(1000), app_model::ProvisioningPhase::SetupActive),
            "setup active");
    case 6:
        return labelled(Event::provisioning_phase(
                            script.at(2000), app_model::ProvisioningPhase::ValidatingConfiguration),
                        "validating configuration");
    case 7:
        // A valid configuration ends provisioning (§14.1's converse) and clears any
        // access classification, so acquisition may begin.
        return labelled(Event::configuration_observed(script.at(3000), valid_configuration(1)),
                        "configuration saved");
    case 8:
        return labelled(Event::network_observed(script.at(3100), network_connected()),
                        "wifi connected");
    case 9:
        return labelled(Event::connection_observed(script.at(3200),
                                                   connection(app_model::ConnectionPhase::Probing)),
                        "probing printer");
    case 10:
        return labelled(Event::connection_observed(
                            script.at(4000), connection(app_model::ConnectionPhase::Connected)),
                        "printer connected");
    case 11:
        return labelled(
            Event::printer_snapshot(script.at(4100),
                                    snapshot(app_model::PrinterState::Ready, script.sequence(1))),
            "printer ready");
    default:
        return Step{};
    }
}

// --- 3. Full print cycle ---------------------------------------------------

constexpr std::size_t kFullPrintCycleSteps = 19;

Step full_print_cycle_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        return labelled(
            Event::printer_snapshot(
                script.at(1000), snapshot(app_model::PrinterState::Preparing, script.sequence(2))),
            "heating");
    case 9:
        return labelled(
            Event::printer_snapshot(
                script.at(2000), snapshot(app_model::PrinterState::Printing, script.sequence(3))),
            "printing");
    case 10:
        return labelled(Event::job_observed(script.at(2100), job(script.sequence(1), {5, 12})),
                        "job 5%");
    case 11:
        return labelled(Event::job_observed(script.at(3000), job(script.sequence(2), {40, 100})),
                        "job 40%");
    case 12:
        return labelled(
            Event::printer_snapshot(script.at(4000),
                                    snapshot(app_model::PrinterState::Paused, script.sequence(4))),
            "paused");
    case 13:
        return labelled(
            Event::printer_snapshot(
                script.at(5000), snapshot(app_model::PrinterState::Printing, script.sequence(5))),
            "resumed");
    case 14:
        return labelled(Event::job_observed(script.at(5100), job(script.sequence(3), {80, 200})),
                        "job 80%");
    case 15:
        return labelled(Event::job_observed(script.at(6000), job(script.sequence(4), {100, 250})),
                        "job 100%");
    case 16:
        // The terminal transition, which is what persists the completion snapshot.
        return labelled(
            Event::printer_snapshot(
                script.at(6100), snapshot(app_model::PrinterState::Finished, script.sequence(6))),
            "completed");
    case 17:
        return labelled(Event::job_unavailable(script.at(8000), script.sequence(5)),
                        "job cleared by printer");
    case 18:
        // §9 Print Completed: the interface settles back toward Ready.
        return labelled(
            Event::printer_snapshot(script.at(9000),
                                    snapshot(app_model::PrinterState::Ready, script.sequence(7))),
            "settled to ready");
    default:
        return Step{};
    }
}

// --- 4. Printing to stopped ------------------------------------------------

constexpr std::size_t kPrintingToStoppedSteps = 13;

Step printing_to_stopped_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        return labelled(
            Event::printer_snapshot(
                script.at(1000), snapshot(app_model::PrinterState::Preparing, script.sequence(2))),
            "heating");
    case 9:
        return labelled(
            Event::printer_snapshot(
                script.at(2000), snapshot(app_model::PrinterState::Printing, script.sequence(3))),
            "printing");
    case 10:
        return labelled(Event::job_observed(script.at(2100), job(script.sequence(1), {34, 86})),
                        "job 34%");
    case 11:
        return labelled(
            Event::printer_snapshot(script.at(4000),
                                    snapshot(app_model::PrinterState::Stopped, script.sequence(4))),
            "stopped");
    case 12:
        return labelled(
            Event::printer_snapshot(script.at(8000),
                                    snapshot(app_model::PrinterState::Ready, script.sequence(5))),
            "settled to ready");
    default:
        return Step{};
    }
}

// --- 5. Confidence decay ---------------------------------------------------

constexpr std::size_t kConfidenceDecaySteps = 14;

Step confidence_decay_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        return labelled(
            Event::printer_snapshot(
                script.at(1000), snapshot(app_model::PrinterState::Printing, script.sequence(2))),
            "printing");
    case 9:
        return labelled(Event::job_observed(script.at(1100), job(script.sequence(1), {62, 156})),
                        "job 62%");
    case 10:
        // Past the fresh window: a recent known state, reduced confidence (§7).
        return labelled(Event::confidence_aged(script.at(6000)), "confidence degraded");
    case 11:
        return labelled(
            Event::connection_observed(script.at(20000),
                                       connection(app_model::ConnectionPhase::PrinterUnavailable)),
            "printer stopped answering");
    case 12:
        // Still Degraded: §11.5 keeps stale values presentable behind an indicator.
        return labelled(Event::confidence_aged(script.at(30000)), "state stale");
    case 13:
        // Expired: the live counters are removed and nothing may be presented as
        // current.
        return labelled(Event::confidence_aged(script.at(70000)), "state expired");
    default:
        return Step{};
    }
}

// --- 6. Authentication recovery -------------------------------------------

constexpr std::size_t kAuthenticationRecoverySteps = 11;

Step authentication_recovery_step(const std::size_t index, const Script& script) {
    if (index < kBootPreludeSteps) {
        return boot_prelude(index, script, valid_configuration());
    }

    switch (index) {
    case 5:
        return labelled(Event::network_observed(script.at(50), network_connected()),
                        "wifi connected");
    case 6:
        return labelled(Event::connection_observed(
                            script.at(60), connection(app_model::ConnectionPhase::Unauthorized)),
                        "api key rejected");
    case 7:
        // §16.2 permits a retry only after a configuration change, and a new
        // credential generation is what makes this one (§13.3).
        return labelled(Event::configuration_observed(script.at(1000), valid_configuration(1)),
                        "api key corrected");
    case 8:
        return labelled(Event::connection_observed(script.at(1100),
                                                   connection(app_model::ConnectionPhase::Probing)),
                        "probing printer");
    case 9:
        return labelled(Event::connection_observed(
                            script.at(2000), connection(app_model::ConnectionPhase::Connected)),
                        "printer connected");
    case 10:
        return labelled(
            Event::printer_snapshot(script.at(2100),
                                    snapshot(app_model::PrinterState::Ready, script.sequence(1))),
            "printer ready");
    default:
        return Step{};
    }
}

// --- 7. Firmware update lifecycle -----------------------------------------

constexpr std::size_t kFirmwareUpdateSteps = 16;

Step firmware_update_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        // Information, not an operation: §18.6 forbids silent installation, so a
        // background check must not take the screen.
        return labelled(Event::ota_observed(script.at(1000), ota(app_model::OtaPhase::Checking, 0)),
                        "checking for update");
    case 9:
        return labelled(
            Event::ota_observed(script.at(2000), ota(app_model::OtaPhase::Available, 0)),
            "update available");
    case 10:
        return labelled(
            Event::ota_observed(script.at(3000), ota(app_model::OtaPhase::Downloading, 20)),
            "downloading 20%");
    case 11:
        return labelled(
            Event::ota_observed(script.at(4000), ota(app_model::OtaPhase::Downloading, 70)),
            "downloading 70%");
    case 12:
        return labelled(
            Event::ota_observed(script.at(5000), ota(app_model::OtaPhase::Verifying, 100)),
            "verifying image");
    case 13:
        return labelled(
            Event::ota_observed(script.at(6000), ota(app_model::OtaPhase::Installing, 40)),
            "installing 40%");
    case 14:
        return labelled(
            Event::ota_observed(script.at(7000), ota(app_model::OtaPhase::Installing, 95)),
            "installing 95%");
    case 15:
        return labelled(
            Event::ota_observed(script.at(8000), ota(app_model::OtaPhase::RebootRequired, 100)),
            "reboot required");
    default:
        return Step{};
    }
}

// --- 8. Recovery and device fault -----------------------------------------

constexpr std::size_t kRecoveryAndFaultSteps = 11;

Step recovery_and_fault_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        return labelled(Event::recovery_mode_entered(script.at(1000)), "recovery mode");
    case 9:
        return labelled(Event::recovery_mode_exited(script.at(5000)), "recovery cleared");
    case 10:
        // A device cause, and only a device cause: DeviceFaultReason cannot express
        // a printer error or an auth failure (UIAD 001 §10).
        return labelled(
            Event::device_fault(script.at(6000), app_model::DeviceFaultReason::LowMemory),
            "device fault");
    default:
        return Step{};
    }
}

// --- 9. Modal over an operational subject ---------------------------------

// Two fewer than before Milestone 3.10: the Settings pair was removed with the gesture that
// opened it (owner ruling 1). What the scenario demonstrates is unchanged.
constexpr std::size_t kModalOverSubjectSteps = 12;

Step modal_over_subject_step(const std::size_t index, const Script& script) {
    if (index < kReadyPreludeSteps) {
        return ready_prelude(index, script);
    }

    switch (index) {
    case 8:
        return labelled(
            Event::printer_snapshot(
                script.at(1000), snapshot(app_model::PrinterState::Printing, script.sequence(2))),
            "printing");
    case 9:
        return labelled(Event::job_observed(script.at(1100), job(script.sequence(1), {62, 156})),
                        "job 62%");
    // ⬅️ ONE MODAL, NOT TWO — owner ruling 1 of Milestone 3.10. This scenario used to open
    // Settings with a long press and then the reset confirmation with a very long one. The long
    // press is now deliberate reconfiguration entry and opens no modal at all, so the first pair
    // would have scripted a gesture that changes nothing and then dismissed a surface that was
    // never there. What the scenario is for — a modal drawing over a live print and giving it
    // back — is unchanged and is demonstrated by the confirmation alone.
    case 10:
        return labelled(
            Event::button_gesture(script.at(2000), app_model::ButtonGesture::VeryLongPress),
            "reset confirmation");
    case 11:
        return labelled(Event::modal_surface_dismissed(script.at(5000)), "confirmation dismissed");
    default:
        return Step{};
    }
}

constexpr std::array<Scenario, kScenarioCount> kAllScenarios = {
    Scenario::ColdBootToReady,
    Scenario::FirstSetupToReady,
    Scenario::FullPrintCycle,
    Scenario::PrintingToStopped,
    Scenario::ConfidenceDecay,
    Scenario::AuthenticationRecovery,
    Scenario::FirmwareUpdateLifecycle,
    Scenario::RecoveryAndDeviceFault,
    Scenario::ModalOverOperationalSubject,
};

static_assert(kAllScenarios.size() == kScenarioCount,
              "kScenarioCount must match the scenario table");

} // namespace

std::string_view name(const Scenario scenario) {
    switch (scenario) {
    case Scenario::ColdBootToReady:
        return "cold-boot-to-ready";

    case Scenario::FirstSetupToReady:
        return "first-setup-to-ready";

    case Scenario::FullPrintCycle:
        return "full-print-cycle";

    case Scenario::PrintingToStopped:
        return "printing-to-stopped";

    case Scenario::ConfidenceDecay:
        return "confidence-decay";

    case Scenario::AuthenticationRecovery:
        return "authentication-recovery";

    case Scenario::FirmwareUpdateLifecycle:
        return "firmware-update-lifecycle";

    case Scenario::RecoveryAndDeviceFault:
        return "recovery-and-device-fault";

    case Scenario::ModalOverOperationalSubject:
        return "modal-over-operational-subject";
    }

    return "unknown";
}

Scenario scenario_at(const std::size_t index) {
    if (index >= kScenarioCount) {
        return Scenario::ColdBootToReady;
    }

    return kAllScenarios[index];
}

std::size_t scenario_index(const Scenario scenario) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        if (kAllScenarios[index] == scenario) {
            return index;
        }
    }

    return 0;
}

std::size_t step_count(const Scenario scenario) {
    switch (scenario) {
    case Scenario::ColdBootToReady:
        return kColdBootSteps;

    case Scenario::FirstSetupToReady:
        return kFirstSetupSteps;

    case Scenario::FullPrintCycle:
        return kFullPrintCycleSteps;

    case Scenario::PrintingToStopped:
        return kPrintingToStoppedSteps;

    case Scenario::ConfidenceDecay:
        return kConfidenceDecaySteps;

    case Scenario::AuthenticationRecovery:
        return kAuthenticationRecoverySteps;

    case Scenario::FirmwareUpdateLifecycle:
        return kFirmwareUpdateSteps;

    case Scenario::RecoveryAndDeviceFault:
        return kRecoveryAndFaultSteps;

    case Scenario::ModalOverOperationalSubject:
        return kModalOverSubjectSteps;
    }

    return 0;
}

Step step_at(const Scenario scenario, const std::size_t index, const Timestamp base,
             const std::uint32_t sequence_base) {
    if (index >= step_count(scenario)) {
        return Step{};
    }

    const Script script{base, sequence_base};

    switch (scenario) {
    case Scenario::ColdBootToReady:
        return cold_boot_step(index, script);

    case Scenario::FirstSetupToReady:
        return first_setup_step(index, script);

    case Scenario::FullPrintCycle:
        return full_print_cycle_step(index, script);

    case Scenario::PrintingToStopped:
        return printing_to_stopped_step(index, script);

    case Scenario::ConfidenceDecay:
        return confidence_decay_step(index, script);

    case Scenario::AuthenticationRecovery:
        return authentication_recovery_step(index, script);

    case Scenario::FirmwareUpdateLifecycle:
        return firmware_update_step(index, script);

    case Scenario::RecoveryAndDeviceFault:
        return recovery_and_fault_step(index, script);

    case Scenario::ModalOverOperationalSubject:
        return modal_over_subject_step(index, script);
    }

    return Step{};
}

} // namespace prd::fake_source
