// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "support/state_builders.hpp"

// The projection suite.
//
// Two things are being protected. First, UIAD 001 §12's priority order, exactly and
// in both directions: each layer must win over everything below it and yield to
// everything above it. Second, the rule that makes the accepted UI possible at all —
// absent data stays absent, so no scene can render a placeholder without inventing
// one.

using namespace prd::app_model;
using prd::test::at_ms;
using prd::test::booted_device;
using prd::test::printer_snapshot;
using prd::test::printing_device;
using prd::test::printing_job;
using prd::test::with_connection;
using prd::test::with_printer_state;

namespace {

TEST(ProjectionAgeTests, CarriesStatusAnchorOnlyWhilePresentableAndDoesNotWake) {
    auto state = printing_device(at_ms(1234));
    state = reduce(state, Event::confidence_aged(at_ms(21234)));
    const auto view = project(state);
    ASSERT_TRUE(view.stale_indicator);
    ASSERT_TRUE(view.stale_observed_at.has_value());
    EXPECT_EQ(*view.stale_observed_at, state.printer.observed_at);
    const auto power = state.display;
    state = reduce(state, Event::confidence_aged(at_ms(22234)));
    EXPECT_EQ(project(state).stale_observed_at, view.stale_observed_at);
    EXPECT_EQ(state.display, power);
    state = reduce(state, Event::confidence_aged(at_ms(62235)));
    EXPECT_FALSE(project(state).stale_observed_at.has_value());
}

TEST(ProjectionAgeTests, LastObservedProgressSurvivesOnlyThePresentableStaleWindow) {
    auto state = printing_device(at_ms(1000));
    const auto original = project(state).progress;
    ASSERT_TRUE(original.known);
    for (const auto age : {16000U, 30000U, 59000U}) {
        state = reduce(state, Event::confidence_aged(at_ms(1000 + age)));
        const auto view = project(state);
        ASSERT_EQ(view.confidence, Confidence::Degraded);
        EXPECT_TRUE(view.stale_indicator);
        EXPECT_TRUE(view.stale_observed_at.has_value());
        EXPECT_TRUE(view.progress.known);
        EXPECT_EQ(view.progress.percent, original.percent);
        EXPECT_FALSE(view.remaining.known);
        EXPECT_FALSE(view.nozzle.current.known);
    }
    state = reduce(state, Event::confidence_aged(at_ms(62000)));
    const auto expired = project(state);
    EXPECT_EQ(expired.confidence, Confidence::Unavailable);
    EXPECT_FALSE(expired.progress.known);
    EXPECT_FALSE(expired.show_operational_form);
}

TEST(ProjectionAgeTests, DegradedNeverInventsMissingProgress) {
    auto state = printing_device(at_ms(1000));
    state.current_job.progress = {};
    state = reduce(state, Event::confidence_aged(at_ms(31000)));
    EXPECT_TRUE(project(state).stale_indicator);
    EXPECT_FALSE(project(state).progress.known);
}

// A printing device with a modal surface open, provisioning required, and a
// recovery condition — every lower layer simultaneously live, so a test can assert
// which one wins rather than which one happens to be the only candidate.
AppState contested_state() {
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    state = reduce(state, Event::provisioning_phase(at_ms(1100), ProvisioningPhase::SetupRequired));
    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(1200));
    state = reduce(state, Event::recovery_mode_entered(at_ms(1300)));

    return state;
}

// --- D-4: a pending printer is not an established printer ------------------
//
// Milestone 3.8 persists a printer address and key the device has NEVER CONTACTED (R4).
// The recorded M3.8 prerequisite is that this must not reach the Operational presentation
// as though validation had happened.

TEST(ProjectionTests, APendingPrinterConfigurationIsProjectedAsPending) {
    AppState state;
    state.configuration.printer_configuration_pending = true;

    EXPECT_TRUE(project(state).printer_configuration_pending);
}

TEST(ProjectionTests, APendingPrinterContributesNoPrinterName) {
    AppState state;
    state.configuration.printer_configuration_pending = true;
    state.configuration.has_printer_configuration = false;
    state.configuration.printer_address.assign("192.168.1.50");
    state.configuration.printer_alias.assign("Workshop");

    const Presentation view = project(state);

    // Neither the address nor the alias may name a printer that has never answered.
    EXPECT_FALSE(view.printer_name_known);
    EXPECT_TRUE(view.printer_name.empty());
}

TEST(ProjectionTests, AValidatedPrinterStillContributesItsConfiguredName) {
    AppState state;
    state.configuration.printer_configuration_pending = false;
    state.configuration.has_printer_configuration = true;
    state.configuration.printer_alias.assign("Workshop");
    // Confidence::Current so the ladder itself is under test. A default-constructed state
    // carries Confidence::Unavailable, where §9 hides identity outright — that gate is
    // asserted by PrinterIdentityDoesNotSurviveLossOfObservation, not here.
    state.confidence = Confidence::Current;

    const Presentation view = project(state);

    EXPECT_TRUE(view.printer_name_known);
    EXPECT_EQ(view.printer_name.view(), "Workshop");
}

TEST(ProjectionTests, AValidatedPrinterFallsBackToItsAddressWhenNothingElseIsKnown) {
    AppState state;
    state.configuration.has_printer_configuration = true;
    state.configuration.printer_address.assign("192.168.1.50");
    // Confidence::Current so the ladder itself is under test. A default-constructed state
    // carries Confidence::Unavailable, where §9 hides identity outright — that gate is
    // asserted by PrinterIdentityDoesNotSurviveLossOfObservation, not here.
    state.confidence = Confidence::Current;

    const Presentation view = project(state);

    EXPECT_TRUE(view.printer_name_known);
    EXPECT_EQ(view.printer_name.view(), "192.168.1.50");
}

// What the printer itself reported is never gated: those fields can only be populated by a
// printer that answered, which is Milestone 4's business.
TEST(ProjectionTests, AReportedNameSurvivesEvenWhileTheConfigurationIsPending) {
    AppState state;
    state.configuration.printer_configuration_pending = true;
    state.configuration.printer_address.assign("192.168.1.50");
    state.printer_identity.reported_name.assign("CORE One");
    // Confidence::Current so the ladder itself is under test. A default-constructed state
    // carries Confidence::Unavailable, where §9 hides identity outright — that gate is
    // asserted by PrinterIdentityDoesNotSurviveLossOfObservation, not here.
    state.confidence = Confidence::Current;

    const Presentation view = project(state);

    EXPECT_TRUE(view.printer_name_known);
    EXPECT_EQ(view.printer_name.view(), "CORE One");
}

} // namespace

// --- Deterministic priority (UIAD 001 §12) ---------------------------------

TEST(PresentationPriorityTests, DeviceFaultWinsOverEveryOtherLayer) {
    AppState state = contested_state();

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    state = reduce(state, Event::ota_observed(at_ms(1400), installing));

    state = reduce(state, Event::device_fault(at_ms(1500), DeviceFaultReason::TaskWatchdog));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::DeviceFault);
    EXPECT_EQ(view.device_fault, DeviceFaultReason::TaskWatchdog);
}

TEST(PresentationPriorityTests, ExclusiveOperationWinsOverEverythingBelowIt) {
    AppState state = contested_state();

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    installing.available_version.assign("0.2.0");
    installing.progress = ProgressValue{true, 40};
    state = reduce(state, Event::ota_observed(at_ms(1400), installing));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::ExclusiveOperation);
    EXPECT_EQ(view.device_operation, DeviceOperation::FirmwareUpdating);
    EXPECT_EQ(view.ota_phase, OtaPhase::Installing);
    EXPECT_EQ(view.update_version.view(), "0.2.0");

    // Production UI §9: firmware update progress is never the operational ring.
    EXPECT_TRUE(view.update_progress.known);
    EXPECT_FALSE(view.show_operational_form);
    EXPECT_FALSE(view.show_telemetry);
}

TEST(PresentationPriorityTests, UpdateProgressIsClampedOnlyInThePresentation) {
    auto state = printing_device(at_ms(0));
    OtaObservation downloading;
    downloading.phase = OtaPhase::Downloading;
    downloading.progress = ProgressValue{true, 255};
    state = reduce(state, Event::ota_observed(at_ms(1000), downloading));

    ASSERT_TRUE(state.ota.progress.known);
    EXPECT_EQ(state.ota.progress.percent, 255U);
    const auto view = project(state);
    ASSERT_TRUE(view.update_progress.known);
    EXPECT_EQ(view.update_progress.percent, 100U);
}

TEST(PresentationPriorityTests, StartupWinsOverProvisioningAccessAndSubject) {
    // UIAD 001 §4: Starting is startup "before normal operation, provisioning or
    // recovery has been resolved", so nothing below it can be trusted yet.
    AppState state = contested_state();

    state = reduce(state, Event::boot_phase(at_ms(1400), BootPhase::StartingServices));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::SystemLifecycle);
    EXPECT_EQ(view.boot_phase, BootPhase::StartingServices);
}

TEST(PresentationPriorityTests, RecoveryOrProvisioningWinsOverAccessAndModal) {
    const AppState state = contested_state();

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::RecoveryOrProvisioning);
    EXPECT_EQ(view.provisioning, ProvisioningPhase::SetupRequired);
    EXPECT_EQ(view.device_operation, DeviceOperation::RecoveryMode);

    // Production UI §9 Initial Setup: printer telemetry is hidden, and no simulated
    // printer information is shown.
    EXPECT_FALSE(view.show_telemetry);
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.nozzle.current.known);
}

TEST(PresentationPriorityTests, AccessConditionWinsOverModalAndSubject) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(1100));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::AccessCondition);
    EXPECT_EQ(view.access_condition, AccessCondition::AuthenticationRequired);

    // §8: this is neither a subject state nor a device fault, and it must not be
    // reported as either.
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.device_operation, DeviceOperation::None);
    EXPECT_EQ(view.device_fault, DeviceFaultReason::None);
}

TEST(PresentationPriorityTests, ModalSurfaceWinsOverSubjectButKeepsItAlive) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::ModalSurface);
    EXPECT_EQ(view.modal_surface, ModalSurface::FactoryResetConfirmation);

    // UIAD 001 §9: "The underlying Subject State, conditions and confidence
    // continue to exist while they are presented." A modal draws over a live
    // composition, not over a blank screen.
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.confidence, Confidence::Current);
    EXPECT_TRUE(view.show_telemetry);
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.progress.known);
}

