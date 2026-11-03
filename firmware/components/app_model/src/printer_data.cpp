// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/printer_data.hpp"

namespace prd::app_model {

std::string_view log_label(const PrinterState state) {
    switch (state) {
    case PrinterState::Unknown:
        return "unknown";

    case PrinterState::Idle:
        return "idle";

    case PrinterState::Ready:
        return "ready";

    case PrinterState::Preparing:
        return "preparing";

    case PrinterState::Printing:
        return "printing";

    case PrinterState::Paused:
        return "paused";

    case PrinterState::Attention:
        return "attention";

    case PrinterState::Error:
        return "error";

    case PrinterState::Finished:
        return "finished";

    case PrinterState::Stopped:
        return "stopped";

    case PrinterState::Offline:
        return "offline";
    }

    return "unknown";
}

std::string_view log_label(const EstimateQuality quality) {
    switch (quality) {
    case EstimateQuality::Unknown:
        return "unknown";

    case EstimateQuality::Approximate:
        return "approximate";

    case EstimateQuality::Normal:
        return "normal";
    }

    return "unknown";
}

std::string_view log_label(const AlertSeverity severity) {
    switch (severity) {
    case AlertSeverity::Info:
        return "info";

    case AlertSeverity::Warning:
        return "warning";

    case AlertSeverity::Error:
        return "error";
    }

    return "unknown";
}

std::string_view log_label(const AlertSource source) {
    switch (source) {
    case AlertSource::PrinterState:
        return "printer-state";

    case AlertSource::StatusPrinter:
        return "status-printer";

    case AlertSource::ResponseMetadata:
        return "response-metadata";

    case AlertSource::LocalConnection:
        return "local-connection";

    case AlertSource::Firmware:
        return "firmware";
    }

    return "unknown";
}

bool operator==(const PrinterIdentity& lhs, const PrinterIdentity& rhs) {
    return lhs.display_name == rhs.display_name && lhs.reported_name == rhs.reported_name &&
           lhs.configured_alias == rhs.configured_alias && lhs.hostname == rhs.hostname &&
           lhs.serial == rhs.serial && lhs.firmware_version == rhs.firmware_version &&
           lhs.api_version == rhs.api_version && lhs.model == rhs.model;
}

bool operator!=(const PrinterIdentity& lhs, const PrinterIdentity& rhs) {
    return !(lhs == rhs);
}

bool operator==(const PrinterOperationalState& lhs, const PrinterOperationalState& rhs) {
    return lhs.normalized == rhs.normalized && lhs.raw == rhs.raw && lhs.is_busy == rhs.is_busy &&
           lhs.requires_attention == rhs.requires_attention && lhs.observed_at == rhs.observed_at &&
           lhs.ever_observed == rhs.ever_observed;
}

bool operator!=(const PrinterOperationalState& lhs, const PrinterOperationalState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const TemperatureState& lhs, const TemperatureState& rhs) {
    return lhs.nozzle_current == rhs.nozzle_current && lhs.nozzle_target == rhs.nozzle_target &&
           lhs.bed_current == rhs.bed_current && lhs.bed_target == rhs.bed_target &&
           lhs.chamber_current == rhs.chamber_current && lhs.chamber_target == rhs.chamber_target &&
           lhs.observed_at == rhs.observed_at;
}

bool operator!=(const TemperatureState& lhs, const TemperatureState& rhs) {
    return !(lhs == rhs);
}

bool operator==(const PrintJob& lhs, const PrintJob& rhs) {
    return lhs.present == rhs.present && lhs.id == rhs.id && lhs.display_name == rhs.display_name &&
           lhs.original_file_name == rhs.original_file_name && lhs.source_path == rhs.source_path &&
           lhs.normalized_state == rhs.normalized_state && lhs.raw_state == rhs.raw_state &&
           lhs.progress == rhs.progress && lhs.elapsed == rhs.elapsed &&
           lhs.remaining == rhs.remaining && lhs.layers == rhs.layers &&
           lhs.estimate_quality == rhs.estimate_quality &&
           lhs.locally_inferred_started_at == rhs.locally_inferred_started_at &&
           lhs.last_updated_at == rhs.last_updated_at &&
           lhs.reported_missing == rhs.reported_missing &&
           lhs.reported_missing_at == rhs.reported_missing_at;
}

bool operator!=(const PrintJob& lhs, const PrintJob& rhs) {
    return !(lhs == rhs);
}

bool operator==(const CompletedJobSnapshot& lhs, const CompletedJobSnapshot& rhs) {
    return lhs.present == rhs.present && lhs.id == rhs.id && lhs.display_name == rhs.display_name &&
           lhs.terminal_state == rhs.terminal_state && lhs.progress == rhs.progress &&
           lhs.layers == rhs.layers && lhs.completed_at == rhs.completed_at;
}

bool operator!=(const CompletedJobSnapshot& lhs, const CompletedJobSnapshot& rhs) {
    return !(lhs == rhs);
}

bool operator==(const PrinterAlert& lhs, const PrinterAlert& rhs) {
    return lhs.active == rhs.active && lhs.severity == rhs.severity && lhs.source == rhs.source &&
           lhs.code == rhs.code && lhs.title == rhs.title && lhs.message == rhs.message &&
           lhs.help_url == rhs.help_url && lhs.is_actionable == rhs.is_actionable &&
           lhs.observed_at == rhs.observed_at;
}

bool operator!=(const PrinterAlert& lhs, const PrinterAlert& rhs) {
    return !(lhs == rhs);
}

} // namespace prd::app_model
