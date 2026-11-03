// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>
#include <cstdint>

#include "app_model/app_state.hpp"
#include "app_model/events.hpp"
#include "app_model/printer_data.hpp"
#include "app_model/reducer.hpp"

// Shared builders for the reducer and projection suites.
//
// These exist so a test reads as the scenario it describes rather than as ten lines
// of field assignment. Every one of them goes through the real reducer: a test must
// never hand-assemble an AppState that the reducer could not have produced, or it
// would be asserting against a state the product cannot reach.

namespace prd::test {

constexpr app_model::Timestamp at_ms(const std::int64_t milliseconds) {
    return std::chrono::duration_cast<app_model::Timestamp>(
        std::chrono::milliseconds{milliseconds});
}

// A device that has finished booting with healthy hardware and a valid
// configuration. The starting point for anything about the printer, because until
// boot resolves, the projection is showing the boot screen whatever else is true.
inline app_model::AppState booted_device(const app_model::Timestamp now = at_ms(0)) {
    app_model::AppState state;

    app_model::HardwareProbeObservation probe;
    probe.board_target.assign("waveshare_esp32_s3_lcd_2_8");
    probe.reset_reason.assign("power-on");
    probe.flash_validation = app_model::ValidationState::Validated;
    probe.psram_validation = app_model::ValidationState::Validated;

    state = app_model::reduce(state, app_model::Event::hardware_probed(now, probe));
    state = app_model::reduce(
        state, app_model::Event::storage_observed(now, app_model::StorageStatus::Ready));

    app_model::ConfigurationObservation configuration;
    configuration.status = app_model::ConfigurationStatus::Valid;
    configuration.schema_version = 1;
    configuration.has_wifi_credentials = true;
    configuration.has_printer_configuration = true;
    configuration.printer_address.assign("printer.local");
    configuration.auth_method = app_model::AuthMethod::ApiKey;

    state = app_model::reduce(state, app_model::Event::configuration_observed(now, configuration));
    state =
        app_model::reduce(state, app_model::Event::boot_phase(now, app_model::BootPhase::Ready));

    return state;
}

// A connected printer connection. Kept separate from the printer snapshot on
// purpose: §24.5 forbids inferring one from the other, and a test that conflated
// them would hide a regression in exactly that rule.
inline app_model::AppState with_connection(app_model::AppState state,
                                           const app_model::ConnectionPhase phase,
                                           const app_model::Timestamp now) {
    app_model::ConnectionObservation observation;
    observation.phase = phase;
    observation.endpoint.assign("printer.local:80");
    observation.auth_method = app_model::AuthMethod::ApiKey;
    observation.capability_tier.assign("core-one");
    observation.response_code_known = true;
    observation.response_code = 200;
    observation.latency_known = true;
    observation.latency_ms = 42;

    return app_model::reduce(state, app_model::Event::connection_observed(now, observation));
}

inline app_model::PrinterSnapshotObservation printer_snapshot(const app_model::PrinterState value,
                                                              const std::uint32_t sequence) {
    app_model::PrinterSnapshotObservation observation;

    observation.sequence = sequence;
    observation.normalized = value;
    observation.raw.assign(app_model::log_label(value));
    observation.is_busy =
        value == app_model::PrinterState::Printing || value == app_model::PrinterState::Preparing;
    observation.nozzle_current = app_model::TemperatureValue{true, 215.0F};
    observation.nozzle_target = app_model::TemperatureValue{true, 215.0F};
    observation.bed_current = app_model::TemperatureValue{true, 60.0F};
    observation.bed_target = app_model::TemperatureValue{true, 60.0F};

    return observation;
}

// Applies a printer snapshot at `now`. The sequence advances with each call so the
// §8.1 rejection rule does not quietly discard the second one.
inline app_model::AppState with_printer_state(app_model::AppState state,
                                              const app_model::PrinterState value,
                                              const app_model::Timestamp now,
                                              const std::uint32_t sequence) {
    return app_model::reduce(
        state, app_model::Event::printer_snapshot(now, printer_snapshot(value, sequence)));
}

inline app_model::JobObservation printing_job(const std::uint32_t sequence) {
    app_model::JobObservation observation;

    observation.sequence = sequence;
    observation.id.assign("job-1");
    observation.display_name.assign("CORE One.gcode");
    observation.original_file_name.assign("CORE One.gcode");
    observation.source_path.assign("/usb/CORE One.gcode");
    observation.normalized_state = app_model::PrinterState::Printing;
    observation.raw_state.assign("PRINTING");
    observation.progress = app_model::ProgressValue{true, 62};
    observation.elapsed = app_model::DurationValue{true, 3600};
    observation.remaining = app_model::DurationValue{true, 4680};
    observation.layers = app_model::LayerProgress{true, 156, 250};
    observation.estimate_quality = app_model::EstimateQuality::Normal;

    return observation;
}

// A printer actively printing, with a job: the canonical operational state.
inline app_model::AppState printing_device(const app_model::Timestamp now = at_ms(0)) {
    app_model::AppState state = booted_device(now);

    state = with_connection(state, app_model::ConnectionPhase::Connected, now);
    state = with_printer_state(state, app_model::PrinterState::Printing, now, 1);
    state = app_model::reduce(state, app_model::Event::job_observed(now, printing_job(1)));

    return state;
}

} // namespace prd::test