TEST(PresentationPriorityTests, SubjectPresentationIsTheRestingResolution) {
    const Presentation view = project(printing_device(at_ms(0)));

    EXPECT_EQ(view.layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.subject_condition, SubjectCondition::None);
}

TEST(PresentationPriorityTests, EachLayerYieldsToTheOneAboveInSequence) {
    // The order read as a single narrowing sequence: remove the top condition and
    // the next one must surface, all the way down.
    AppState state = contested_state();

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    state = reduce(state, Event::ota_observed(at_ms(1400), installing));
    state = reduce(state, Event::device_fault(at_ms(1500), DeviceFaultReason::LowMemory));
    state = reduce(state, Event::boot_phase(at_ms(1600), BootPhase::StartingServices));

    EXPECT_EQ(project(state).layer, PresentationLayer::DeviceFault);

    // Cleared by hand, deliberately: there is no fault-cleared event, because a
    // device fault persists until the device restarts. Walking down the ladder needs
    // the condition removed, and this is the only honest way to do it — the value
    // itself is one the reducer produces (it is the default), only the transition
    // back to it is unmodelled.
    state.device_fault = DeviceFaultReason::None;
    state = reduce(state, Event::confidence_aged(at_ms(1700)));
    EXPECT_EQ(project(state).layer, PresentationLayer::ExclusiveOperation);

    OtaObservation idle;
    idle.phase = OtaPhase::Idle;
    state = reduce(state, Event::ota_observed(at_ms(1800), idle));
    EXPECT_EQ(project(state).layer, PresentationLayer::SystemLifecycle);

    state = reduce(state, Event::boot_phase(at_ms(1900), BootPhase::Ready));
    EXPECT_EQ(project(state).layer, PresentationLayer::RecoveryOrProvisioning);

    state = reduce(state, Event::recovery_mode_exited(at_ms(2000)));
    state = reduce(state, Event::provisioning_phase(at_ms(2100), ProvisioningPhase::Inactive));
    EXPECT_EQ(project(state).layer, PresentationLayer::AccessCondition);

    state = with_connection(state, ConnectionPhase::Connected, at_ms(2200));
    EXPECT_EQ(project(state).layer, PresentationLayer::ModalSurface);

    state = reduce(state, Event::modal_surface_dismissed(at_ms(2300)));
    EXPECT_EQ(project(state).layer, PresentationLayer::SubjectPresentation);
}

// --- Confidence in the presentation ---------------------------------------

TEST(PresentationConfidenceTests, CurrentShowsTelemetryWithoutAStaleMarker) {
    const Presentation view = project(printing_device(at_ms(0)));

    EXPECT_EQ(view.confidence, Confidence::Current);
    EXPECT_FALSE(view.stale_indicator);
    EXPECT_TRUE(view.show_telemetry);
    EXPECT_TRUE(view.remaining.known);
}

// --- §10: confidence tightens presence (OF-9, Phase G.5) --------------------
//
// THE COMPLETE DURABLE / DECAYED TABLE. §10: "values that decay cease to be true and become
// absent, while durable values remain". The split is derived, not chosen — a value decays when
// its meaning is "as of now", which is precisely the claim an unobservable machine no longer
// supports.

namespace {

// A live print the display has stopped hearing from, long enough to matter.
//
// 30 s is past the 15 s aging window and well inside the 60 s expiry, so the observation is Stale
// — the level UIAD 001 §7 calls Degraded and the one §10 speaks about.
//
// IT WAS 5 s UNTIL 2026-08-13, when the correction made Aging map to Current: routine missed polls
// must not become visible (UI Behaviour Decision 001 §4). These tests are about what §10 decays at
// Degraded, not about which level produces it, so they now reach it through the level that still
// does. The change of number is the point of the correction showing up in the suite.
[[nodiscard]] AppState degraded_printing() {
    AppState state = printing_device(at_ms(0));
    return reduce(state, Event::confidence_aged(at_ms(30000)));
}

} // namespace

TEST(PresentationConfidenceTests, DegradedRetainsMarkedProgressButRemovesLiveMeasurements) {
    const Presentation view = project(degraded_printing());

    ASSERT_EQ(view.confidence, Confidence::Degraded);

    // Temperatures are measurements at an instant, and this instant has none.
    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.nozzle.target.known);
    EXPECT_FALSE(view.bed.current.known);
    EXPECT_FALSE(view.bed.target.known);
    EXPECT_FALSE(view.chamber.current.known);

    // §11.5's own named archetype: "sensitive dynamic values such as remaining time are removed".
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.remaining_is_approximate);
    EXPECT_FALSE(view.elapsed.known);

    // Owner ruling: retained progress is explicitly historical, never advanced locally.
    EXPECT_TRUE(view.progress.known);
    EXPECT_TRUE(view.stale_indicator);
    EXPECT_EQ(view.progress.percent, project(printing_device(at_ms(0))).progress.percent);
    EXPECT_FALSE(view.layers.known);
}

TEST(PresentationConfidenceTests, DegradedKeepsTheDurableValuesAndStatesItselfExplicitly) {
    const Presentation view = project(degraded_printing());

    // §11.5: what remains is shown "only with an explicit stale indicator", and §10 requires
    // confidence to state itself rather than be inferred from what is missing.
    EXPECT_TRUE(view.stale_indicator);

    // Identity is not telemetry. §9 Printer Unavailable keeps "Printer identity (if known)"
    // even at Unavailable, so Degraded certainly keeps it.
    EXPECT_TRUE(view.printer_name_known);
    EXPECT_TRUE(view.job_name_known) << "the job is still the job it was";

    // §10: "A degraded Printing remains Printing at maximum energy. The machine has not slowed;
    // it has become unobservable." Subject, membership and component presence are untouched.
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.subject_condition, SubjectCondition::None);
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.show_telemetry) << "the column is membership; its rows answer §7 one by one";
}

TEST(PresentationConfidenceTests, TheRecordAndLastObservedProgressSurviveDegraded) {
    // THE DISTINCTION G.5 EXISTS TO MAKE. "The machine never reported a record" and "the record
    // is not presentable right now" are different facts, and the composition's shape follows the
    // first while its content follows the second.
    const Presentation view = project(degraded_printing());

    EXPECT_TRUE(view.progress.known) << "last observed percentage is retained";
    EXPECT_TRUE(view.job_record_supplied) << "the record is not";
}

TEST(PresentationConfidenceTests, ATerminalJobsRecordSurvivesReducedConfidence) {
    // §7's worked example, and the one asymmetry in the table: "A terminal job's layer count is
    // still true — it is the count the machine reached — and is therefore present." A finished
    // job's figures cannot move, so losing contact does not make them false.
    const PrinterState terminal[] = {PrinterState::Finished, PrinterState::Stopped};

    for (const auto reported : terminal) {
        AppState state = printing_device(at_ms(0));
        state = with_printer_state(state, reported, at_ms(1000), 2);
        state = reduce(state, Event::confidence_aged(at_ms(31000)));

        const Presentation view = project(state);
        ASSERT_EQ(view.confidence, Confidence::Degraded) << log_label(reported);

        EXPECT_TRUE(view.progress.known) << log_label(reported) << ": the count it reached";
        EXPECT_TRUE(view.layers.known) << log_label(reported);

        // The machine's own readings still decay — they are about the machine now, not about
        // the job that ended.
        EXPECT_FALSE(view.nozzle.current.known) << log_label(reported);
        EXPECT_FALSE(view.bed.current.known) << log_label(reported);
        EXPECT_FALSE(view.remaining.known) << log_label(reported);
    }
}

TEST(PresentationConfidenceTests, APausedJobIsLiveRatherThanTerminalForThisPurpose) {
    // Paused retains explicitly historical progress, just like Printing.
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Paused, at_ms(1000), 2);
    state = reduce(state, Event::confidence_aged(at_ms(31000)));

    const Presentation view = project(state);
    ASSERT_EQ(view.subject, SubjectState::Paused);
    ASSERT_EQ(view.confidence, Confidence::Degraded);

    EXPECT_TRUE(view.progress.known);
    EXPECT_TRUE(view.stale_indicator);
    EXPECT_FALSE(view.layers.known);
}

TEST(PresentationConfidenceTests, CurrentConfidenceDecaysNothing) {
    // The control. Without it every assertion above would pass for a projection that simply
    // never reported a value.
    const Presentation view = project(printing_device(at_ms(0)));

    ASSERT_EQ(view.confidence, Confidence::Current);
    EXPECT_TRUE(view.nozzle.current.known);
    EXPECT_TRUE(view.bed.current.known);
    EXPECT_TRUE(view.remaining.known);
    EXPECT_TRUE(view.elapsed.known);
    EXPECT_TRUE(view.progress.known);
    EXPECT_TRUE(view.layers.known);
    EXPECT_FALSE(view.stale_indicator);
}

TEST(PresentationConfidenceTests, DecayTakesAwayAndNeverAdds) {
    // §10 TIGHTENS §7 rather than replacing it, so every value present at Degraded must also
    // have been present at Current. Walked field by field so a future rule that resurrected
    // something under reduced confidence fails here rather than on the panel.
    const Presentation current = project(printing_device(at_ms(0)));
    const Presentation degraded = project(degraded_printing());

    const auto tightens = [](const bool before, const bool after, const char* const field) {
        EXPECT_TRUE(before || !after) << field << " became present as confidence fell";
    };

    tightens(current.nozzle.current.known, degraded.nozzle.current.known, "nozzle");
    tightens(current.bed.current.known, degraded.bed.current.known, "bed");
    tightens(current.remaining.known, degraded.remaining.known, "remaining");
    tightens(current.elapsed.known, degraded.elapsed.known, "elapsed");
    tightens(current.progress.known, degraded.progress.known, "progress");
    tightens(current.layers.known, degraded.layers.known, "layers");
    tightens(current.printer_name_known, degraded.printer_name_known, "printer name");
    tightens(current.job_name_known, degraded.job_name_known, "job name");
    tightens(current.show_telemetry, degraded.show_telemetry, "telemetry");
    tightens(current.show_operational_form, degraded.show_operational_form, "form");
}

