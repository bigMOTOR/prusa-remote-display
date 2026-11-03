// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/device_data.hpp"

namespace prd::app_model {

// Two of these labels do not match their enumerator name, deliberately.
//
// `phase=initializing` and `phase=error` are the strings in every committed
// Milestone 1 hardware-validation evidence log, and those logs exist to be
// compared against later runs. Renaming them to match §7.2's enumerator spelling
// would change nothing about behaviour and would invalidate that comparison, so
// the labels are preserved and the enumerators follow the specification. The label
// is a diagnostics contract; the enumerator is the model.
std::string_view log_label(const BootPhase phase) {
    switch (phase) {
    case BootPhase::Starting:
        return "initializing";

    case BootPhase::HardwareInitializing:
        return "hardware-initializing";

    case BootPhase::StorageInitializing:
        return "storage-initializing";

    case BootPhase::LoadingConfiguration:
        return "loading-configuration";

    case BootPhase::StartingServices:
        return "starting-services";

    case BootPhase::Ready:
        return "ready";

    case BootPhase::FatalError:
        return "error";
    }

    return "unknown";
}

std::string_view log_label(const StorageStatus status) {
    switch (status) {
    case StorageStatus::Unknown:
        return "unknown";

    case StorageStatus::Ready:
        return "ready";

    case StorageStatus::RecoveredWithDefaults:
        return "recovered-with-defaults";

    case StorageStatus::Failed:
        return "failed";
    }

    return "unknown";
}

std::string_view log_label(const ConfigurationStatus status) {
    switch (status) {
    case ConfigurationStatus::Missing:
        return "missing";

    case ConfigurationStatus::Loading:
        return "loading";

    case ConfigurationStatus::Valid:
        return "valid";

    case ConfigurationStatus::Invalid:
        return "invalid";

    case ConfigurationStatus::MigrationRequired:
        return "migration-required";

    case ConfigurationStatus::StorageFailure:
        return "storage-failure";
    }

    return "unknown";
}

std::string_view log_label(const AuthMethod method) {
    switch (method) {
    case AuthMethod::None:
        return "none";

    case AuthMethod::ApiKey:
        return "api-key";

    case AuthMethod::Digest:
        return "digest";
    }

    return "unknown";
}

std::string_view log_label(const NetworkPhase phase) {
    switch (phase) {
    case NetworkPhase::Disabled:
        return "disabled";

    case NetworkPhase::Starting:
        return "starting";

    case NetworkPhase::Connecting:
        return "connecting";

    case NetworkPhase::Connected:
        return "connected";

    case NetworkPhase::Reconnecting:
        return "reconnecting";

    case NetworkPhase::Unavailable:
        return "unavailable";
    }

    return "unknown";
}

std::string_view log_label(const ConnectionPhase phase) {
    switch (phase) {
    case ConnectionPhase::Unconfigured:
        return "unconfigured";

    case ConnectionPhase::WaitingForNetwork:
        return "waiting-for-network";

    case ConnectionPhase::Resolving:
        return "resolving";

    case ConnectionPhase::Probing:
        return "probing";

    case ConnectionPhase::Authenticating:
        return "authenticating";

    case ConnectionPhase::Connected:
        return "connected";

    case ConnectionPhase::Degraded:
        return "degraded";

    case ConnectionPhase::Unauthorized:
        return "unauthorized";

    case ConnectionPhase::PrinterUnavailable:
        return "printer-unavailable";

    case ConnectionPhase::UnsupportedApi:
        return "unsupported-api";

    case ConnectionPhase::InvalidResponse:
        return "invalid-response";
    }

    return "unknown";
}

std::string_view log_label(const DisplayPower power) {
    switch (power) {
    case DisplayPower::On:
        return "on";

    case DisplayPower::Dimmed:
        return "dimmed";

    case DisplayPower::Off:
        return "off";
    }

    return "unknown";
}

std::string_view log_label(const OtaPhase phase) {
    switch (phase) {
    case OtaPhase::Idle:
        return "idle";

    case OtaPhase::Checking:
        return "checking";

    case OtaPhase::Available:
        return "available";

    case OtaPhase::Downloading:
        return "downloading";

    case OtaPhase::Verifying:
        return "verifying";

    case OtaPhase::Installing:
        return "installing";

    case OtaPhase::RebootRequired:
        return "reboot-required";

    case OtaPhase::Validating:
        return "validating";

    case OtaPhase::Failed:
        return "failed";
    }

    return "unknown";
}

std::string_view log_label(const NetworkFault fault) {
    switch (fault) {
    case NetworkFault::None:
        return "none";

    case NetworkFault::LinkLost:
        return "link-lost";

    case NetworkFault::CredentialsRejected:
        return "credentials-rejected";

    case NetworkFault::DriverUnresponsive:
        return "driver-unresponsive";
    }

    return "none";
}

std::string_view log_label(const DeviceFaultReason reason) {
    switch (reason) {
    case DeviceFaultReason::None:
        return "none";

    case DeviceFaultReason::StorageFailure:
        return "storage-failure";

    case DeviceFaultReason::BoardMismatch:
        return "board-mismatch";

    case DeviceFaultReason::DisplayInitializationFailure:
        return "display-initialization-failure";

    case DeviceFaultReason::LowMemory:
        return "low-memory";

    case DeviceFaultReason::TaskWatchdog:
        return "task-watchdog";

    case DeviceFaultReason::WifiUnavailable:
        return "wifi-unavailable";

    case DeviceFaultReason::FactoryResetIncomplete:
        return "factory-reset-incomplete";

    case DeviceFaultReason::OtaValidationFailure:
        return "ota-validation-failure";
    }

    return "unknown";
}

bool fault_holds_device(const DeviceFaultReason reason) {
    switch (reason) {
    // ⛔ THE TWO THAT HOLD IT.
    //
    // FactoryResetIncomplete: a confirmed reset executed and did not finish. Its journal may still
    // be armed, so a device released into provisioning would have that new configuration erased by
    // the next boot's resume.
    //
    // StorageFailure: the store would not answer. Nothing about the stored state is established, so
    // nothing may be written on the strength of it.
    case DeviceFaultReason::FactoryResetIncomplete:
    case DeviceFaultReason::StorageFailure:
        return true;

    // Everything else leaves the device alone. WifiUnavailable is the one to say out loud: the
    // accepted 3c ruling keeps the setup access point and its server physically alive under it so a
    // browser mid-flow is not stranded, and holding the device would refuse the very submissions
    // that surface is up to accept.
    //
    // Listed rather than defaulted, so a new reason fails to compile until someone decides.
    case DeviceFaultReason::None:
    case DeviceFaultReason::WifiUnavailable:
    case DeviceFaultReason::BoardMismatch:
    case DeviceFaultReason::DisplayInitializationFailure:
    case DeviceFaultReason::LowMemory:
    case DeviceFaultReason::TaskWatchdog:
    case DeviceFaultReason::OtaValidationFailure:
        return false;
    }

    return false;
}

bool operator==(const BootStatus& lhs, const BootStatus& rhs) {
    return lhs.phase == rhs.phase && lhs.reset_reason == rhs.reset_reason &&
           lhs.board_target == rhs.board_target && lhs.flash_validation == rhs.flash_validation &&
           lhs.psram_validation == rhs.psram_validation && lhs.storage == rhs.storage &&
           lhs.safe_mode == rhs.safe_mode;
}

bool operator!=(const BootStatus& lhs, const BootStatus& rhs) {
    return !(lhs == rhs);
}

bool operator==(const ConfigurationState& lhs, const ConfigurationState& rhs) {
    return lhs.status == rhs.status && lhs.schema_version == rhs.schema_version &&
           lhs.credential_generation == rhs.credential_generation &&
           lhs.has_wifi_credentials == rhs.has_wifi_credentials &&
           lhs.has_printer_configuration == rhs.has_printer_configuration &&
           lhs.printer_configuration_pending == rhs.printer_configuration_pending &&
           lhs.printer_address == rhs.printer_address && lhs.printer_alias == rhs.printer_alias &&
           lhs.auth_method == rhs.auth_method;
}

bool operator!=(const ConfigurationState& lhs, const ConfigurationState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const NetworkState& lhs, const NetworkState& rhs) {
    return lhs.phase == rhs.phase && lhs.ip_address == rhs.ip_address &&
           lhs.signal_known == rhs.signal_known &&
           lhs.signal_strength_dbm == rhs.signal_strength_dbm &&
           lhs.last_connected_at == rhs.last_connected_at &&
           lhs.consecutive_failures == rhs.consecutive_failures && lhs.last_error == rhs.last_error;
}

bool operator!=(const NetworkState& lhs, const NetworkState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const PrinterConnectionState& lhs, const PrinterConnectionState& rhs) {
    return lhs.phase == rhs.phase && lhs.endpoint == rhs.endpoint &&
           lhs.auth_method == rhs.auth_method && lhs.capability_tier == rhs.capability_tier &&
           lhs.last_attempt_at == rhs.last_attempt_at &&
           lhs.last_success_at == rhs.last_success_at &&
           lhs.consecutive_failures == rhs.consecutive_failures &&
           lhs.response_code_known == rhs.response_code_known &&
           lhs.last_response_code == rhs.last_response_code &&
           lhs.latency_known == rhs.latency_known && lhs.last_latency_ms == rhs.last_latency_ms &&
           lhs.last_error == rhs.last_error && lhs.freshness == rhs.freshness;
}

bool operator!=(const PrinterConnectionState& lhs, const PrinterConnectionState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const DisplayState& lhs, const DisplayState& rhs) {
    return lhs.power == rhs.power && lhs.brightness_percent == rhs.brightness_percent &&
           lhs.idle_mode == rhs.idle_mode && lhs.last_interaction_at == rhs.last_interaction_at;
}

bool operator!=(const DisplayState& lhs, const DisplayState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const OtaState& lhs, const OtaState& rhs) {
    return lhs.phase == rhs.phase && lhs.available_version == rhs.available_version &&
           lhs.progress == rhs.progress && lhs.error == rhs.error &&
           lhs.pending_validation == rhs.pending_validation &&
           lhs.notice_spent == rhs.notice_spent && lhs.notice_presenting == rhs.notice_presenting;
}

bool operator!=(const OtaState& lhs, const OtaState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const DiagnosticsSummary& lhs, const DiagnosticsSummary& rhs) {
    return lhs.events_applied == rhs.events_applied &&
           lhs.events_rejected_stale == rhs.events_rejected_stale &&
           lhs.events_out_of_order == rhs.events_out_of_order &&
           lhs.unknown_printer_states_seen == rhs.unknown_printer_states_seen &&
           lhs.out_of_range_progress_seen == rhs.out_of_range_progress_seen;
}

bool operator!=(const DiagnosticsSummary& lhs, const DiagnosticsSummary& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
