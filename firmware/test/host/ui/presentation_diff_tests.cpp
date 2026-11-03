// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "fake_source/fixtures.hpp"
#include "fake_source/observations.hpp"
#include "presentation_diff.hpp"
#include "support/state_builders.hpp"

// The incremental-update suite — Architecture Spec §17.4, Production UI §11 Redraw.
//
// Two failures are being protected against, and they are opposites.
//
// Reporting too little is a stale panel: a temperature that moved and was not redrawn
// leaves the wrong number on screen indefinitely, and nothing else would notice.
//
// Reporting too much is a full-screen redraw every poll: lv_label_set_text() invalidates
// a label's area whether or not the text differs, so a diff that always reported change
// would be worse than no diff at all — it would cost the redraw AND the comparison.

using namespace prd::ui;
using prd::app_model::Confidence;
using prd::app_model::Presentation;
using prd::app_model::project;
using prd::app_model::SubjectState;
using prd::fake_source::Fixture;

namespace {

Presentation printing() {
    return project(prd::fake_source::state_for(Fixture::Printing));
}

} // namespace

// --- No change --------------------------------------------------------------

TEST(PresentationDiffTests, IdenticalPresentationsReportNoChange) {
    // The case that matters most: a poll that reported the same values must draw nothing.
    const auto view = printing();
    const auto delta = diff(view, view);

    EXPECT_FALSE(delta.any());
    EXPECT_FALSE(delta.full);
    EXPECT_FALSE(delta.scene);
    EXPECT_FALSE(delta.progress);
    EXPECT_FALSE(delta.remaining);
    EXPECT_FALSE(delta.nozzle);
    EXPECT_FALSE(delta.bed);
    EXPECT_FALSE(delta.layers);
    EXPECT_FALSE(delta.components);
    EXPECT_FALSE(delta.update);
}

TEST(PresentationDiffTests, TheFirstApplicationIsAlwaysFull) {
    // Nothing is on screen yet, so every value is new to it. Without this, a value that
    // happened to equal the default-constructed Presentation's would never be drawn.
    const auto delta = diff(Presentation{}, printing(), true);

    EXPECT_TRUE(delta.full);
    EXPECT_TRUE(delta.progress);
    EXPECT_TRUE(delta.remaining);
    EXPECT_TRUE(delta.nozzle);
    EXPECT_TRUE(delta.bed);
    EXPECT_TRUE(delta.layers);
    EXPECT_TRUE(delta.components);
    EXPECT_TRUE(delta.update);
}

// --- Field isolation --------------------------------------------------------
TEST(PresentationDiffTests, ChangedStatusAgeAnchorInvalidatesOnlyConfidenceCopy) {
    auto before = printing();
    before.stale_indicator = true;
    before.stale_observed_at = prd::app_model::Timestamp{1000};
    auto after = before;
    after.stale_observed_at = prd::app_model::Timestamp{2000};
    const auto delta = diff(before, after);
    EXPECT_NE(before, after);
    EXPECT_TRUE(delta.stale);
    EXPECT_FALSE(delta.full || delta.scene || delta.content || delta.progress || delta.remaining ||
                 delta.nozzle || delta.bed || delta.layers || delta.components);
}

//
// Each of these changes exactly one thing and asserts that exactly one flag moves. A
// diff that over-reported would pass a "did it notice?" test and fail these.

TEST(PresentationDiffTests, AChangedProgressReportsOnlyProgress) {
    const auto before = printing();
    auto after = before;
    after.progress.percent = static_cast<std::uint8_t>(before.progress.percent + 1);

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.progress);
    EXPECT_FALSE(delta.remaining);
    EXPECT_FALSE(delta.nozzle);
    EXPECT_FALSE(delta.bed);
    EXPECT_FALSE(delta.layers);
    EXPECT_FALSE(delta.components);
    EXPECT_FALSE(delta.full);
}