TEST(PresentationConfidenceTests, UnavailablePresentsNothingAsCurrent) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at_ms(120000)));

    const Presentation view = project(state);

    // §7: "Unavailable means no previous Subject State may be presented as
    // current", and Production UI §9 Printer Unavailable: "Previously displayed
    // telemetry shall not be presented as current data."
    EXPECT_EQ(view.confidence, Confidence::Unavailable);
    EXPECT_FALSE(view.show_telemetry);
    EXPECT_FALSE(view.show_operational_form);
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.job_name_known);

    // A stale marker over nothing would be meaningless.
    EXPECT_FALSE(view.stale_indicator);
}

// ⛔ REVERSED BY THE ACCEPTED §9 REVISION OF 2026-08-31, and kept as the negative control for the
// behaviour it used to assert.
//
// It read: "§9 Printer Unavailable keeps 'Printer identity (if known)' — it is configuration, not
// telemetry, and it did not go stale." That was a correct reading of the old §9. The revised §9
// removes identity from the Visible list and hides it "at all times", because identity is not
// truthfully available here and a name beside "Printer unavailable" reads as a claim about THAT
// printer. This test now fails if the old behaviour returns.
TEST(PresentationConfidenceTests, PrinterIdentityDoesNotSurviveLossOfObservation) {
    AppState state = printing_device(at_ms(0));

    // Degraded first: identity DOES survive here. Confidence and validity are different questions,
    // and a stale printer is still a printer the device can name.
    state = reduce(state, Event::confidence_aged(at_ms(30000)));
    ASSERT_EQ(state.confidence, Confidence::Degraded);
    const Presentation degraded = project(state);
    EXPECT_TRUE(degraded.printer_name_known) << "identity survives Degraded";
    EXPECT_EQ(degraded.printer_name.view(), "printer.local");

    // Expiry into Unavailable removes it.
    state = reduce(state, Event::confidence_aged(at_ms(600000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    const Presentation lost = project(state);
    EXPECT_FALSE(lost.printer_name_known) << "§9: printer identity is Hidden at all times";
    EXPECT_TRUE(lost.printer_name.view().empty());
}

TEST(PresentationConfidenceTests, NothingIsPresentedBeforeTheFirstObservation) {
    const Presentation view = project(booted_device(at_ms(0)));

    EXPECT_EQ(view.layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(view.subject, SubjectState::AcquiringState);
    EXPECT_EQ(view.confidence, Confidence::Unavailable);
    EXPECT_FALSE(view.show_telemetry);
    EXPECT_FALSE(view.show_operational_form);
}

// --- Absence stays absent -------------------------------------------------

TEST(PresentationAbsenceTests, UnreportedValuesAreAbsentRatherThanZero) {
    // UI Design Decision 003: "Information that does not yet exist shall remain
    // absent rather than represented by placeholders", and §9.3: absent must be
    // distinguishable from zero. A scene cannot render "---" from this structure
    // without inventing it.
    AppState state = booted_device(at_ms(0));

    PrinterSnapshotObservation sparse;
    sparse.sequence = 1;
    sparse.normalized = PrinterState::Ready;
    sparse.raw.assign("IDLE");
    // No temperature reported at all.

    state = reduce(state, Event::printer_snapshot(at_ms(100), sparse));

    const Presentation view = project(state);

    EXPECT_EQ(view.subject, SubjectState::Ready);
    EXPECT_EQ(view.confidence, Confidence::Current);

    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.bed.current.known);
    EXPECT_FALSE(view.chamber.current.known);
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.job_name_known);
}

TEST(PresentationAbsenceTests, ZeroIsCarriedAsAKnownValue) {
    // The converse: a bed genuinely at 0 °C is a fact and must not be mistaken for
    // an absent reading.
    AppState state = booted_device(at_ms(0));

    PrinterSnapshotObservation cold;
    cold.sequence = 1;
    cold.normalized = PrinterState::Ready;
    cold.bed_current = TemperatureValue{true, 0.0F};

    state = reduce(state, Event::printer_snapshot(at_ms(100), cold));

    const Presentation view = project(state);

    EXPECT_TRUE(view.bed.current.known);
    EXPECT_FLOAT_EQ(view.bed.current.celsius, 0.0F);
}

// --- Shared components (Production UI §8, §9) -----------------------------

TEST(PresentationComponentTests, EveryFamilyMemberCarriesTheOperationalForm) {
    // UI Design Decision 004 §13.1: the inventory of primitives is constant across
    // the six operational states — "no state introduces an element another state
    // lacks". §6 already made the Ready Horizon a transformation of the Progress
    // Ring rather than an independent primitive, so there is one component, and the
    // only question here is whether the composition draws it.
    //
    // This replaces three earlier tests. Two of them asserted which of two shapes a
    // state carried, which the projection no longer decides — the shape follows the
    // state's energy (scenes/operational_form.hpp). The third pinned that the two
    // shapes were never present at once, an invariant a single flag cannot break.
    struct Case {
        PrinterState reported;
        SubjectState expected;
    };

    const Case cases[] = {
        {PrinterState::Ready, SubjectState::Ready},
        {PrinterState::Preparing, SubjectState::Preparing},
        {PrinterState::Printing, SubjectState::Printing},
        {PrinterState::Paused, SubjectState::Paused},
        {PrinterState::Finished, SubjectState::Completed},
        {PrinterState::Stopped, SubjectState::Stopped},
    };

    for (const auto& scenario : cases) {
        AppState state = printing_device(at_ms(0));
        state = with_printer_state(state, scenario.reported, at_ms(1000), 2);

        const Presentation view = project(state);

        ASSERT_EQ(view.subject, scenario.expected)
            << "printer state " << log_label(scenario.reported);
        EXPECT_TRUE(view.show_operational_form)
            << "no form for subject " << log_label(view.subject);
    }

    // The seventh subject state, which no printer report produces. Acquiring is not
    // a member of the family — Production UI §9 gives it its own Connecting screen —
    // so it carries no form.
    const Presentation acquiring = project(booted_device(at_ms(0)));
    ASSERT_EQ(acquiring.subject, SubjectState::AcquiringState);
    EXPECT_FALSE(acquiring.show_operational_form);
}

TEST(PresentationComponentTests, StoppedKeepsItsRecordAndItsForm) {
    // §9 Print Stopped: "Visible components are identical to Printing", "Progress
    // remains visible", "Last known telemetry remains visible until printer state
    // changes."
    //
    // The divergence this test used to carry is gone. It formerly recorded that the
    // written specification and accepted artwork board 2 panel 6 disagreed about
    // whether Stopped kept the ring. UIDD 004 §12 resolves it without amending §9:
    // "progress" is the value, not the arc, so the record stays visible while the
    // form recedes with the state's energy. The component inventory is unchanged,
    // which is exactly what §9 asks for — and the inventory is what this layer owns.
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Stopped, at_ms(1000), 2);

    const Presentation view = project(state);

    EXPECT_EQ(view.subject, SubjectState::Stopped);
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.progress.known);
    EXPECT_TRUE(view.layers.known);
}

TEST(PresentationComponentTests, TheFormDoesNotRequireAKnownProgressValue) {
    // THE SEMANTIC CHANGE THIS CHECKPOINT EXISTS FOR, and the inverse of what this
    // test asserted before.
    //
    // The projection used to set the ring's presence from `progress.known`, so a
    // Printing state the printer had not yet reported progress for drew no form at
    // all. UIDD 004 §3.2 prohibits the record determining the form's shape, and
    // deleting the form outright is the stronger version of that; §13.1's constant
    // inventory forbids it independently.
    //
    // So the form is present and the record is absent. Nothing is invented to fill
    // the gap: progress stays unknown, and the scene draws neither a sweep nor a
    // numeral. The machine is printing; only the figure is missing.
    AppState state = booted_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Printing, at_ms(100), 1);

    const Presentation view = project(state);

    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_FALSE(view.progress.known);
    EXPECT_TRUE(view.show_operational_form);
}

// --- Values, not formatting ----------------------------------------------

TEST(PresentationValueTests, ProgressIsClampedForDisplayOnly) {
    AppState state = printing_device(at_ms(0));

    JobObservation impossible = printing_job(2);
    impossible.progress = ProgressValue{true, 137};
    state = reduce(state, Event::job_observed(at_ms(1000), impossible));

    // §7.8: clamped for display; the state keeps what the printer reported.
    EXPECT_EQ(state.current_job.progress.percent, 137U);
    EXPECT_EQ(project(state).progress.percent, 100U);
}

TEST(PresentationValueTests, ApproximateEstimatesAreFlaggedNotFormatted) {
    // §17.7 requires an "approximate marker for inaccurate remaining time". The
    // projection reports that the value is an estimate; how the marker is drawn is
    // the scene's business.
    AppState state = printing_device(at_ms(0));

    JobObservation rough = printing_job(2);
    rough.estimate_quality = EstimateQuality::Approximate;
    state = reduce(state, Event::job_observed(at_ms(1000), rough));

    const Presentation view = project(state);

    EXPECT_TRUE(view.remaining.known);
    EXPECT_EQ(view.remaining.seconds, 4680U);
    EXPECT_TRUE(view.remaining_is_approximate);
}

TEST(PresentationValueTests, PrinterNameFallsBackThroughTheAcceptedOrder) {
    // §9.4: "what fallback printer name to use". The user's own alias wins, then
    // what the printer calls itself, then the address they typed.
    //
    // ⬅️ PROJECTED FROM AN OBSERVED COPY. booted_device() has observed nothing, so its confidence
    // is Unavailable — and §9, as revised 2026-08-31, hides identity there outright. That gate is
    // asserted by PrinterIdentityDoesNotSurviveLossOfObservation; what THIS test owns is the
    // ordering of the name candidates, which is a different rule and still applies.
    const auto observed = [](AppState value) {
        value.confidence = Confidence::Current;
        return value;
    };

    AppState state = booted_device(at_ms(0));
    EXPECT_EQ(project(observed(state)).printer_name.view(), "printer.local");

    PrinterIdentity identity;
    identity.reported_name.assign("CORE One");
    state = reduce(state, Event::printer_identity(at_ms(100), identity));
    EXPECT_EQ(project(observed(state)).printer_name.view(), "CORE One");

    ConfigurationObservation configuration;
    configuration.status = ConfigurationStatus::Valid;
    configuration.schema_version = 1;
    configuration.has_wifi_credentials = true;
    configuration.has_printer_configuration = true;
    configuration.printer_address.assign("printer.local");
    configuration.printer_alias.assign("Workshop");
    configuration.auth_method = AuthMethod::ApiKey;
    state = reduce(state, Event::configuration_observed(at_ms(200), configuration));
    EXPECT_EQ(project(observed(state)).printer_name.view(), "Workshop");
}

