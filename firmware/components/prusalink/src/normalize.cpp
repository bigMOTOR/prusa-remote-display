// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "normalize.hpp"

#include <cmath>
#include <cstdint>

namespace prd::prusalink {
namespace {

// Assigns a temperature only when the printer actually reported a usable number.
void assign_temperature(app_model::TemperatureValue& out, const bool known, const double value) {
    if (!known || !temperature_is_representable(value)) {
        return;
    }

    out.known = true;
    out.celsius = static_cast<float>(value);
}

// A percentage the domain can hold, rounded to the byte it is stored in.
//
// Rounded rather than truncated: the contract's own example is 42.0 and a firmware reporting 99.6
// means the print is nearly done, not 99. Half-up is the same convention the display's brightness
// curve uses.
void assign_progress(app_model::ProgressValue& out, const bool known, const double percent) {
    if (!known || !std::isfinite(percent)) {
        return;
    }

    // ⛔ NEGATIVE IS UNUSABLE, NOT ZERO, AND THE TEST IS ON THE REPORTED VALUE. A printer that
    // reports -1 has not told us the print is at the beginning; it has told us something this
    // build cannot read as a percentage. §9.3 keeps absent and zero distinguishable, and turning
    // a refusal into 0 % would put a figure on the panel that the machine never stated.
    //
    // ⬅️ CHECKED BEFORE THE ROUNDING, WHICH IS THE WHOLE POINT. Rounding first would let -0.4
    // become 0 and be published as a real reading, so the sign of what the printer actually said
    // would decide nothing. The upper bound is checked AFTER, because that is where rounding can
    // carry a value out of range: 255.6 is in range as reported and is not representable once
    // rounded.
    if (percent < 0.0) {
        return;
    }

    const double rounded = std::floor(percent + 0.5);

    if (rounded > kMaxReportedProgressPercent) {
        return;
    }

    out.known = true;
    out.percent = static_cast<std::uint8_t>(rounded);
}

// A duration in whole seconds the domain can hold.
void assign_duration(app_model::DurationValue& out, const bool known, const double seconds) {
    if (!known || !std::isfinite(seconds)) {
        return;
    }

    // ⛔ A NEGATIVE REMAINING TIME IS A REFUSAL, NOT A ZERO. v6.8.1 emits `time_remaining` only
    // when it holds a valid estimate, so a negative value is a firmware this build does not
    // understand rather than a print that is overdue — and "0 s remaining" is a claim the printer
    // did not make. `DurationValue::seconds` is unsigned, so there is nowhere truthful to put it.
    //
    // Sign before the rounding, magnitude after — same reasoning as assign_progress() above.
    if (seconds < 0.0) {
        return;
    }

    const double rounded = std::floor(seconds + 0.5);

    if (rounded > kMaxReportedDurationSeconds) {
        return;
    }

    out.known = true;
    out.seconds = static_cast<std::uint32_t>(rounded);
}

} // namespace

app_model::PrinterState state_from(const std::string_view raw) {
    if (raw == "IDLE") {
        return app_model::PrinterState::Idle;
    }

    if (raw == "READY") {
        return app_model::PrinterState::Ready;
    }

    // §5.1: BUSY covers the preparation phases, without saying which.
    if (raw == "BUSY") {
        return app_model::PrinterState::Preparing;
    }

    if (raw == "PRINTING") {
        return app_model::PrinterState::Printing;
    }

    if (raw == "PAUSED") {
        return app_model::PrinterState::Paused;
    }

    if (raw == "FINISHED") {
        return app_model::PrinterState::Finished;
    }

    if (raw == "STOPPED") {
        return app_model::PrinterState::Stopped;
    }

    if (raw == "ATTENTION") {
        return app_model::PrinterState::Attention;
    }

    if (raw == "ERROR") {
        return app_model::PrinterState::Error;
    }

    // Includes the empty string, and any value a future firmware introduces.
    //
    // ⛔ Offline is deliberately unreachable from here. It is a connection fact, and
    // §7.5's closing rule — repeated as §24.5 — forbids inferring connection state
    // from printer operational state. A printer that cannot be reached produces no
    // snapshot at all; it produces a connection observation.
    return app_model::PrinterState::Unknown;
}

bool temperature_is_representable(const double celsius) {
    // isfinite first: NaN fails every comparison below, including the ones that look as though
    // they would reject it.
    return std::isfinite(celsius) && celsius >= kMinReportedCelsius &&
           celsius <= kMaxReportedCelsius;
}

app_model::PrinterSnapshotObservation snapshot_from(const StatusDto& dto,
                                                    const app_model::SnapshotOrder order) {
    app_model::PrinterSnapshotObservation observation;

    observation.generation = order.generation;
    observation.sequence = order.sequence;
    observation.raw = dto.printer_state;
    observation.normalized = state_from(dto.printer_state.view());

    switch (observation.normalized) {
    // The machine is occupied with work. §7.7's isBusy is about occupation, which is
    // why Paused counts: the job still owns the printer.
    case app_model::PrinterState::Preparing:
    case app_model::PrinterState::Printing:
    case app_model::PrinterState::Paused:
        observation.is_busy = true;
        break;

    case app_model::PrinterState::Attention:
    case app_model::PrinterState::Error:
        observation.requires_attention = true;
        break;

    case app_model::PrinterState::Unknown:
    case app_model::PrinterState::Idle:
    case app_model::PrinterState::Ready:
    case app_model::PrinterState::Finished:
    case app_model::PrinterState::Stopped:
    case app_model::PrinterState::Offline:
        break;
    }

    assign_temperature(observation.nozzle_current, dto.temp_nozzle_known, dto.temp_nozzle);
    assign_temperature(observation.nozzle_target, dto.target_nozzle_known, dto.target_nozzle);
    assign_temperature(observation.bed_current, dto.temp_bed_known, dto.temp_bed);
    assign_temperature(observation.bed_target, dto.target_bed_known, dto.target_bed);

    // Chamber values stay unknown. §17.6 of the API research records that the
    // documented status schema carries no chamber telemetry, and §24.18 forbids
    // inventing what the printer did not report.

    return observation;
}

JobTelemetry job_telemetry_of(const StatusDto& dto) {
    return dto.job_present ? JobTelemetry::Present : JobTelemetry::Absent;
}

app_model::StatusObservation status_from(const StatusDto& dto,
                                         const app_model::SnapshotOrder order) {
    app_model::StatusObservation observation;

    observation.printer = snapshot_from(dto, order);
    observation.job.reported = job_telemetry_of(dto) == JobTelemetry::Present;

    // ⛔ THE FOUR VALUES THE ENDPOINT SUPPLIES, AND NOTHING ELSE. No name, no file, no path, no
    // layer, no estimate quality — the endpoint states none of them, and StatusJobTelemetry has
    // nowhere to put them. No generation, no sequence and no state either: those belong to the
    // reading, and `observation.printer` already carries them.
    // ⛔ THE FACT AND THE VALUE, BOTH. `job_id_known` is what stops the reducer reading an absent
    // id as an empty one and concluding the job changed — see StatusJobTelemetry::id_known.
    observation.job.id_known = dto.job_id_known;

    if (dto.job_id_known) {
        observation.job.id = dto.job_id;
    }

    assign_progress(observation.job.progress, dto.progress_known, dto.progress_percent);
    assign_duration(observation.job.remaining, dto.time_remaining_known,
                    dto.time_remaining_seconds);
    assign_duration(observation.job.elapsed, dto.time_printing_known, dto.time_printing_seconds);

    return observation;
}

void apply_version(const VersionDto& dto, app_model::PrinterIdentity& identity) {
    if (!dto.api.empty()) {
        identity.api_version = dto.api;
    }

    if (!dto.firmware.empty()) {
        identity.firmware_version = dto.firmware;
    }

    if (!dto.text.empty()) {
        identity.model = dto.text;
    }
}

} // namespace prd::prusalink