TEST(PresentationDiffTests, UpdateProgressAndPhaseUseTheUpdateFlag) {
    const auto before = printing();
    auto progress = before;
    progress.update_progress = {true, 42};

    auto delta = diff(before, progress);
    EXPECT_TRUE(delta.update);
    EXPECT_FALSE(delta.scene || delta.content || delta.progress || delta.remaining ||
                 delta.nozzle || delta.bed || delta.layers || delta.components);

    auto phase = progress;
    phase.ota_phase = prd::app_model::OtaPhase::Available;
    delta = diff(progress, phase);
    EXPECT_TRUE(delta.update);
    EXPECT_FALSE(delta.scene || delta.content || delta.progress || delta.remaining ||
                 delta.nozzle || delta.bed || delta.layers || delta.components);

    auto version = phase;
    version.update_version.assign("6.4.0");
    delta = diff(phase, version);
    EXPECT_TRUE(delta.update);
    EXPECT_FALSE(delta.scene || delta.content || delta.progress || delta.remaining ||
                 delta.nozzle || delta.bed || delta.layers || delta.components);
}

TEST(PresentationDiffTests, AChangedNozzleTemperatureReportsOnlyTheNozzle) {
    const auto before = printing();
    auto after = before;
    after.nozzle.current.celsius += 1.0F;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.nozzle);
    EXPECT_FALSE(delta.bed);
    EXPECT_FALSE(delta.progress);
    EXPECT_FALSE(delta.layers);
}

TEST(PresentationDiffTests, AChangedBedTemperatureReportsOnlyTheBed) {
    const auto before = printing();
    auto after = before;
    after.bed.current.celsius += 1.0F;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.bed);
    EXPECT_FALSE(delta.nozzle);
    EXPECT_FALSE(delta.progress);
}

TEST(PresentationDiffTests, AChangedLayerReportsOnlyLayers) {
    const auto before = printing();
    auto after = before;
    after.layers.current += 1;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.layers);
    EXPECT_FALSE(delta.progress);
    EXPECT_FALSE(delta.remaining);
}

TEST(PresentationDiffTests, AChangedRemainingTimeReportsOnlyRemaining) {
    const auto before = printing();
    auto after = before;
    after.remaining.seconds += 60;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.remaining);
    EXPECT_FALSE(delta.progress);
    EXPECT_FALSE(delta.layers);
}

TEST(PresentationDiffTests, TheApproximateMarkerBelongsToTheRemainingFlag) {
    // A job whose estimate quality improved without its seconds changing still has to
    // redraw the text, because the `~` is part of how the duration reads.
    const auto before = printing();
    auto after = before;
    after.remaining_is_approximate = !before.remaining_is_approximate;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.remaining);
    EXPECT_FALSE(delta.progress);
}

TEST(PresentationDiffTests, AChangedComponentVisibilityReportsComponents) {
    const auto before = printing();
    auto after = before;
    after.show_telemetry = !before.show_telemetry;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.components);
    EXPECT_FALSE(delta.progress);
}

// --- What must NOT report ---------------------------------------------------

TEST(PresentationDiffTests, ATargetTemperatureChangeReportsNothing) {
    // The projection carries targets and no Phase C composition draws them — §9's
    // Printing list has no target and the artwork shows none. Reporting a change would
    // invalidate a label whose content did not move.
    //
    // This is a deliberate coupling to what the scene renders, and it is why the delta's
    // field set grows with each scene rather than mirroring Presentation.
    const auto before = printing();
    auto after = before;
    after.nozzle.target.celsius += 10.0F;
    after.bed.target.celsius += 10.0F;

    EXPECT_FALSE(diff(before, after).any());
}

TEST(PresentationDiffTests, ARevisionBumpAloneReportsNothing) {
    // Presentation carries no revision, which is the point: the store's revision rises
    // on every accepted mutation, including ones that change nothing presentable. If the
    // revision reached the renderer, every dispatch would redraw.
    const auto state = prd::fake_source::state_for(Fixture::Printing);

    auto later = state;
    later.revision += 1;
    later.last_event_at = state.last_event_at + prd::test::at_ms(1000);

    EXPECT_FALSE(diff(project(state), project(later)).any());
}

// --- Scene changes ----------------------------------------------------------

TEST(PresentationDiffTests, AStateChangeWithinTheFamilyIsAContentChangeNotASceneChange) {
    // THE HAZARD THIS GUARDS. The whole operational family is one composition, so moving from
    // Printing to Ready does not switch scenes — `delta.scene` stays false. A diff that only
    // knew about scene changes would therefore leave "Printing" on the panel after the print
    // finished, with the old ring form and the old third line, and nothing would report it.
    //
    // `content` is what fires instead, and every state-dependent element is rebuilt on it.
    const auto from = printing();
    const auto to = project(prd::fake_source::state_for(Fixture::Ready));

    const auto delta = diff(from, to);

    EXPECT_FALSE(delta.scene) << "the family is one composition; the scene does not change";
    EXPECT_TRUE(delta.content) << "the state changed, so what the composition shows must too";
    EXPECT_TRUE(delta.any());
}