TEST(PresentationValueTests, AnUnknownPrinterNameIsAbsent) {
    const Presentation view = project(AppState{});

    EXPECT_FALSE(view.printer_name_known);
    EXPECT_TRUE(view.printer_name.empty());
}

// --- Conditions surface whatever is drawn over them -----------------------

TEST(PresentationConditionTests, AttentionModifiesRatherThanReplacingThePresentation) {
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Attention, at_ms(1000), 2);

    const Presentation view = project(state);

    // Production UI §9 Attention Required: "The interface shall preserve operational
    // continuity while emphasizing the attention condition."
    EXPECT_EQ(view.layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.subject_condition, SubjectCondition::Attention);
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.show_telemetry);
}

TEST(PresentationConditionTests, AnActiveAlertIsCarriedUnderAModalSurface) {
    // UIAD 001 §11: a presentation drawn on top must not block a persistent
    // condition.
    AppState state = printing_device(at_ms(0));

    AlertObservation alert;
    alert.severity = AlertSeverity::Error;
    alert.title.assign("Thermal runaway");
    alert.message.assign("Printer stopped heating");
    state = reduce(state, Event::alert_raised(at_ms(1000), alert));
    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::VeryLongPress));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::ModalSurface);
    EXPECT_TRUE(view.alert.active);
    EXPECT_EQ(view.alert.severity, AlertSeverity::Error);
    EXPECT_EQ(view.alert.title.view(), "Thermal runaway");
    EXPECT_EQ(view.subject_condition, SubjectCondition::Error);
}

TEST(PresentationConditionTests, AnActiveAlertSurvivesADeviceFaultTakingOver) {
    AppState state = printing_device(at_ms(0));

    AlertObservation alert;
    alert.severity = AlertSeverity::Warning;
    alert.title.assign("Filament runout");
    state = reduce(state, Event::alert_raised(at_ms(1000), alert));
    state = reduce(state, Event::device_fault(at_ms(2000), DeviceFaultReason::LowMemory));

    const Presentation view = project(state);

    EXPECT_EQ(view.layer, PresentationLayer::DeviceFault);
    EXPECT_TRUE(view.alert.active);
}

// --- Determinism ----------------------------------------------------------

TEST(PresentationDeterminismTests, ProjectionIsAPureFunctionOfTheState) {
    const AppState state = printing_device(at_ms(1000));

    EXPECT_EQ(project(state), project(state));
}

TEST(PresentationDeterminismTests, EqualStatesProjectEqually) {
    const AppState first = printing_device(at_ms(1000));
    const AppState second = printing_device(at_ms(1000));

    ASSERT_EQ(first, second);
    EXPECT_EQ(project(first), project(second));
}

// --- Aging is invisible (UI Behaviour Decision 001 §4, 2026-08-13) ----------

TEST(ProjectionTests, AgingProducesExactlyTheSamePresentationAsFresh) {
    // THE WHOLE POINT OF THE CORRECTION, asserted where the user would notice it.
    //
    // §4: "Routine polling, retries and small telemetry changes shall not become
    // visible." Aging is one to two missed polls at the active interval, so the
    // panel must be bit-for-bit unchanged. Comparing whole presentations rather than
    // named fields means a value added to Presentation later cannot start moving
    // during aging without this failing.
    const AppState fresh = printing_device(at_ms(0));
    const AppState aging = reduce(fresh, Event::confidence_aged(at_ms(5000)));

    ASSERT_EQ(aging.printer_connection.freshness, DataFreshness::Aging);
    EXPECT_EQ(project(aging), project(fresh));
}

TEST(ProjectionTests, TheStaleLevelStillDegradesAndSaysSo) {
    // The level below aging is untouched: reduced confidence, stated explicitly
    // (UIDD 004 §10), with the decayed values gone (§7 as §10 tightens it).
    const AppState fresh = printing_device(at_ms(0));
    const AppState stale = reduce(fresh, Event::confidence_aged(at_ms(30000)));

    ASSERT_EQ(stale.printer_connection.freshness, DataFreshness::Stale);

    const auto view = project(stale);
    EXPECT_EQ(view.confidence, Confidence::Degraded);
    EXPECT_TRUE(view.stale_indicator);
    EXPECT_NE(view, project(fresh));

    // Still Printing at Printing's own energy — §10 forbids confidence changing the
    // subject or the form, and this correction did not touch that.
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_TRUE(view.show_operational_form);
}

TEST(ProjectionTests, ExpiryStillLeavesTheOperationalPresentation) {
    // §16.3's "after expiry" branch, and UIAD 001 §7's prohibition on presenting a
    // previous subject state as current. Unchanged by the correction.
    const AppState expired =
        reduce(printing_device(at_ms(0)), Event::confidence_aged(at_ms(120000)));

    const auto view = project(expired);
    EXPECT_EQ(view.confidence, Confidence::Unavailable);
}

// --- Milestone 3.11 T18: the recovery -> setup -> expiry lifecycle ----------
//
// ⛔ WHAT THESE PROTECT, AND WHY THE MODEL IS THE RIGHT PLACE. `state.recovery_mode` used to be
// cleared by exactly one thing — entering BootPhase::Starting, which is a reboot. A deliberate long
// press raises a privileged session WITHOUT rebooting, so RecoveryMode outlived the condition it
// describes and the device sat in RecoveryMode and SetupActive at once.
//
// That combination is not merely untidy. ProvisioningScene's recovery branch returns early: it
// clears the network, password and address rows and draws §8's Status Message in their place. On
// the bench the panel therefore told the reader to hold the button to start setup — while setup was
// already running — and withheld the password they needed to join. Found by T18 on hardware.
//
// The scene reads `presentation.device_operation`, and the session reaches it through a separate
// setter that never enters the presentation model, so what is assertable here is the exact guard
// that branch tests. The composition itself is confirmed on the panel.

TEST(RecoveryLifecycleTests, ADeliberateLongPressLeavesRecoveryAndOpensSetup) {
    AppState state = booted_device(at_ms(0));

    state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));
    ASSERT_EQ(project(state).device_operation, DeviceOperation::RecoveryMode);

    // The two events app_core dispatches for a long press out of recovery, in that order.
    state = reduce(state, Event::recovery_mode_exited(at_ms(2000)));
    state = reduce(state, Event::provisioning_phase(at_ms(2100), ProvisioningPhase::SetupActive));

    const Presentation view = project(state);

    EXPECT_FALSE(state.recovery_mode);
    EXPECT_EQ(view.provisioning, ProvisioningPhase::SetupActive);

    // ⛔ THE DEFECT, STATED AS THE PROPERTY. This is the condition ProvisioningScene's recovery
    // branch tests before returning early, so while it holds the Status Message is not drawn and
    // the session rows are not cleared.
    EXPECT_NE(view.device_operation, DeviceOperation::RecoveryMode);

    // The tier is unchanged — UIAD 001 §12.3 groups recovery with the provisioning phases, and a
    // session belongs to that tier on its own account.
    EXPECT_EQ(view.layer, PresentationLayer::RecoveryOrProvisioning);
}

TEST(RecoveryLifecycleTests, EveryPhaseThatOffersANetworkIsFreeOfRecovery) {
    // The rows and the QR are drawn for all three phases that keep the access point up, so the
    // guard must be clear for each of them and not only for SetupActive.
    for (const ProvisioningPhase phase :
         {ProvisioningPhase::SetupActive, ProvisioningPhase::ValidatingConfiguration,
          ProvisioningPhase::SetupFailed}) {
        AppState state = booted_device(at_ms(0));

        state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));
        state = reduce(state, Event::recovery_mode_exited(at_ms(2000)));
        state = reduce(state, Event::provisioning_phase(at_ms(2100), phase));

        const Presentation view = project(state);

        EXPECT_EQ(view.provisioning, phase);
        EXPECT_NE(view.device_operation, DeviceOperation::RecoveryMode);
    }
}

TEST(RecoveryLifecycleTests, AnExpiredRecoverySessionComesBackToRecovery) {
    // Owner ruling 2026-08-21. The session ended without replacing the faulted record, so the
    // fault stands and recovery is the truthful resting state. These are the events app_core
    // dispatches once reentry::after_expiry() asks for it — the phase first, then recovery, so no
    // projection ever sees recovery beside a session phase.
    AppState state = booted_device(at_ms(0));

    state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));
    state = reduce(state, Event::recovery_mode_exited(at_ms(2000)));
    state = reduce(state, Event::provisioning_phase(at_ms(2100), ProvisioningPhase::SetupActive));
    ASSERT_NE(project(state).device_operation, DeviceOperation::RecoveryMode);

    state = reduce(state, Event::provisioning_phase(at_ms(3000), ProvisioningPhase::Inactive));
    state = reduce(state, Event::recovery_mode_entered(at_ms(3100)));

    const Presentation view = project(state);

    EXPECT_EQ(view.device_operation, DeviceOperation::RecoveryMode);
    EXPECT_EQ(view.layer, PresentationLayer::RecoveryOrProvisioning);

    // ⛔ AND WITHOUT A SESSION PHASE BESIDE IT, which is the whole point of returning to Inactive:
    // recovery and a session are mutually exclusive by construction, not by convention.
    EXPECT_EQ(view.provisioning, ProvisioningPhase::Inactive);
}

TEST(RecoveryLifecycleTests, AnOrdinarySetupSessionNeverAcquiresRecovery) {
    // Requirement 5, and the negative control for the rule above: a first-boot session that
    // expires goes back to asking to be set up, with no recovery it never had.
    AppState state = booted_device(at_ms(0));

    state = reduce(state, Event::provisioning_phase(at_ms(1000), ProvisioningPhase::SetupActive));
    state = reduce(state, Event::provisioning_phase(at_ms(2000), ProvisioningPhase::SetupRequired));

    const Presentation view = project(state);

    EXPECT_EQ(view.provisioning, ProvisioningPhase::SetupRequired);
    EXPECT_NE(view.device_operation, DeviceOperation::RecoveryMode);
    EXPECT_FALSE(state.recovery_mode);
}

