// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/events.hpp"

namespace prd::app_model {
namespace {

// Every factory starts here, so the tag and the timestamp are set in exactly one
// place and a new event type cannot forget either.
Event make(const EventType type, const Timestamp moment) {
    Event event;
    event.type = type;
    event.observed_at = moment;
    return event;
}

} // namespace

std::string_view log_label(const ButtonGesture gesture) {
    switch (gesture) {
    case ButtonGesture::ShortPress:
        return "short-press";

    case ButtonGesture::LongPress:
        return "long-press";

    case ButtonGesture::VeryLongPress:
        return "very-long-press";
    }

    return "unknown";
}

std::string_view log_label(const EventType type) {
    switch (type) {
    case EventType::None:
        return "none";

    case EventType::BootPhaseObserved:
        return "boot-phase-observed";

    case EventType::HardwareProbed:
        return "hardware-probed";

    case EventType::StorageObserved:
        return "storage-observed";

    case EventType::DeviceFaultRaised:
        return "device-fault-raised";

    case EventType::RecoveryModeEntered:
        return "recovery-mode-entered";

    case EventType::RecoveryModeExited:
        return "recovery-mode-exited";

    case EventType::ConfigurationObserved:
        return "configuration-observed";

    case EventType::ProvisioningPhaseObserved:
        return "provisioning-phase-observed";

    case EventType::NetworkObserved:
        return "network-observed";

    case EventType::PrinterConnectionObserved:
        return "printer-connection-observed";

    case EventType::PrinterIdentityObserved:
        return "printer-identity-observed";

    case EventType::PrinterSnapshotObserved:
        return "printer-snapshot-observed";

    case EventType::PrinterStatusObserved:
        return "printer-status";

    case EventType::JobObserved:
        return "job-observed";

    case EventType::JobUnavailable:
        return "job-unavailable";

    case EventType::AlertRaised:
        return "alert-raised";

    case EventType::AlertCleared:
        return "alert-cleared";

    case EventType::ConfidenceAged:
        return "confidence-aged";

    case EventType::OtaObserved:
        return "ota-observed";

    case EventType::FactoryResetConfirmed:
        return "factory-reset-confirmed";

    case EventType::FactoryResetCancelled:
        return "factory-reset-cancelled";

    case EventType::DisplayIdleTimerFired:
        return "display-idle-timer-fired";

    case EventType::DisplaySleepTimerFired:
        return "display-sleep-timer-fired";

    case EventType::DisplayWakeRequested:
        return "display-wake-requested";

    case EventType::DisplayBrightnessObserved:
        return "display-brightness-observed";

    case EventType::ButtonGestureObserved:
        return "button-gesture-observed";

    case EventType::ModalSurfaceDismissed:
        return "modal-surface-dismissed";
    }

    return "unknown";
}

// --- Boot and device lifecycle ---------------------------------------------

Event Event::boot_phase(const Timestamp moment, const BootPhase phase) {
    Event event = make(EventType::BootPhaseObserved, moment);
    event.boot.phase = phase;
    return event;
}

Event Event::hardware_probed(const Timestamp moment, const HardwareProbeObservation& observation) {
    Event event = make(EventType::HardwareProbed, moment);
    event.hardware = observation;
    return event;
}

Event Event::storage_observed(const Timestamp moment, const StorageStatus status) {
    Event event = make(EventType::StorageObserved, moment);
    event.storage.status = status;
    return event;
}

Event Event::device_fault(const Timestamp moment, const DeviceFaultReason reason) {
    Event event = make(EventType::DeviceFaultRaised, moment);
    event.fault.reason = reason;
    return event;
}

Event Event::recovery_mode_entered(const Timestamp moment) {
    return make(EventType::RecoveryModeEntered, moment);
}

Event Event::recovery_mode_exited(const Timestamp moment) {
    return make(EventType::RecoveryModeExited, moment);
}

// --- Setup and configuration lifecycle -------------------------------------

Event Event::configuration_observed(const Timestamp moment,
                                    const ConfigurationObservation& observation) {
    Event event = make(EventType::ConfigurationObserved, moment);
    event.configuration = observation;
    return event;
}

Event Event::provisioning_phase(const Timestamp moment, const ProvisioningPhase phase) {
    Event event = make(EventType::ProvisioningPhaseObserved, moment);
    event.provisioning.phase = phase;
    return event;
}

// --- Network lifecycle -----------------------------------------------------

Event Event::network_observed(const Timestamp moment, const NetworkObservation& observation) {
    Event event = make(EventType::NetworkObserved, moment);
    event.network = observation;
    return event;
}

// --- Printer connection lifecycle ------------------------------------------

Event Event::connection_observed(const Timestamp moment, const ConnectionObservation& observation) {
    Event event = make(EventType::PrinterConnectionObserved, moment);
    event.connection = observation;
    return event;
}

Event Event::printer_identity(const Timestamp moment, const PrinterIdentity& identity) {
    Event event = make(EventType::PrinterIdentityObserved, moment);
    event.identity.identity = identity;
    return event;
}

// --- Printer snapshot updates ----------------------------------------------

Event Event::printer_snapshot(const Timestamp moment,
                              const PrinterSnapshotObservation& observation) {
    Event event = make(EventType::PrinterSnapshotObserved, moment);
    event.printer = observation;
    return event;
}

// --- Job lifecycle ---------------------------------------------------------

Event Event::printer_status(const Timestamp moment, const StatusObservation& observation) {
    Event event = make(EventType::PrinterStatusObserved, moment);
    event.status = observation;
    return event;
}

Event Event::job_observed(const Timestamp moment, const JobObservation& observation) {
    Event event = make(EventType::JobObserved, moment);
    event.job = observation;
    return event;
}

Event Event::job_unavailable(const Timestamp moment, const std::uint32_t sequence) {
    Event event = make(EventType::JobUnavailable, moment);
    event.job_absence.sequence = sequence;
    return event;
}

// --- Warnings and errors ---------------------------------------------------

Event Event::alert_raised(const Timestamp moment, const AlertObservation& observation) {
    Event event = make(EventType::AlertRaised, moment);
    event.alert = observation;
    return event;
}

Event Event::alert_cleared(const Timestamp moment) {
    return make(EventType::AlertCleared, moment);
}

// --- Confidence ageing -----------------------------------------------------

Event Event::confidence_aged(const Timestamp moment) {
    return make(EventType::ConfidenceAged, moment);
}

// --- Firmware update and reset lifecycle -----------------------------------

Event Event::ota_observed(const Timestamp moment, const OtaObservation& observation) {
    Event event = make(EventType::OtaObserved, moment);
    event.ota = observation;
    return event;
}

Event Event::factory_reset_confirmed(const Timestamp moment) {
    return make(EventType::FactoryResetConfirmed, moment);
}

Event Event::factory_reset_cancelled(const Timestamp moment) {
    return make(EventType::FactoryResetCancelled, moment);
}

// --- Display power and idle ------------------------------------------------

Event Event::display_idle_timer_fired(const Timestamp moment) {
    return make(EventType::DisplayIdleTimerFired, moment);
}

Event Event::display_sleep_timer_fired(const Timestamp moment) {
    return make(EventType::DisplaySleepTimerFired, moment);
}

Event Event::display_wake(const Timestamp moment) {
    return make(EventType::DisplayWakeRequested, moment);
}

Event Event::display_brightness(const Timestamp moment, const std::uint8_t brightness_percent) {
    Event event = make(EventType::DisplayBrightnessObserved, moment);
    event.brightness.brightness_percent = brightness_percent;
    return event;
}

// --- Physical input --------------------------------------------------------

Event Event::button_gesture(const Timestamp moment, const ButtonGesture gesture) {
    Event event = make(EventType::ButtonGestureObserved, moment);
    event.button.gesture = gesture;
    return event;
}

// --- Modal surface lifecycle -----------------------------------------------

Event Event::modal_surface_dismissed(const Timestamp moment) {
    return make(EventType::ModalSurfaceDismissed, moment);
}

} // namespace prd::app_model