TEST(PresentationDiffTests, EveryTransitionWithinTheFamilyReportsAContentChange) {
    // Walked over the whole family rather than one pair, so a transition that happens to
    // leave the compared fields equal — Paused to Stopped, say, which can share progress and
    // telemetry — is still caught.
    using prd::app_model::Confidence;
    using prd::app_model::SubjectState;
    using prd::fake_source::Fixture;
    const Fixture family[] = {Fixture::Ready,  Fixture::Preparing, Fixture::Printing,
                              Fixture::Paused, Fixture::Completed, Fixture::Stopped};

    for (const auto from_fixture : family) {
        for (const auto to_fixture : family) {
            if (from_fixture == to_fixture) {
                continue;
            }

            const auto from = project(prd::fake_source::state_for(from_fixture));
            const auto to = project(prd::fake_source::state_for(to_fixture));

            EXPECT_TRUE(diff(from, to).content) << prd::fake_source::name(from_fixture) << " -> "
                                                << prd::fake_source::name(to_fixture);
        }
    }
}

TEST(PresentationDiffTests, TheStaleIndicatorIsItsOwnFlag) {
    // §11.5's indicator appears and disappears without any value changing — confidence can
    // fall while the last known figures stay exactly as they were. If it shared a flag with
    // something else, it would be shown or hidden only when that other thing happened to move.
    const auto before = printing();
    auto after = before;
    after.stale_indicator = !before.stale_indicator;

    const auto delta = diff(before, after);

    EXPECT_TRUE(delta.stale);
    EXPECT_FALSE(delta.progress);
    EXPECT_FALSE(delta.nozzle);
}

TEST(PresentationDiffTests, ThePrinterNameIsComparedBecauseReadyDrawsIt) {
    // In every other state the name is carried and not rendered; in Ready it is the title. So
    // it has to be compared like any other drawn value.
    const auto before = project(prd::fake_source::state_for(Fixture::Ready));
    auto after = before;
    after.printer_name.assign("Workshop Two");

    EXPECT_TRUE(diff(before, after).printer_name);
}

TEST(PresentationDiffTests, AValueChangeWithinOneStateIsNotFull) {
    const auto before = printing();
    auto after = before;
    after.progress.percent = static_cast<std::uint8_t>(before.progress.percent + 1);

    const auto delta = diff(before, after);

    EXPECT_FALSE(delta.scene);
    EXPECT_FALSE(delta.full);
    EXPECT_TRUE(delta.any());
}

// --- Against the real event stream -----------------------------------------

TEST(PresentationDiffTests, ARepeatedIdenticalObservationDrawsNothing) {
    // The end-to-end version of the first test, through the reducer: a printer that
    // reports the same status twice must not cause a redraw. This is the §11 requirement
    // in the form it actually occurs — a 2-second poll on a stable print.
    using namespace prd::app_model;
    using prd::fake_source::JobProgress;

    auto state = prd::fake_source::state_for(Fixture::Printing);
    const auto first = project(state);

    // The same job figures again, later in time and with a higher sequence, which is
    // exactly what a real adapter posts on the next poll of an unchanged print.
    const JobProgress unchanged{first.progress.percent,
                                static_cast<std::uint32_t>(first.layers.current)};

    state = reduce(
        state, Event::job_observed(state.last_event_at + prd::test::at_ms(2000),
                                   prd::fake_source::job(state.last_job_sequence + 1, unchanged)));

    const auto second = project(state);

    EXPECT_FALSE(diff(first, second).any()) << "an identical observation must not produce a redraw";
}