TEST(RecoveryLifecycleTests, RecoveryOnItsOwnPublishesNoSessionToJoin) {
    // Requirement 6 at the level the model owns it: entering recovery raises nothing. The phase
    // stays Inactive, so there is no network, password, address or code for any scene to draw —
    // ADR-0008 decision 3's "no access point, no mutable surface" has no session to describe.
    // That no access point is STARTED is app_core's, and T18 confirms it on hardware.
    AppState state = booted_device(at_ms(0));

    state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));

    const Presentation view = project(state);

    EXPECT_EQ(view.device_operation, DeviceOperation::RecoveryMode);
    EXPECT_EQ(view.provisioning, ProvisioningPhase::Inactive);
}

// --- The transient acknowledgement of lost observation ----------------------
//
// Production UI §9, UIAD 001 §11.1, UIBD 001 §5.1, all accepted 2026-08-31. The projection decides
// whether the acknowledgement is drawn; the controller only obeys it. These tests own that decision.

namespace {

// A printing device whose observations have expired: a real subject state is on record, and
// confidence says it may no longer be presented as current.
AppState lost_observation_state() {
    AppState state = printing_device(at_ms(0));
    state.confidence = Confidence::Unavailable;
    state.display.power = DisplayPower::On;
    return state;
}

} // namespace

TEST(LostObservationProjectionTests, TheConditionIsTrueWheneverObservationIsLost) {
    for (const DisplayPower power : {DisplayPower::On, DisplayPower::Dimmed, DisplayPower::Off}) {
        AppState state = lost_observation_state();
        state.display.power = power;

        const Presentation view = project(state);

        // ⛔ THE CONDITION OUTLIVES ITS PRESENTATION — §11.1: "Retirement of the acknowledgement
        // ends the presentation, not the condition."
        EXPECT_TRUE(view.observation_lost);
    }
}

// ⛔ THESE DRIVE THE REDUCER, NOT THE FIELDS. The three tests that stood here set
// state.display.power by hand and expected the acknowledgement flag to follow, because the flag
// WAS `Unavailable && On`. That rule was the confirmed defect; ownership is now an episode the
// reducer decides, so a test that assigns power proves nothing about it.

namespace {

// Age a real observation out through the reducer until confidence expires. The threshold itself is
// deliberately not asserted — it remains an unratified implementation baseline.
AppState lost_after_printing(const Timestamp at = at_ms(600000)) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at));
    return state;
}

} // namespace

namespace {

// The five authoritative higher-priority categories, each reached through its own event, plus the
// subject condition that is part of the subject presentation an acknowledgement would replace.
struct PriorityCase {
    const char* name;

    // What takes the panel away from the subject presentation.
    Event trigger;

    // What gives it back. Applied in order, and every test that uses it ASSERTS that eligibility
    // actually returned before drawing any conclusion from what followed.
    std::vector<Event> clear;
};

// ⛔ EVERY CASE CARRIES ITS OWN CLEAR, AND THE TESTS PROVE THE CLEAR WORKED. A generic sequence of
// boot/provisioning/recovery events was used for all of them, which left device faults, exclusive
// operations, modal surfaces and access conditions STILL IN FORCE — so "no delayed acknowledgement
// leaked through" was being asserted about a panel that was never eligible again. A vacuous pass.
std::vector<PriorityCase> priority_cases() {
    OtaObservation installing;
    installing.phase = OtaPhase::Installing;

    OtaObservation idle;
    idle.phase = OtaPhase::Idle;

    ConnectionObservation unauthorized;
    unauthorized.phase = ConnectionPhase::Unauthorized;

    ConnectionObservation connected;
    connected.phase = ConnectionPhase::Connected;

    return {
        {"device fault",
         Event::device_fault(at_ms(1000), DeviceFaultReason::TaskWatchdog),
         {Event::device_fault(at_ms(1500), DeviceFaultReason::None)}},

        {"exclusive operation",
         Event::ota_observed(at_ms(1000), installing),
         {Event::ota_observed(at_ms(1500), idle)}},

        {"unresolved boot",
         Event::boot_phase(at_ms(1000), BootPhase::StartingServices),
         {Event::boot_phase(at_ms(1500), BootPhase::Ready)}},

        {"fatal boot",
         Event::boot_phase(at_ms(1000), BootPhase::FatalError),
         {Event::boot_phase(at_ms(1500), BootPhase::Ready)}},

        {"provisioning",
         Event::provisioning_phase(at_ms(1000), ProvisioningPhase::SetupRequired),
         {Event::provisioning_phase(at_ms(1500), ProvisioningPhase::Inactive)}},

        {"recovery mode",
         Event::recovery_mode_entered(at_ms(1000)),
         {Event::recovery_mode_exited(at_ms(1500))}},

        {"modal surface",
         Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress),
         {Event::factory_reset_cancelled(at_ms(1500))}},

        // ⬅️ THE CASE THE MATRIX WAS MISSING ENTIRELY. UIAD 001 §8's access condition outranks subject
        // presentation, and §16.2 makes it NOT transient — it survives connection retries and clears
        // only on a positive reclassification. So it is exactly the priority most likely to be live
        // when a printer stops answering, and it was untested.
        {"access condition",
         Event::connection_observed(at_ms(1000), unauthorized),
         {Event::connection_observed(at_ms(1500), connected)}},
    };
}

} // namespace

TEST(LostObservationEpisodeTests, OneEligibleLossProducesExactlyOneAcknowledgement) {
    AppState state = lost_after_printing();
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    ASSERT_TRUE(state.loss_episode.active);
    EXPECT_TRUE(state.loss_episode.automatic_spent);
    EXPECT_TRUE(state.loss_episode.presenting);
    EXPECT_TRUE(project(state).observation_lost_acknowledgement);
    const std::uint32_t first = project(state).observation_lost_generation;

    for (int i = 0; i < 20; ++i) {
        state = reduce(state, Event::confidence_aged(at_ms(600000 + 1000 * (i + 1))));
    }
    EXPECT_TRUE(state.loss_episode.presenting) << "still the FIRST presentation";
    EXPECT_EQ(project(state).observation_lost_generation, first) << "no restart, so no new motion";
}

// ⛔ THE ORIGIN IS ONE POSITION PER OPERATIONAL STATE — ALL SIX, EXHAUSTIVELY. Two buckets made four
// of the six jump to an endpoint's geometry at the instant of loss, which is the discontinuity
// Candidate D's accepted contract forbids.
TEST(LostObservationEpisodeTests, TheOriginIsCapturedForEveryOperationalState) {
    struct Case {
        PrinterState reported;
        SubjectState subject;
        LossOriginForm origin;
    };

    const std::vector<Case> cases{
        {PrinterState::Idle, SubjectState::Ready, LossOriginForm::Ready},
        {PrinterState::Stopped, SubjectState::Stopped, LossOriginForm::Stopped},
        {PrinterState::Preparing, SubjectState::Preparing, LossOriginForm::Preparing},
        {PrinterState::Finished, SubjectState::Completed, LossOriginForm::Completed},
        {PrinterState::Paused, SubjectState::Paused, LossOriginForm::Paused},
        {PrinterState::Printing, SubjectState::Printing, LossOriginForm::Printing},
    };

    ASSERT_EQ(cases.size(), 6U) << "the axis has six positions and every one is covered";

    for (const Case& c : cases) {
        AppState state = booted_device(at_ms(0));
        state = with_printer_state(state, c.reported, at_ms(100), 1);
        ASSERT_EQ(state.subject, c.subject) << static_cast<int>(c.reported);

        state = reduce(state, Event::confidence_aged(at_ms(600000)));
        ASSERT_EQ(state.confidence, Confidence::Unavailable) << static_cast<int>(c.reported);
        ASSERT_TRUE(state.loss_episode.presenting) << static_cast<int>(c.reported);

        EXPECT_EQ(state.loss_episode.origin, c.origin) << static_cast<int>(c.reported);
        EXPECT_EQ(project(state).observation_lost_origin, c.origin) << static_cast<int>(c.reported);
    }

    // ⛔ FIRING CONTROL for the two-bucket collapse. Six states must produce six DISTINCT origins;
    // `{Resting, Carrying}` produced two.
    std::set<LossOriginForm> origins;
    for (const Case& c : cases) {
        AppState state = booted_device(at_ms(0));
        state = with_printer_state(state, c.reported, at_ms(100), 1);
        state = reduce(state, Event::confidence_aged(at_ms(600000)));
        origins.insert(state.loss_episode.origin);
    }
    EXPECT_EQ(origins.size(), 6U) << "two buckets would give 2";
}

