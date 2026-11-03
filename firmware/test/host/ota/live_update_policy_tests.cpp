// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/live_update.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "app_model/reducer.hpp"

namespace prd::ota {
namespace {

TEST(LiveUpdatePolicyTests, OnlyKnownDownloadProgressMayCoalesce) {
    LiveReport report;
    report.kind = LiveReportKind::Observation;
    report.observation.phase = app_model::OtaPhase::Downloading;
    report.observation.progress = {true, 25};
    EXPECT_EQ(saturation_policy(report), LiveReportSaturationPolicy::Coalesce);

    report.observation.progress = {};
    EXPECT_EQ(saturation_policy(report), LiveReportSaturationPolicy::Preserve);

    report.observation.phase = app_model::OtaPhase::Verifying;
    report.observation.progress = {true, 100};
    EXPECT_EQ(saturation_policy(report), LiveReportSaturationPolicy::Preserve);

    report.observation.phase = app_model::OtaPhase::Failed;
    EXPECT_EQ(saturation_policy(report), LiveReportSaturationPolicy::Preserve);

    report.kind = LiveReportKind::Available;
    EXPECT_EQ(saturation_policy(report), LiveReportSaturationPolicy::Preserve);
}

TEST(LiveUpdatePolicyTests, InFlightDiscoveryCannotReplaceAnAdmittedInstallSession) {
    constexpr std::uint32_t installed_generation = 41;
    constexpr std::uint32_t later_generation = 42;

    LiveReport later_discovery;
    later_discovery.generation = later_generation;
    later_discovery.kind = LiveReportKind::Available;
    later_discovery.observation.phase = app_model::OtaPhase::Available;
    const auto rejected_install = failed_live_install(installed_generation, "0.2.0");

    // Both reports already exist before the app task drains either one. The worker has released
    // its admission, but the app-owned generation latch must survive until Failed(G) is handled.
    const std::array queued_reports{later_discovery, rejected_install};
    std::uint32_t displayed_generation = installed_generation;
    std::uint32_t admitted_generation = installed_generation;
    app_model::AppState state;
    state = app_model::reduce(state,
                              app_model::Event::ota_observed({}, accepted_live_install("0.2.0")));
    std::vector<app_model::OtaPhase> drained_phases;

    for (const auto& report : queued_reports) {
        if (!live_report_belongs_to_session(displayed_generation, admitted_generation, report)) {
            continue;
        }
        if (report.kind == LiveReportKind::Available) {
            displayed_generation = report.generation;
        }
        state = app_model::reduce(state, app_model::Event::ota_observed({}, report.observation));
        drained_phases.push_back(report.observation.phase);
        if (report.observation.phase == app_model::OtaPhase::Failed) {
            displayed_generation = 0;
            admitted_generation = 0;
        }
    }

    ASSERT_EQ(drained_phases.size(), 1U);
    EXPECT_EQ(drained_phases.front(), app_model::OtaPhase::Failed);
    EXPECT_EQ(state.ota.phase, app_model::OtaPhase::Failed);
    EXPECT_EQ(state.ota.error.view(), "Update failed");
    EXPECT_EQ(displayed_generation, 0U);
    EXPECT_EQ(admitted_generation, 0U);
}

TEST(LiveUpdatePolicyTests, AcceptedHandoffIsTruthfulExclusiveStart) {
    const auto observation = accepted_live_install("0.2.0");
    EXPECT_EQ(observation.phase, app_model::OtaPhase::Downloading);
    EXPECT_EQ(observation.available_version.view(), "0.2.0");
    EXPECT_TRUE(observation.progress.known);
    EXPECT_EQ(observation.progress.percent, 0);
    EXPECT_TRUE(app_model::ota_holds_device(observation.phase));
}

TEST(LiveUpdatePolicyTests, EarlyWorkerRejectionProducesTerminalFailure) {
    const auto report = failed_live_install(42, "0.2.0");
    EXPECT_EQ(report.generation, 42U);
    EXPECT_EQ(report.kind, LiveReportKind::Observation);
    EXPECT_EQ(report.observation.phase, app_model::OtaPhase::Failed);
    EXPECT_EQ(report.observation.available_version.view(), "0.2.0");
    EXPECT_EQ(report.observation.error.view(), "Update failed");
}

} // namespace
} // namespace prd::ota