TEST(PresentationDiffTests, ARecordDisappearingAtReducedConfidenceIsStillAChange) {
    // THE ONE INPUT THAT CAN MOVE WHILE EVERY DIFFED VALUE STANDS STILL, added in Phase G.5.
    //
    // §10 has already removed the record's values at Degraded, so progress, layers and remaining
    // are absent on both sides. What changes is whether the record exists at all — which decides
    // the stack's arrangement. Without `job_record_supplied` in the comparison, nothing would
    // fire and the composition would keep a row for a record that had gone.
    Presentation before;
    before.subject = SubjectState::Printing;
    before.confidence = Confidence::Degraded;
    before.stale_indicator = true;
    before.show_operational_form = true;
    before.show_telemetry = true;
    before.job_record_supplied = true;

    Presentation after = before;
    after.job_record_supplied = false;

    const auto delta = diff(before, after);

    ASSERT_EQ(before.progress, after.progress) << "the values were already absent on both sides";
    ASSERT_EQ(before.layers, after.layers);
    ASSERT_EQ(before.remaining, after.remaining);

    EXPECT_TRUE(delta.content) << "the arrangement's input changed and nothing reported it";
    EXPECT_TRUE(delta.any());
}

// --- The record leaving and returning --------------------------------------
//
// REGRESSION, 2026-08-14. Not a hypothetical: this transition drew 62 % over a ring with no
// sweep on the physical panel, because `delta.content` did not compare `progress.known`.
//
// The transition is Printing -> reduced confidence or a withheld record -> Printing. Across it
// the subject, the condition, the printer name and `job_record_supplied` ALL STAND STILL — the
// machine supplied a record throughout — so no other clause in `delta.content` can fire and the
// form's record flag never reaches ProgressRing::set_form(). The composition is left holding the
// previous treatment.

TEST(PresentationDiffTests, LosingTheRecordReResolvesTheContent) {
    const Presentation with_record = printing();
    const Presentation without_record =
        project(prd::fake_source::state_for(Fixture::PrintingUnknownProgress));

    ASSERT_TRUE(with_record.progress.known);
    ASSERT_FALSE(without_record.progress.known);

    EXPECT_TRUE(diff(with_record, without_record).content);
}

TEST(PresentationDiffTests, RegainingTheRecordReResolvesTheContent) {
    // The half that was broken. Going the other way had `job_record_supplied` moving in the
    // no-record case, which masked the gap in one direction only.
    const Presentation without_record =
        project(prd::fake_source::state_for(Fixture::PrintingUnknownProgress));
    const Presentation with_record = printing();

    EXPECT_TRUE(diff(without_record, with_record).content)
        << "the form's record flag would never be re-applied, so the sweep would not return";
}

TEST(PresentationDiffTests, TheSweepReturnsAcrossAFullLossAndRecovery) {
    // The whole sequence the walk puts on the panel, asserted as one story: the frames either
    // side must be identical presentations, and BOTH transitions must re-resolve the content.
    const Presentation before = printing();
    const Presentation lost =
        project(prd::fake_source::state_for(Fixture::PrintingUnknownProgress));
    const Presentation after = printing();

    EXPECT_EQ(before, after) << "frames 1 and 3 must be the same presentation";

    EXPECT_TRUE(diff(before, lost).content);
    EXPECT_TRUE(diff(lost, after).content);

    // Degraded now retains the last percentage, but still re-resolves age and time content.
    const Presentation degraded = project(prd::fake_source::state_for(Fixture::PrintingDegraded));
    ASSERT_TRUE(degraded.progress.known);
    ASSERT_TRUE(degraded.stale_indicator);
    ASSERT_TRUE(degraded.job_record_supplied);

    EXPECT_TRUE(diff(before, degraded).stale);
    EXPECT_TRUE(diff(degraded, after).stale);
    EXPECT_FALSE(diff(before, degraded).content)
        << "retained progress no longer switches to the empty-record treatment";
}

TEST(PresentationDiffTests, AChangingPercentageDoesNotReResolveTheContent) {
    // The other half of the bargain, and the reason `.known` is compared rather than the value.
    // A percentage tick must move `progress` and nothing structural, or every poll would redraw
    // the whole composition.
    Presentation earlier = printing();
    Presentation later = printing();

    ASSERT_TRUE(earlier.progress.known);
    later.progress.percent = static_cast<std::uint8_t>(earlier.progress.percent + 1);

    const auto delta = diff(earlier, later);

    EXPECT_TRUE(delta.progress);
    EXPECT_FALSE(delta.content) << "a one-percent tick must not re-resolve the content table";
}