// ⬅️ AND WHERE THERE WAS NO OPERATIONAL FORM AT ALL, the origin says so explicitly. §9 routes
// AcquiringState to its own Connecting screen, so nothing was on the panel to carry out.
//
// ⛔ FIRING CONTROL FOR THE B2 SCOPE LEAK. This returned LossOriginForm::Ready until 2026-09-02,
// which silently gave a loss that carried NO form the accepted Ready-only B2 entry. Mapping it back
// to Ready fails here, and fails again in the ui suite's no-form baseline test.
TEST(LostObservationEpisodeTests, AnOriginWithNoOperationalFormSaysSoExplicitly) {
    AppState state = booted_device(at_ms(0));
    ASSERT_EQ(state.subject, SubjectState::AcquiringState);

    state = reduce(state, Event::confidence_aged(at_ms(600000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_EQ(state.loss_episode.origin, LossOriginForm::NoOperationalForm);
    EXPECT_NE(state.loss_episode.origin, LossOriginForm::Ready)
        << "a loss that carried no form must not borrow the real Ready origin's entry";

    // And it is published through the seam unchanged.
    EXPECT_EQ(project(state).observation_lost_origin, LossOriginForm::NoOperationalForm);
}

// ⛔ AND THE ORIGIN CARRIES NOTHING BUT THE POSITION. Two losses from the same state, one at 12 % and
// one at 87 %, with different temperatures, layers and job names, must be indistinguishable here.
TEST(LostObservationEpisodeTests, TheOriginCarriesNoValueFromTheSubjectItLeft) {
    AppState first = printing_device(at_ms(0));
    AppState second = printing_device(at_ms(0));

    first = with_printer_state(first, PrinterState::Printing, at_ms(100), 2);
    second = with_printer_state(second, PrinterState::Printing, at_ms(100), 2);

    first = reduce(first, Event::confidence_aged(at_ms(600000)));
    second = reduce(second, Event::confidence_aged(at_ms(600000)));

    EXPECT_EQ(first.loss_episode.origin, second.loss_episode.origin);
    EXPECT_EQ(first.loss_episode.origin, LossOriginForm::Printing);

    // The projected acknowledgement fields are the origin and its identity — no reading of any kind.
    const Presentation view = project(first);
    EXPECT_TRUE(view.observation_lost);
    EXPECT_TRUE(view.observation_lost_acknowledgement);
    EXPECT_EQ(view.observation_lost_origin, LossOriginForm::Printing);

    // §9 hides the identity at Unavailable, and there is no record to project either.
    EXPECT_FALSE(view.printer_name_known);
    EXPECT_FALSE(view.job_record_supplied);
}

TEST(LostObservationEpisodeTests, RetirementHappensAtTheOrdinaryOnToDimmedTransition) {
    AppState state = lost_after_printing();
    ASSERT_TRUE(project(state).observation_lost_acknowledgement);

    state = reduce(state, Event::display_idle_timer_fired(at_ms(620000)));
    ASSERT_EQ(state.display.power, DisplayPower::Dimmed);
    EXPECT_FALSE(state.loss_episode.presenting);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);
    EXPECT_TRUE(state.loss_episode.active) << "the condition outlives its presentation";

    state = reduce(state, Event::display_sleep_timer_fired(at_ms(922000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);
}

// ⛔ FIRING CONTROL for the regressed `Unavailable && DisplayPower::On` rule.
TEST(LostObservationEpisodeTests, DisplayPowerOnAloneNeverCreatesOrReplaysIt) {
    AppState state = lost_after_printing();
    state = reduce(state, Event::display_idle_timer_fired(at_ms(620000)));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(922000)));
    ASSERT_FALSE(state.loss_episode.presenting);

    state = reduce(state, Event::display_wake(at_ms(930000)));
    ASSERT_EQ(state.display.power, DisplayPower::On);
    EXPECT_FALSE(state.loss_episode.presenting) << "an unrelated wake must not replay it";
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);

    state = reduce(state, Event::confidence_aged(at_ms(935000)));
    EXPECT_FALSE(state.loss_episode.presenting);
}

TEST(LostObservationEpisodeTests, AnExplicitShortPressFromOffReplaysIt) {
    AppState state = lost_after_printing();
    const std::uint32_t entry = project(state).observation_lost_generation;
    state = reduce(state, Event::display_idle_timer_fired(at_ms(620000)));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(922000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    state = reduce(state, Event::button_gesture(at_ms(930000), ButtonGesture::ShortPress));
    ASSERT_EQ(state.display.power, DisplayPower::On);
    EXPECT_TRUE(state.loss_episode.presenting) << "the press is the cause";
    EXPECT_TRUE(project(state).observation_lost_acknowledgement);
    EXPECT_NE(project(state).observation_lost_generation, entry) << "a replay restarts the motion";

    state = reduce(state, Event::button_gesture(at_ms(932000), ButtonGesture::ShortPress));
    ASSERT_EQ(state.display.power, DisplayPower::Off);
    EXPECT_FALSE(state.loss_episode.presenting);
}

TEST(LostObservationEpisodeTests, RecoveryClosesTheEpisodeImmediately) {
    AppState state = lost_after_printing();
    ASSERT_TRUE(state.loss_episode.presenting);

    state = with_printer_state(state, PrinterState::Printing, at_ms(601000), 2);
    EXPECT_NE(state.confidence, Confidence::Unavailable);
    EXPECT_FALSE(state.loss_episode.active);
    EXPECT_FALSE(state.loss_episode.presenting);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);
    EXPECT_FALSE(project(state).observation_lost);
}

TEST(LostObservationEpisodeTests, ALaterDistinctLossIsANewEpisodeWithItsOwnAcknowledgement) {
    AppState state = lost_after_printing();
    state = reduce(state, Event::display_idle_timer_fired(at_ms(620000)));
    state = with_printer_state(state, PrinterState::Printing, at_ms(630000), 2);
    ASSERT_FALSE(state.loss_episode.active);

    state = reduce(state, Event::confidence_aged(at_ms(1300000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_TRUE(state.loss_episode.active) << "a new episode";
    EXPECT_TRUE(state.loss_episode.presenting) << "with its own single acknowledgement";
}

// ⛔ FIRING CONTROL for generation reuse — defect 3. The episode is reset by recovery, so a counter
// living inside it republished generation 1 and a renderer that had coalesced the intervening
// recovery presentation never restarted the motion.
TEST(LostObservationEpisodeTests, TwoEpisodesNeverPublishTheSameGeneration) {
    AppState state = lost_after_printing();
    const std::uint32_t first = project(state).observation_lost_generation;
    ASSERT_GT(first, 0U);

    // Recovery, then a second genuine loss. The intermediate recovery presentation is deliberately
    // never "consumed" here — that is exactly the coalescing case the defect needed.
    state = with_printer_state(state, PrinterState::Printing, at_ms(601000), 2);
    state = reduce(state, Event::confidence_aged(at_ms(1300000)));
    ASSERT_TRUE(state.loss_episode.presenting);

    const std::uint32_t second = project(state).observation_lost_generation;
    EXPECT_NE(second, first) << "a second episode must restart Candidate-D motion";
    EXPECT_EQ(second, first + 1U) << "monotonic across episode closure";
}

// ⛔ FIRING CONTROL for generation reuse at the unsigned wrap. The reducer cannot be driven 2^32
// times, so the wrap is tested where it is decided.
//
// Zero is the projection's "nothing is acknowledged" value, so a live generation must never be zero
// — a consumer comparing a published zero against its own idle zero would see no change and draw
// nothing for a real loss.
TEST(LostObservationEpisodeTests, ThePresentationGenerationWrapSkipsTheReservedZero) {
    EXPECT_EQ(next_presentation_generation(0U), 1U);
    EXPECT_EQ(next_presentation_generation(1U), 2U);
    EXPECT_EQ(next_presentation_generation(0xFFFFFFFEU), 0xFFFFFFFFU);
    EXPECT_EQ(next_presentation_generation(0xFFFFFFFFU), 1U) << "the wrap steps over zero";

    // And every step is a change, which is the only property consumers rely on.
    for (const std::uint32_t g : {0U, 1U, 7U, 0xFFFFFFFEU, 0xFFFFFFFFU}) {
        EXPECT_NE(next_presentation_generation(g), g);
        EXPECT_NE(next_presentation_generation(g), 0U);
    }
}

// A projected acknowledgement therefore always carries a non-zero identity, and its absence always
// carries zero. That pairing is what lets a render task use one field for both questions.
TEST(LostObservationEpisodeTests, ZeroMeansNothingIsAcknowledgedAndAnythingElseIsAnIdentity) {
    const AppState presenting = lost_after_printing();
    ASSERT_TRUE(project(presenting).observation_lost_acknowledgement);
    EXPECT_NE(project(presenting).observation_lost_generation, 0U);

    const AppState retired = reduce(presenting, Event::display_idle_timer_fired(at_ms(620000)));
    ASSERT_FALSE(project(retired).observation_lost_acknowledgement);
    EXPECT_EQ(project(retired).observation_lost_generation, 0U);
}

// ⛔ FIRING CONTROL for priority leakage — defect 1. Priority arrives AFTER the acknowledgement has
// begun, then clears before the ordinary 20 s retirement. Nothing may come back.
TEST(LostObservationEpisodeTests, PriorityArrivingDuringTheAcknowledgementForfeitsIt) {
    for (const auto& c : priority_cases()) {
        AppState state = lost_after_printing();
        ASSERT_TRUE(state.loss_episode.presenting) << c.name;
        ASSERT_TRUE(project(state).observation_lost_acknowledgement) << c.name;

        // Priority takes ownership mid-acknowledgement.
        AppState raised = reduce(state, c.trigger);
        EXPECT_FALSE(raised.loss_episode.presenting) << c.name << ": retired immediately";
        EXPECT_TRUE(raised.loss_episode.automatic_spent) << c.name << ": and forfeited";
        EXPECT_FALSE(project(raised).observation_lost_acknowledgement) << c.name;

        // It clears well before the 20 s retirement would have run — and the clear is THIS case's
        // own, not a generic sequence that happens to leave it in force.
        AppState cleared = raised;
        for (const Event& event : c.clear) {
            cleared = reduce(cleared, event);
        }

        // ⛔ THE PRECONDITION FOR THE ASSERTION BELOW, AND IT IS THE WHOLE POINT. If the priority is
        // still in force, "nothing leaked" says nothing at all.
        ASSERT_TRUE(subject_presentation_owns_panel(cleared))
            << c.name
            << ": the clear did not return the panel to eligible ownership, so the leak "
               "assertion below would be vacuous";
        ASSERT_EQ(cleared.confidence, Confidence::Unavailable)
            << c.name << ": the loss must still be in force for a leak to be possible";

        // An unrelated wake on top of that, which is the case the original defect reappeared under.
        cleared = reduce(cleared, Event::button_gesture(at_ms(1800), ButtonGesture::ShortPress));

        // The press above lands on a LIT panel, so it is not the explicit-wake cause.
        EXPECT_FALSE(cleared.loss_episode.presenting)
            << c.name << ": no delayed acknowledgement may leak through";
    }
}

TEST(LostObservationEpisodeTests, LossUnderPriorityForfeitsTheAutomaticAcknowledgement) {
    for (const auto& c : priority_cases()) {
        AppState state = printing_device(at_ms(0));
        state = reduce(state, c.trigger);
        state = reduce(state, Event::confidence_aged(at_ms(600000)));

        ASSERT_EQ(state.confidence, Confidence::Unavailable) << c.name;
        EXPECT_TRUE(state.loss_episode.automatic_spent) << c.name << ": forfeited, not queued";
        EXPECT_FALSE(state.loss_episode.presenting) << c.name;
        EXPECT_FALSE(project(state).observation_lost_acknowledgement) << c.name;
    }
}

// The full accepted cause sequence for defect 2.
TEST(LostObservationEpisodeTests, AnExplicitPressStillWorksAfterAnAutomaticForfeiture) {
    // 1. loss under priority: the automatic acknowledgement is forfeited.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::provisioning_phase(at_ms(1000), ProvisioningPhase::SetupRequired));
    state = reduce(state, Event::confidence_aged(at_ms(600000)));
    ASSERT_TRUE(state.loss_episode.automatic_spent);
    ASSERT_FALSE(state.loss_episode.presenting);

    // 2. an explicit press WHILE priority still exists presents nothing.
    AppState blocked = reduce(state, Event::display_sleep_timer_fired(at_ms(601000)));
    blocked = reduce(blocked, Event::button_gesture(at_ms(602000), ButtonGesture::ShortPress));
    EXPECT_FALSE(blocked.loss_episode.presenting) << "current priority still blocks the press";

    // 3. priority clears: nothing appears automatically.
    state = reduce(state, Event::provisioning_phase(at_ms(603000), ProvisioningPhase::Inactive));
    ASSERT_TRUE(subject_presentation_owns_panel(state));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_FALSE(state.loss_episode.presenting) << "no automatic reveal after priority clears";

    // 4. an unrelated activity wake still does nothing.
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(604000)));
    state = reduce(state, Event::display_wake(at_ms(605000)));
    EXPECT_FALSE(state.loss_episode.presenting) << "an unrelated wake is not a cause";

    // 5. the panel returns to Off.
    state = reduce(state, Event::display_idle_timer_fired(at_ms(606000)));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(607000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    // 6. an explicit short press, no current priority: the acknowledgement presents.
    state = reduce(state, Event::button_gesture(at_ms(608000), ButtonGesture::ShortPress));
    EXPECT_TRUE(state.loss_episode.presenting)
        << "an explicit request is a different cause from the forfeited automatic entry";
    EXPECT_TRUE(project(state).observation_lost_acknowledgement);
}

// ⛔ ONE PRECEDENCE LIST, PROVEN OVER EVERY CATEGORY — defect 4. The reducer's ownership test and
// the projection's layer resolution are the same function, so they cannot drift.
// ---------------------------------------------------------------------------
// ACCESS CONDITION, IN ITS OWN RIGHT (UIAD 001 §8, §16.2)
// ---------------------------------------------------------------------------
//
// It outranks subject presentation, it is NOT transient — §16.2: "Authentication errors and
// unsupported API errors are not transient network failures" — and it clears only on a positive
// reclassification. That makes it the priority most likely to be live exactly when a printer stops
// answering, and it was absent from the matrix entirely.

TEST(LostObservationEpisodeTests, AnAccessConditionArrivingMidAcknowledgementForfeitsIt) {
    AppState state = lost_after_printing();
    ASSERT_TRUE(state.loss_episode.presenting);
    ASSERT_TRUE(project(state).observation_lost_acknowledgement);

    ConnectionObservation unauthorized;
    unauthorized.phase = ConnectionPhase::Unauthorized;
    state = reduce(state, Event::connection_observed(at_ms(601000), unauthorized));

    // Retired immediately, and forfeited — not merely hidden in the projection.
    ASSERT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);
    EXPECT_EQ(project(state).layer, PresentationLayer::AccessCondition);
    EXPECT_FALSE(state.loss_episode.presenting);
    EXPECT_TRUE(state.loss_episode.automatic_spent);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);
    EXPECT_FALSE(project(state).observation_lost_eligible);

    // A successful connection is the positive reclassification that clears it — and it clears the
    // ACCESS condition without making the printer observable again, which is the whole point.
    ConnectionObservation connected;
    connected.phase = ConnectionPhase::Connected;
    state = reduce(state, Event::connection_observed(at_ms(602000), connected));

    ASSERT_EQ(state.access_condition, AccessCondition::None);
    ASSERT_TRUE(subject_presentation_owns_panel(state)) << "the panel really is eligible again";
    ASSERT_EQ(state.confidence, Confidence::Unavailable) << "and still unobservable";
    EXPECT_TRUE(project(state).observation_lost_eligible);

    // ⛔ AND NOTHING REVEALS ITSELF. The forfeited acknowledgement does not queue up for the moment
    // the higher-priority surface clears.
    EXPECT_FALSE(state.loss_episode.presenting) << "no automatic reveal once access is restored";
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);

    // Nor after further failed polls, nor after an unrelated wake.
    state = reduce(state, Event::confidence_aged(at_ms(603000)));
    state = reduce(state, Event::display_wake(at_ms(604000)));
    EXPECT_FALSE(state.loss_episode.presenting);
}

