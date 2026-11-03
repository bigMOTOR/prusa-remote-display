// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/events.hpp"
#include "ota/installation.hpp"

namespace prd::ota {

enum class LiveReportKind : std::uint8_t { Available, Observation };

struct LiveReport {
    std::uint32_t generation{0};
    LiveReportKind kind{LiveReportKind::Observation};
    app_model::OtaObservation observation;
};

struct LiveConfig {
    InstallationPort* port{nullptr};
    InstallationAdmission* admission{nullptr};
    DeviceCompatibility device;
};

enum class LiveReportSaturationPolicy : std::uint8_t { Preserve, Coalesce };

// The application owns both identities. The admitted generation is latched on the application
// task and survives the worker releasing its admission before its terminal report is drained.
// While it is non-zero, no discovery may replace the admitted install session.
[[nodiscard]] constexpr bool live_report_belongs_to_session(const std::uint32_t current_generation,
                                                            const std::uint32_t admitted_generation,
                                                            const LiveReport& report) {
    if (report.kind == LiveReportKind::Available) {
        return admitted_generation == 0;
    }
    const auto expected_generation =
        admitted_generation == 0 ? current_generation : admitted_generation;
    return report.generation == expected_generation;
}

// §10.7: byte progress may be coalesced; availability and every phase/terminal transition must
// survive queue saturation. Kept pure so the exact production classification is host-tested.
[[nodiscard]] constexpr LiveReportSaturationPolicy saturation_policy(const LiveReport& report) {
    return report.kind == LiveReportKind::Observation &&
                   report.observation.phase == app_model::OtaPhase::Downloading &&
                   report.observation.progress.known
               ? LiveReportSaturationPolicy::Coalesce
               : LiveReportSaturationPolicy::Preserve;
}

[[nodiscard]] inline app_model::OtaObservation
accepted_live_install(const std::string_view version) {
    app_model::OtaObservation observation;
    observation.phase = app_model::OtaPhase::Downloading;
    observation.available_version.assign(version);
    observation.progress = {true, 0};
    return observation;
}

[[nodiscard]] inline LiveReport failed_live_install(const std::uint32_t generation,
                                                    const std::string_view version) {
    LiveReport report;
    report.generation = generation;
    report.kind = LiveReportKind::Observation;
    report.observation.phase = app_model::OtaPhase::Failed;
    report.observation.available_version.assign(version);
    report.observation.error.assign("Update failed");
    return report;
}

// One bounded OTA worker. Discovery is background-only; install is accepted only for the exact
// generation most recently reported Available. Reports are drained and session-filtered by the
// serialized application task.
[[nodiscard]] bool start_live_update(const LiveConfig& config);
[[nodiscard]] std::uint32_t request_release_check();
[[nodiscard]] bool request_live_install(std::uint32_t generation);
[[nodiscard]] bool take_live_report(LiveReport& report);

// Local capacity fact used to assemble DeviceCompatibility without exposing ESP-IDF partition
// types above this component.
[[nodiscard]] std::uint64_t device_inactive_slot_size();

} // namespace prd::ota
