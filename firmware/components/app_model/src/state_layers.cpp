// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/state_layers.hpp"

// Log-stable lowercase labels. Every switch is exhaustive over its enum with no
// default inside the switch, so adding a state fails the build at every place
// that has to know about it rather than silently falling through to "unknown".
// The trailing return exists only because the value may be out of range at run
// time (a corrupted state, a value from a future version).

namespace prd::app_model {

std::string_view log_label(const ProvisioningPhase phase) {
    switch (phase) {
    case ProvisioningPhase::Inactive:
        return "inactive";

    case ProvisioningPhase::SetupRequired:
        return "setup-required";

    case ProvisioningPhase::SetupActive:
        return "setup-active";

    case ProvisioningPhase::ValidatingConfiguration:
        return "validating-configuration";

    case ProvisioningPhase::SetupFailed:
        return "setup-failed";
    }

    return "unknown";
}

std::string_view log_label(const SubjectState state) {
    switch (state) {
    case SubjectState::AcquiringState:
        return "acquiring-state";

    case SubjectState::Ready:
        return "ready";

    case SubjectState::Preparing:
        return "preparing";

    case SubjectState::Printing:
        return "printing";

    case SubjectState::Paused:
        return "paused";

    case SubjectState::Completed:
        return "completed";

    case SubjectState::Stopped:
        return "stopped";
    }

    return "unknown";
}

std::string_view log_label(const SubjectCondition condition) {
    switch (condition) {
    case SubjectCondition::None:
        return "none";

    case SubjectCondition::Attention:
        return "attention";

    case SubjectCondition::Error:
        return "error";
    }

    return "unknown";
}

std::string_view log_label(const Confidence confidence) {
    switch (confidence) {
    case Confidence::Current:
        return "current";

    case Confidence::Degraded:
        return "degraded";

    case Confidence::Unavailable:
        return "unavailable";
    }

    return "unknown";
}

std::string_view log_label(const AccessCondition condition) {
    switch (condition) {
    case AccessCondition::None:
        return "none";

    case AccessCondition::AuthenticationRequired:
        return "authentication-required";

    case AccessCondition::UnsupportedInterface:
        return "unsupported-interface";
    }

    return "unknown";
}

std::string_view log_label(const ModalSurface surface) {
    switch (surface) {
    case ModalSurface::None:
        return "none";

    case ModalSurface::Settings:
        return "settings";

    case ModalSurface::FactoryResetConfirmation:
        return "factory-reset-confirmation";
    }

    return "unknown";
}

std::string_view log_label(const DeviceOperation operation) {
    switch (operation) {
    case DeviceOperation::None:
        return "none";

    case DeviceOperation::FirmwareUpdating:
        return "firmware-updating";

    case DeviceOperation::FactoryResetting:
        return "factory-resetting";

    case DeviceOperation::RecoveryMode:
        return "recovery-mode";

    case DeviceOperation::DeviceFault:
        return "device-fault";
    }

    return "unknown";
}

} // namespace prd::app_model