TEST(LostObservationEpisodeTests, LossBeginningUnderAnAccessConditionStillPermitsTheExplicitPress) {
    // 1. the access condition is live BEFORE the loss.
    AppState state = printing_device(at_ms(0));
    ConnectionObservation unauthorized;
    unauthorized.phase = ConnectionPhase::Unauthorized;
    state = reduce(state, Event::connection_observed(at_ms(1000), unauthorized));
    ASSERT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);

    // 2. observation is lost under it: the automatic acknowledgement is forfeited outright.
    state = reduce(state, Event::confidence_aged(at_ms(600000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    ASSERT_TRUE(state.loss_episode.active);
    EXPECT_TRUE(state.loss_episode.automatic_spent) << "forfeited, never queued";
    EXPECT_FALSE(state.loss_episode.presenting);

    // 3. the condition clears, and eligibility genuinely returns.
    ConnectionObservation connected;
    connected.phase = ConnectionPhase::Connected;
    state = reduce(state, Event::connection_observed(at_ms(601000), connected));
    ASSERT_EQ(state.access_condition, AccessCondition::None);
    ASSERT_TRUE(subject_presentation_owns_panel(state));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_FALSE(state.loss_episode.presenting) << "still no automatic reveal";

    // 4. the panel recedes all the way to true Off.
    state = reduce(state, Event::display_idle_timer_fired(at_ms(602000)));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(903000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    // 5. ⛔ AND THE EXPLICIT PRESS IS STILL PERMITTED. The press is a different cause from the
    // automatic entry, and forfeiting the automatic one must not withdraw the person's own.
    state = reduce(state, Event::button_gesture(at_ms(904000), ButtonGesture::ShortPress));
    EXPECT_TRUE(state.loss_episode.presenting) << "the press keeps its own permission";
    EXPECT_TRUE(project(state).observation_lost_acknowledgement);
    EXPECT_NE(project(state).observation_lost_generation, 0U);
}

// A SUBJECT CONDITION — §7's Attention, which attaches to the subject rather than replacing it, and
// is the one eligibility test that is not about a higher LAYER.
//
// ⛔ IT CANNOT ARRIVE WHILE OBSERVATION IS LOST, AND THAT IS WORTH PINNING RATHER THAN ASSUMING.
// subject_condition is derived from printer data, and the only thing that changes printer data is a
// FRESH observation — which restores confidence and closes the episode outright. So the "condition
// arrives mid-acknowledgement, then clears" sequence is not reachable through the reducer at all: the
// arrival is itself a recovery. Asserted below, so that a future change making it reachable fails
// here instead of quietly creating a path with no forfeiture rule.
TEST(LostObservationEpisodeTests, ASubjectConditionCannotArriveWithoutEndingTheEpisode) {
    AppState state = lost_after_printing();
    const std::uint32_t first = project(state).observation_lost_generation;
    ASSERT_TRUE(state.loss_episode.presenting);
    ASSERT_EQ(state.subject_condition, SubjectCondition::None);

    // The only way to raise a condition is an observation, and it lands as a recovery.
    state = with_printer_state(state, PrinterState::Attention, at_ms(601000), 2);
    ASSERT_NE(state.subject_condition, SubjectCondition::None);
    EXPECT_EQ(state.confidence, Confidence::Current) << "the arrival IS a recovery";
    EXPECT_FALSE(state.loss_episode.active) << "so the episode is closed, not forfeited";
    EXPECT_FALSE(project(state).observation_lost);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);

    // The condition is nonetheless ineligible ownership while it lasts — the case a layer-only guard
    // misses, because the LAYER is still the subject presentation.
    EXPECT_EQ(project(state).layer, PresentationLayer::SubjectPresentation);
    EXPECT_FALSE(subject_presentation_owns_panel(state));
    EXPECT_FALSE(project(state).observation_lost_eligible);

    // Observation is lost again while the condition is still live: forfeited, never queued.
    state = reduce(state, Event::confidence_aged(at_ms(1300000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    ASSERT_NE(state.subject_condition, SubjectCondition::None);
    EXPECT_TRUE(state.loss_episode.active);
    EXPECT_TRUE(state.loss_episode.automatic_spent) << "forfeited under the live condition";
    EXPECT_FALSE(state.loss_episode.presenting);

    // The condition clears — which again can only be a fresh observation, so again a recovery — and
    // the next loss is a NEW episode with its own single acknowledgement and a DISTINCT generation.
    // That is correct, and a distinct generation is what proves it is not the old one resurfacing.
    state = with_printer_state(state, PrinterState::Printing, at_ms(1301000), 3);
    ASSERT_EQ(state.subject_condition, SubjectCondition::None);
    state = reduce(state, Event::confidence_aged(at_ms(2000000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);
    ASSERT_TRUE(subject_presentation_owns_panel(state));

    EXPECT_TRUE(state.loss_episode.presenting);
    const std::uint32_t third = project(state).observation_lost_generation;
    EXPECT_NE(third, first) << "a new episode, not the forfeited one leaking back";
    EXPECT_NE(third, 0U);
}

// ⛔ VALUE EQUALITY MUST SEE EVERY LOST-OBSERVATION FIELD. controller.cpp skips an identical
// presentation without drawing — §11's redraw rule — so a field omitted from equality is a change
// the renderer never sees, however load-bearing it is.
TEST(LostObservationEpisodeTests, EqualitySeesEveryLostObservationField) {
    const Presentation base = project(lost_after_printing());
    ASSERT_TRUE(base.observation_lost);
    ASSERT_TRUE(base.observation_lost_acknowledgement);
    ASSERT_TRUE(base.observation_lost_eligible);
    ASSERT_NE(base.observation_lost_generation, 0U);
    ASSERT_EQ(base, base) << "a presentation equals itself";

    // ⛔ THE FIELD THIS REGRESSION EXISTS FOR, AND THE FOCUSED FIRING CONTROL FOR IT. Eligibility is
    // varied HERE BY HAND, alone, because that is the only way to isolate the equality entry: remove
    // the comparison from operator== and this assertion is what fails.
    //
    // ⬅️ NO CURRENTLY REACHABLE REDUCER TRANSITION VARIES ELIGIBILITY ALONE. It is a pure function of
    // `layer` and `subject_condition`, and both are already compared, so every reachable displacement
    // moves one of those too — see EligibilityIsDerivableFromTwoAlreadyComparedFields below, which
    // asserts that derivation rather than assuming it.
    //
    // So the equality entry is DEFENSIVE, against a future eligibility predicate that consults a
    // third fact. On that day the derivation test fails and this line becomes what keeps a retired
    // acknowledgement's owed fade from being coalesced into an immediate hide, or the reverse.
    //
    // LosingEligibilityAfterRetirementIsAVisibleChange is the reachable integration check for the
    // same seam, and it is NOT a firing control for this entry: it currently also differs through
    // `layer`, so it passes with or without the comparison.
    Presentation ineligible = base;
    ineligible.observation_lost_eligible = false;
    EXPECT_NE(base, ineligible) << "eligibility alone must make two presentations unequal";
    EXPECT_FALSE(base == ineligible);

    // Its siblings, on the same terms and in the same test, because the omission was of a kind.
    Presentation not_lost = base;
    not_lost.observation_lost = false;
    EXPECT_NE(base, not_lost);

    Presentation retired = base;
    retired.observation_lost_acknowledgement = false;
    EXPECT_NE(base, retired);

    Presentation restarted = base;
    restarted.observation_lost_generation = base.observation_lost_generation + 1U;
    EXPECT_NE(base, restarted);

    Presentation elsewhere = base;
    elsewhere.observation_lost_origin = LossOriginForm::Stopped;
    ASSERT_NE(base.observation_lost_origin, LossOriginForm::Stopped);
    EXPECT_NE(base, elsewhere);
}

// ⬅️ WHY THE ENTRY ABOVE IS DEFENSIVE RATHER THAN A FIX FOR AN OBSERVABLE DEFECT, recorded here so
// nobody has to rediscover it: `observation_lost_eligible` is a pure function of `layer` and
// `subject_condition`, and BOTH of those are already compared. So no presentation the reducer can
// produce differs only in eligibility — the omission was a latent hazard, not a live coalescing bug.
//
// ⛔ AND THIS IS WHAT MAKES IT LATENT RATHER THAN HARMLESS. The day the eligibility rule consults a
// third fact, this test fails and the equality entry becomes the thing that keeps the renderer
// informed. Asserted across every layer the resolver can reach, not argued.
TEST(LostObservationEpisodeTests, EligibilityIsDerivableFromTwoAlreadyComparedFields) {
    const auto agrees = [](const AppState& state) {
        const Presentation view = project(state);
        const bool derivable = view.layer == PresentationLayer::SubjectPresentation &&
                               view.subject_condition == SubjectCondition::None;
        return view.observation_lost_eligible == derivable;
    };

    EXPECT_TRUE(agrees(lost_after_printing()));
    EXPECT_TRUE(agrees(printing_device(at_ms(0))));
    EXPECT_TRUE(agrees(booted_device(at_ms(0))));
    EXPECT_TRUE(agrees(contested_state()));

    for (const auto& c : priority_cases()) {
        EXPECT_TRUE(agrees(reduce(lost_after_printing(), c.trigger))) << c.name;
        EXPECT_TRUE(agrees(reduce(printing_device(at_ms(0)), c.trigger))) << c.name;
    }

    // And a live subject condition, which is the case that moves eligibility without moving layer.
    AppState attention = printing_device(at_ms(0));
    attention = with_printer_state(attention, PrinterState::Attention, at_ms(100), 2);
    ASSERT_NE(attention.subject_condition, SubjectCondition::None);
    ASSERT_EQ(project(attention).layer, PresentationLayer::SubjectPresentation);
    EXPECT_TRUE(agrees(attention));
}

// The reachable displacement pair, end to end: a retired acknowledgement that is then displaced must
// not project as the same presentation. It passes on `layer` alone today — see the note above — and
// it is here as the integration check, NOT as the firing control for the equality entry.
TEST(LostObservationEpisodeTests, LosingEligibilityAfterRetirementIsAVisibleChange) {
    AppState state = lost_after_printing();
    state = reduce(state, Event::display_idle_timer_fired(at_ms(620000)));

    const Presentation retired = project(state);
    ASSERT_FALSE(retired.observation_lost_acknowledgement);
    ASSERT_TRUE(retired.observation_lost);
    ASSERT_TRUE(retired.observation_lost_eligible) << "owed the outgoing fade";

    const AppState displaced = reduce(state, Event::recovery_mode_entered(at_ms(620500)));
    const Presentation after = project(displaced);
    ASSERT_FALSE(after.observation_lost_eligible) << "owed nothing now";

    EXPECT_NE(retired, after)
        << "the renderer must be handed this change rather than have it coalesced away";
}

// ⛔ THE OUTGOING-TAIL SEAM. A renderer must be able to tell WHY an acknowledgement stopped without
// re-deriving §12's precedence, so the projection publishes the eligibility predicate itself.
TEST(LostObservationEpisodeTests, TheEligibilitySeamDistinguishesRetirementFromDisplacement) {
    // Retired by the ordinary recession: still lost, still eligible. The last frame is owed the fade.
    AppState receded = lost_after_printing();
    receded = reduce(receded, Event::display_idle_timer_fired(at_ms(620000)));
    ASSERT_FALSE(project(receded).observation_lost_acknowledgement);
    EXPECT_TRUE(project(receded).observation_lost) << "still unobservable";
    EXPECT_TRUE(project(receded).observation_lost_eligible) << "and nothing has taken the panel";

    // Displaced by priority: still lost, NOT eligible. Owed nothing.
    for (const auto& c : priority_cases()) {
        AppState displaced = reduce(lost_after_printing(), c.trigger);
        ASSERT_FALSE(project(displaced).observation_lost_acknowledgement) << c.name;
        EXPECT_TRUE(project(displaced).observation_lost) << c.name;
        EXPECT_FALSE(project(displaced).observation_lost_eligible) << c.name;
    }

    // Recovered: not lost at all. Also owed nothing, and by the other half of the pair.
    AppState recovered = lost_after_printing();
    recovered = with_printer_state(recovered, PrinterState::Printing, at_ms(601000), 2);
    ASSERT_EQ(recovered.confidence, Confidence::Current);
    EXPECT_FALSE(project(recovered).observation_lost);
    EXPECT_FALSE(project(recovered).observation_lost_acknowledgement);

    // ⬅️ AND ELIGIBILITY IS TRUE HERE, which is exactly why `observation_lost` is the other half of
    // the renderer's test: a recovered panel owns the subject presentation perfectly well.
    EXPECT_TRUE(project(recovered).observation_lost_eligible);
}

TEST(LostObservationEpisodeTests, EligibilityAgreesWithTheLayerResolverForEveryCategory) {
    for (const auto& c : priority_cases()) {
        AppState state = printing_device(at_ms(0));
        state = reduce(state, c.trigger);

        EXPECT_FALSE(subject_presentation_owns_panel(state)) << c.name;
        EXPECT_NE(project(state).layer, PresentationLayer::SubjectPresentation) << c.name;
    }

    // Including the case the two hand-written lists disagreed on.
    AppState fatal = printing_device(at_ms(0));
    fatal = reduce(fatal, Event::boot_phase(at_ms(1000), BootPhase::FatalError));
    EXPECT_FALSE(subject_presentation_owns_panel(fatal)) << "FatalError is an unresolved boot";
    EXPECT_EQ(project(fatal).layer, PresentationLayer::SystemLifecycle);

    // An unresolved subject condition is part of the subject presentation, so it also blocks.
    AppState attention = printing_device(at_ms(0));
    attention = with_printer_state(attention, PrinterState::Attention, at_ms(1000), 2);
    ASSERT_NE(attention.subject_condition, SubjectCondition::None);
    EXPECT_FALSE(subject_presentation_owns_panel(attention));

    // And a plain printing device is eligible.
    EXPECT_TRUE(subject_presentation_owns_panel(printing_device(at_ms(0))));
}

TEST(LostObservationEpisodeTests, DegradedOpensNoEpisodeAndKeepsItsOwnTreatment) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at_ms(30000)));

    ASSERT_EQ(state.confidence, Confidence::Degraded);
    EXPECT_FALSE(state.loss_episode.active);
    EXPECT_FALSE(state.loss_episode.presenting);

    const Presentation view = project(state);
    EXPECT_TRUE(view.stale_indicator);
    EXPECT_FALSE(view.observation_lost);
    EXPECT_FALSE(view.observation_lost_acknowledgement);
    EXPECT_EQ(view.subject, SubjectState::Printing);
}

TEST(LostObservationProjectionTests, NoPreviousSubjectValueOrIdentityIsPresented) {
    const AppState state = lost_observation_state();
    const Presentation view = project(state);

    // A real job WAS on record before expiry, which is what makes this test meaningful.
    ASSERT_TRUE(view.observation_lost);

    EXPECT_FALSE(view.printer_name_known) << "§9: printer identity is hidden at all times";
    EXPECT_FALSE(view.job_name_known);
    EXPECT_FALSE(view.job_record_supplied);
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.elapsed.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.nozzle.target.known);
    EXPECT_FALSE(view.bed.current.known);
    EXPECT_FALSE(view.bed.target.known);
    EXPECT_FALSE(view.chamber.current.known);
    EXPECT_FALSE(view.show_telemetry);

    // And no stale indicator: there is nothing stale being shown, because nothing is shown.
    EXPECT_FALSE(view.stale_indicator);
}

// Degraded keeps its own treatment and does NOT acknowledge — the two conditions stay distinct all
// the way through the projection.
TEST(LostObservationProjectionTests, DegradedKeepsItsStaleTreatmentAndNeverAcknowledges) {
    AppState state = printing_device(at_ms(0));
    state.confidence = Confidence::Degraded;
    state.display.power = DisplayPower::On;

    const Presentation view = project(state);

    EXPECT_TRUE(view.stale_indicator) << "§11.5: stale values behind an explicit indicator";
    EXPECT_FALSE(view.observation_lost);
    EXPECT_FALSE(view.observation_lost_acknowledgement);
    EXPECT_EQ(view.subject, SubjectState::Printing) << "a recent known state is still presented";
}

// Recovery clears the condition and the acknowledgement in one step, and restores truthful data.
TEST(LostObservationProjectionTests, RecoveryClearsTheAcknowledgementAndPresentsCurrentData) {
    const Presentation lost = project(lost_after_printing());
    ASSERT_TRUE(lost.observation_lost_acknowledgement);

    AppState recovered = printing_device(at_ms(0));
    recovered.display.power = DisplayPower::On;
    const Presentation view = project(recovered);

    EXPECT_FALSE(view.observation_lost);
    EXPECT_FALSE(view.observation_lost_acknowledgement);
    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_TRUE(view.show_telemetry) << "truthful current data returns immediately";
}
