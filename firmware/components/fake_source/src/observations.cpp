// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "fake_source/observations.hpp"

#include <string_view>

namespace prd::fake_source {
namespace {

// The job every fixture and scenario prints. One filename throughout, so a log
// read on the bench never has to work out which job it is looking at.
constexpr std::string_view kJobId = "job-0001";
constexpr std::string_view kJobName = "CORE One.gcode";
constexpr std::string_view kJobPath = "/usb/CORE One.gcode";

constexpr std::uint32_t kTotalLayers = 250;

// Nominal PLA temperatures, and the mid-heat figures the accepted Preparing
// artwork shows.
constexpr float kNozzleTarget = 215.0F;
constexpr float kBedTarget = 60.0F;
constexpr float kNozzleHeating = 145.0F;
constexpr float kBedHeating = 45.0F;
constexpr float kAmbient = 22.0F;

// Seconds. A two-hour print, so elapsed and remaining are both plausible at any
// progress figure.
constexpr std::uint32_t kJobDurationSeconds = 7200;

app_model::TemperatureValue at_temperature(const float celsius) {
    return app_model::TemperatureValue{true, celsius};
}

} // namespace

app_model::HardwareProbeObservation healthy_hardware() {
    app_model::HardwareProbeObservation probe;

    probe.board_target.assign("waveshare_esp32_s3_lcd_2_8");
    probe.reset_reason.assign("power-on");
    probe.flash_validation = app_model::ValidationState::Validated;
    probe.psram_validation = app_model::ValidationState::Validated;

    return probe;
}

app_model::HardwareProbeObservation mismatched_hardware() {
    app_model::HardwareProbeObservation probe = healthy_hardware();

    // §12.4's unsupported-hardware condition: the expected capacity is not what the
    // board reports.
    probe.psram_validation = app_model::ValidationState::Failed;

    return probe;
}

app_model::ConfigurationObservation valid_configuration(const std::uint32_t credential_generation) {
    app_model::ConfigurationObservation configuration;

    configuration.status = app_model::ConfigurationStatus::Valid;
    configuration.schema_version = 1;
    configuration.credential_generation = credential_generation;
    configuration.has_wifi_credentials = true;
    configuration.has_printer_configuration = true;
    configuration.printer_address.assign("printer.local");
    configuration.printer_alias.assign("Workshop");
    configuration.auth_method = app_model::AuthMethod::ApiKey;

    return configuration;
}

app_model::ConfigurationObservation missing_configuration() {
    app_model::ConfigurationObservation configuration;

    configuration.status = app_model::ConfigurationStatus::Missing;
    configuration.schema_version = 1;

    return configuration;
}

app_model::NetworkObservation network_connected() {
    app_model::NetworkObservation observation;

    observation.phase = app_model::NetworkPhase::Connected;
    observation.ip_address.assign("192.168.1.42");
    observation.signal_known = true;
    observation.signal_strength_dbm = -54;

    return observation;
}

app_model::NetworkObservation network_connecting() {
    app_model::NetworkObservation observation;

    observation.phase = app_model::NetworkPhase::Connecting;

    return observation;
}

app_model::ConnectionObservation connection(const app_model::ConnectionPhase phase) {
    app_model::ConnectionObservation observation;

    observation.phase = phase;
    observation.endpoint.assign("printer.local:80");
    observation.auth_method = app_model::AuthMethod::ApiKey;
    observation.capability_tier.assign("core-one");

    switch (phase) {
    case app_model::ConnectionPhase::Connected:
        observation.response_code_known = true;
        observation.response_code = 200;
        observation.latency_known = true;
        observation.latency_ms = 38;
        break;

    case app_model::ConnectionPhase::Unauthorized:
        observation.response_code_known = true;
        observation.response_code = 401;
        observation.error.assign("api key rejected");
        break;

    case app_model::ConnectionPhase::UnsupportedApi:
        observation.response_code_known = true;
        observation.response_code = 404;
        observation.error.assign("status endpoint absent");
        break;

    case app_model::ConnectionPhase::PrinterUnavailable:
        observation.error.assign("connection refused");
        break;

    case app_model::ConnectionPhase::InvalidResponse:
        observation.response_code_known = true;
        observation.response_code = 200;
        observation.error.assign("malformed required field");
        break;

    // The transport phases carry no response yet, by definition.
    case app_model::ConnectionPhase::Unconfigured:
    case app_model::ConnectionPhase::WaitingForNetwork:
    case app_model::ConnectionPhase::Resolving:
    case app_model::ConnectionPhase::Probing:
    case app_model::ConnectionPhase::Authenticating:
    case app_model::ConnectionPhase::Degraded:
        break;
    }

    return observation;
}

app_model::PrinterSnapshotObservation snapshot(const app_model::PrinterState state,
                                               const std::uint32_t sequence) {
    app_model::PrinterSnapshotObservation observation;

    observation.sequence = sequence;
    observation.normalized = state;
    observation.raw.assign(app_model::log_label(state));

    switch (state) {
    case app_model::PrinterState::Preparing:
        observation.is_busy = true;
        observation.nozzle_current = at_temperature(kNozzleHeating);
        observation.nozzle_target = at_temperature(kNozzleTarget);
        observation.bed_current = at_temperature(kBedHeating);
        observation.bed_target = at_temperature(kBedTarget);
        break;

    case app_model::PrinterState::Printing:
        observation.is_busy = true;
        observation.nozzle_current = at_temperature(kNozzleTarget);
        observation.nozzle_target = at_temperature(kNozzleTarget);
        observation.bed_current = at_temperature(kBedTarget);
        observation.bed_target = at_temperature(kBedTarget);
        break;

    case app_model::PrinterState::Paused:
    case app_model::PrinterState::Finished:
    case app_model::PrinterState::Attention:
    case app_model::PrinterState::Error:
        observation.requires_attention = state == app_model::PrinterState::Attention;
        observation.nozzle_current = at_temperature(kNozzleTarget);
        observation.nozzle_target = at_temperature(kNozzleTarget);
        observation.bed_current = at_temperature(kBedTarget);
        observation.bed_target = at_temperature(kBedTarget);
        break;

    case app_model::PrinterState::Stopped:
        // Cooling, with the targets released.
        observation.nozzle_current = at_temperature(200.0F);
        observation.bed_current = at_temperature(kBedTarget);
        break;

    case app_model::PrinterState::Idle:
    case app_model::PrinterState::Ready:
        observation.nozzle_current = at_temperature(kAmbient);
        observation.bed_current = at_temperature(kAmbient);
        break;

    // Nothing is known, so nothing is reported. Absence stays absent — a fixture
    // must never be the thing that invents a placeholder.
    case app_model::PrinterState::Unknown:
    case app_model::PrinterState::Offline:
        break;
    }

    return observation;
}

app_model::PrinterIdentity printer_identity() {
    app_model::PrinterIdentity identity;

    identity.reported_name.assign("CORE One");
    identity.hostname.assign("printer.local");
    identity.firmware_version.assign("6.2.3");
    identity.api_version.assign("1.0.0");
    identity.model.assign("CORE One");

    return identity;
}

app_model::JobObservation job(const std::uint32_t sequence, const JobProgress progress) {
    app_model::JobObservation observation;

    observation.sequence = sequence;
    observation.id.assign(kJobId);
    observation.display_name.assign(kJobName);
    observation.original_file_name.assign(kJobName);
    observation.source_path.assign(kJobPath);
    observation.normalized_state = app_model::PrinterState::Printing;
    observation.raw_state.assign("PRINTING");
    observation.progress = app_model::ProgressValue{true, progress.percent};
    observation.layers = app_model::LayerProgress{true, progress.layer, kTotalLayers};
    observation.estimate_quality = app_model::EstimateQuality::Normal;

    // Elapsed and remaining derived from progress, so they always agree with the
    // percentage rather than drifting from it.
    const std::uint32_t elapsed = kJobDurationSeconds * progress.percent / 100U;
    observation.elapsed = app_model::DurationValue{true, elapsed};
    observation.remaining = app_model::DurationValue{true, kJobDurationSeconds - elapsed};

    return observation;
}

app_model::JobObservation job_with_rough_estimate(const std::uint32_t sequence,
                                                  const JobProgress progress) {
    app_model::JobObservation observation = job(sequence, progress);
    observation.estimate_quality = app_model::EstimateQuality::Approximate;
    return observation;
}

app_model::AlertObservation attention_alert() {
    app_model::AlertObservation alert;

    alert.severity = app_model::AlertSeverity::Warning;
    alert.source = app_model::AlertSource::StatusPrinter;
    alert.code.assign("filament-runout");
    alert.title.assign("Filament runout");
    alert.message.assign("Insert filament to continue");

    // §7.10 and §24.18: never actionable unless the action came from verified data.
    alert.is_actionable = false;

    return alert;
}

app_model::AlertObservation error_alert() {
    app_model::AlertObservation alert;

    alert.severity = app_model::AlertSeverity::Error;
    alert.source = app_model::AlertSource::PrinterState;
    alert.code.assign("thermal-runaway");
    alert.title.assign("Printer error");
    alert.message.assign("Heating failed");
    alert.is_actionable = false;

    return alert;
}

app_model::OtaObservation ota(const app_model::OtaPhase phase, const std::uint8_t percent) {
    app_model::OtaObservation observation;

    observation.phase = phase;
    observation.available_version.assign("0.2.0");

    switch (phase) {
    case app_model::OtaPhase::Downloading:
    case app_model::OtaPhase::Verifying:
    case app_model::OtaPhase::Installing:
        observation.progress = app_model::ProgressValue{true, percent};
        break;

    case app_model::OtaPhase::Failed:
        observation.error.assign("digest mismatch");
        break;

    case app_model::OtaPhase::Validating:
        observation.pending_validation = true;
        break;

    case app_model::OtaPhase::Idle:
    case app_model::OtaPhase::Checking:
    case app_model::OtaPhase::Available:
    case app_model::OtaPhase::RebootRequired:
        break;
    }

    return observation;
}

} // namespace prd::fake_source
