// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"
#include "fake_source/scenarios.hpp"
#include "fake_source/source.hpp"

// The scenario suite.
//
// Fixtures prove the states exist. These prove the product gets between them: that a
// paused print keeps its job, that confidence decays in the right order, that a
// corrected API key actually reopens acquisition. Transitions are where the defects
// are, and a state fixture cannot show one.
//
// Also, and just as important: that the fake source cannot cheat. It holds a
// reference to the real store and the only thing it is allowed to do with it is
// dispatch — so every assertion below is also an assertion that the state was reached
// the way the product would reach it.

using namespace prd::app_model;
using prd::app_store::Store;
using prd::fake_source::FakeSource;
using prd::fake_source::kScenarioCount;
using prd::fake_source::Scenario;
using prd::fake_source::scenario_at;
using prd::fake_source::step_at;
using prd::fake_source::step_count;

namespace {

constexpr Timestamp at_ms(const std::int64_t milliseconds) {
    return std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{milliseconds});
}

// A store with a source bound to it. Declaration order matters: the store must exist
// before the source that references it.
struct ScenarioRun {
    Store store;
    FakeSource source;

    ScenarioRun(const Scenario scenario, const Timestamp base)
        : store{}, source{store, scenario, base} {}

    // Runs to completion, ignoring the schedule.
    void play() {
        while (!source.finished()) {
            source.advance();
        }
    }

    [[nodiscard]] Presentation view() const {
        return project(store.state());
    }
};

// Every event a scenario would dispatch, in order, without a store.
std::vector<Event> events_of(const Scenario scenario, const Timestamp base,
                             const std::uint32_t sequence_base = 0) {
    std::vector<Event> events;

    for (std::size_t index = 0; index < step_count(scenario); ++index) {
        events.push_back(step_at(scenario, index, base, sequence_base).event);
    }

    return events;
}

} // namespace

// --- Catalogue validity ----------------------------------------------------

TEST(ScenarioCatalogueTests, EveryScenarioIsAddressableAndNamed) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        EXPECT_FALSE(prd::fake_source::name(scenario).empty()) << "index " << index;
        EXPECT_GT(step_count(scenario), 0U) << prd::fake_source::name(scenario);
    }
}

TEST(ScenarioCatalogueTests, EveryStepIndexYieldsALabelledStep) {
    // Catches the failure mode the switch-plus-constant shape invites: a step_count
    // larger than the cases actually written, which would silently dispatch a
    // default-constructed event.
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        for (std::size_t step = 0; step < step_count(scenario); ++step) {
            const auto value = step_at(scenario, step, Timestamp::zero(), 0);

            EXPECT_FALSE(value.label.empty())
                << prd::fake_source::name(scenario) << " step " << step;
            EXPECT_NE(value.event.type, EventType::None)
                << prd::fake_source::name(scenario) << " step " << step;
        }
    }
}

TEST(ScenarioCatalogueTests, AnOutOfRangeStepIsEmptyRatherThanInvented) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);
        const auto beyond = step_at(scenario, step_count(scenario), Timestamp::zero(), 0);

        EXPECT_TRUE(beyond.label.empty()) << prd::fake_source::name(scenario);
        EXPECT_EQ(beyond.event.type, EventType::None) << prd::fake_source::name(scenario);
    }
}

TEST(ScenarioCatalogueTests, StepTimestampsNeverGoBackwards) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        Timestamp previous = Timestamp::zero();
        for (const auto& event : events_of(scenario, at_ms(1000))) {
            EXPECT_GE(event.observed_at, previous) << prd::fake_source::name(scenario);
            previous = event.observed_at;
        }
    }
}

TEST(ScenarioCatalogueTests, NoScenarioRejectsItsOwnObservations) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        ScenarioRun run{scenario, at_ms(0)};
        run.play();

        EXPECT_EQ(run.store.state().diagnostics.events_rejected_stale, 0U)
            << prd::fake_source::name(scenario);
        EXPECT_EQ(run.store.state().diagnostics.events_out_of_order, 0U)
            << prd::fake_source::name(scenario);
    }
}

// --- Determinism and the store contract ------------------------------------

TEST(ScenarioReplayTests, TwoIndependentRunsProduceIdenticalStates) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        ScenarioRun first{scenario, at_ms(0)};
        ScenarioRun second{scenario, at_ms(0)};
        first.play();
        second.play();

        EXPECT_EQ(first.store.state(), second.store.state()) << prd::fake_source::name(scenario);
    }
}

TEST(ScenarioReplayTests, TheSourceOnlyEverDispatches) {
    // The property that lets the PrusaLink adapter replace this without the store
    // noticing. Reducing the same events by hand must land exactly where dispatching
    // them through the source did — so the source cannot have written state any other
    // way, and the store's dispatch count must equal the steps it applied.
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        ScenarioRun run{scenario, at_ms(0)};
        run.play();

        AppState folded;
        for (const auto& event : events_of(scenario, at_ms(0))) {
            folded = reduce(folded, event);
        }

        EXPECT_EQ(run.store.state(), folded) << prd::fake_source::name(scenario);
        EXPECT_EQ(run.store.statistics().dispatched, step_count(scenario))
            << prd::fake_source::name(scenario);
        EXPECT_EQ(run.source.steps_applied(), step_count(scenario))
            << prd::fake_source::name(scenario);
    }
}

TEST(ScenarioReplayTests, RevisionsOnlyEverRise) {
    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);

        ScenarioRun run{scenario, at_ms(0)};
        std::uint32_t previous = 0;
        std::uint32_t mutations = 0;

        while (!run.source.finished()) {
            const auto advance = run.source.advance();

            EXPECT_TRUE(advance.applied) << prd::fake_source::name(scenario);
            EXPECT_GE(advance.revision, previous) << prd::fake_source::name(scenario);

            if (advance.mutated) {
                EXPECT_GT(advance.revision, previous) << prd::fake_source::name(scenario);
                ++mutations;
            }

            previous = advance.revision;
        }

        EXPECT_GT(mutations, 0U) << prd::fake_source::name(scenario);
        EXPECT_EQ(run.store.statistics().mutations, mutations) << prd::fake_source::name(scenario);
    }
}

TEST(ScenarioReplayTests, StepLabelsAndIndicesAreReportedInOrder) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};

    std::size_t expected_index = 0;
    while (!run.source.finished()) {
        const auto advance = run.source.advance();

        EXPECT_EQ(advance.step_index, expected_index);
        EXPECT_FALSE(advance.label.empty());
        ++expected_index;
    }

    EXPECT_EQ(expected_index, step_count(Scenario::FullPrintCycle));
}

// --- Scheduling ------------------------------------------------------------

TEST(ScenarioSchedulingTests, AdvanceToAppliesOnlyWhatIsDue) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};

    // The prelude is inside the first 100 ms; the first post-prelude step is at 1000.
    const std::size_t prelude = run.source.advance_to(at_ms(500));
    EXPECT_EQ(prelude, 8U);
    EXPECT_EQ(run.view().subject, SubjectState::Ready);

    // Nothing else is due yet.
    EXPECT_EQ(run.source.advance_to(at_ms(999)), 0U);

    EXPECT_EQ(run.source.advance_to(at_ms(1000)), 1U);
    EXPECT_EQ(run.view().subject, SubjectState::Preparing);
}

TEST(ScenarioSchedulingTests, AdvanceToTerminatesWhenTheScenarioIsDone) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    const std::size_t applied = run.source.advance_to(at_ms(1000000));

    EXPECT_EQ(applied, step_count(Scenario::ColdBootToReady));
    EXPECT_TRUE(run.source.finished());
    EXPECT_EQ(run.source.advance_to(at_ms(2000000)), 0U);
}

TEST(ScenarioSchedulingTests, AdvanceIgnoresTheScheduleSoASingleStepAlwaysWorks) {
    // The bench control: a short press advances one step whether or not its scheduled
    // moment has arrived.
    ScenarioRun run{Scenario::ConfidenceDecay, at_ms(0)};

    for (std::size_t step = 0; step < step_count(Scenario::ConfidenceDecay); ++step) {
        EXPECT_TRUE(run.source.advance().applied) << "step " << step;
    }

    EXPECT_TRUE(run.source.finished());
}

// --- Pause, resume, restart ------------------------------------------------

TEST(ScenarioControlTests, PauseStopsBothAdvanceForms) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};
    run.source.advance_to(at_ms(500));
    const std::uint32_t revision = run.store.revision();
    const std::size_t index = run.source.step_index();

    run.source.pause();
    EXPECT_TRUE(run.source.paused());

    EXPECT_FALSE(run.source.advance().applied);
    EXPECT_EQ(run.source.advance_to(at_ms(100000)), 0U);

    EXPECT_EQ(run.store.revision(), revision);
    EXPECT_EQ(run.source.step_index(), index);
}

TEST(ScenarioControlTests, ResumeContinuesFromWhereItPaused) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};
    run.source.advance_to(at_ms(500));

    run.source.pause();
    const std::size_t index = run.source.step_index();

    run.source.resume();
    EXPECT_FALSE(run.source.paused());

    EXPECT_TRUE(run.source.advance().applied);
    EXPECT_EQ(run.source.step_index(), index + 1);
}

// --- The unattended sweep policy -------------------------------------------
//
// This is the policy that used to live in the validation flow, where no host test
// could reach it — and where the pause-at-a-scenario-boundary defect lived until a
// finger on the button found it. It is production code now, so these tests exercise
// the real implementation rather than a copy of it.

namespace {

constexpr std::chrono::milliseconds kDwell{6000};

} // namespace

TEST(ScenarioSweepTests, AdvancesWhatIsDueAndReportsCompletionOnce) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    const auto early = run.source.sweep(at_ms(500), kDwell);
    EXPECT_EQ(early.steps_applied, step_count(Scenario::ColdBootToReady));
    EXPECT_TRUE(early.just_finished);
    EXPECT_FALSE(early.loaded_next);

    // Reported once. A driver logs one completion line, not one per tick for as long
    // as the dwell lasts.
    const auto again = run.source.sweep(at_ms(1000), kDwell);
    EXPECT_EQ(again.steps_applied, 0U);
    EXPECT_FALSE(again.just_finished);
    EXPECT_FALSE(again.loaded_next);
}

TEST(ScenarioSweepTests, HoldsTheTerminalStateForTheDwellThenLoadsTheNext) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    run.source.sweep(at_ms(500), kDwell);

    // Inside the dwell: nothing moves.
    EXPECT_FALSE(run.source.sweep(at_ms(5000), kDwell).loaded_next);
    EXPECT_EQ(run.source.scenario(), Scenario::ColdBootToReady);

    // Past it: the next scenario in the catalogue.
    const auto moved = run.source.sweep(at_ms(7000), kDwell);
    EXPECT_TRUE(moved.loaded_next);
    EXPECT_EQ(run.source.scenario(), Scenario::FirstSetupToReady);
    EXPECT_EQ(run.source.step_index(), 0U);
}

TEST(ScenarioSweepTests, PauseAtAScenarioBoundaryBlocksTheLoad) {
    // THE REGRESSION. Pausing a scenario that has already finished must stop the next
    // one loading. It did not, and because load() clears the pause flag the following
    // long press paused a freshly loaded scenario instead of resuming — from the bench
    // the toggle silently did not toggle.
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    run.source.sweep(at_ms(500), kDwell);
    ASSERT_TRUE(run.source.finished());

    run.source.pause();

    // Well past the dwell, and still held.
    const auto held = run.source.sweep(at_ms(100000), kDwell);
    EXPECT_FALSE(held.loaded_next);
    EXPECT_EQ(run.source.scenario(), Scenario::ColdBootToReady);

    // And still paused: the flag was not cleared underneath the user.
    EXPECT_TRUE(run.source.paused());

    // Resuming releases the boundary.
    run.source.resume();
    EXPECT_TRUE(run.source.sweep(at_ms(100100), kDwell).loaded_next);
    EXPECT_EQ(run.source.scenario(), Scenario::FirstSetupToReady);
}

TEST(ScenarioSweepTests, PauseMidScenarioBlocksAdvancing) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};

    run.source.sweep(at_ms(500), kDwell);
    const std::size_t index = run.source.step_index();
    ASSERT_FALSE(run.source.finished());

    run.source.pause();

    EXPECT_EQ(run.source.sweep(at_ms(100000), kDwell).steps_applied, 0U);
    EXPECT_EQ(run.source.step_index(), index);
}

TEST(ScenarioSweepTests, WalksTheWholeCatalogueAndWrapsWithRisingGenerations) {
    // The unattended behaviour observed on hardware: every scenario in order, then
    // back to the first, with observation generations that only ever rise — which is
    // what keeps §8.1 from rejecting the second pass as stale.
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    std::vector<Scenario> visited{run.source.scenario()};
    std::uint32_t previous_generation = run.source.generation();

    std::int64_t clock = 0;
    // Two full passes plus one, so the wrap is covered rather than assumed.
    while (visited.size() <= kScenarioCount + 1) {
        clock += 1000;
        const auto swept = run.source.sweep(at_ms(clock), kDwell);

        if (swept.loaded_next) {
            EXPECT_GT(run.source.generation(), previous_generation);
            previous_generation = run.source.generation();
            visited.push_back(run.source.scenario());
        }
    }

    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        EXPECT_EQ(visited[index], scenario_at(index)) << "position " << index;
    }

    // Wrapped back to the beginning.
    EXPECT_EQ(visited[kScenarioCount], scenario_at(0));

    // And the whole sweep stayed clean: no observation was ever rejected as stale.
    EXPECT_EQ(run.store.state().diagnostics.events_rejected_stale, 0U);
}

TEST(ScenarioSweepTests, LoadClearsTheDwellSoTheNewScenarioIsNotInstantlyReplaced) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};

    run.source.sweep(at_ms(500), kDwell);
    ASSERT_TRUE(run.source.sweep(at_ms(7000), kDwell).loaded_next);

    // The freshly loaded scenario must get its own dwell, not inherit the previous
    // one's expiry.
    const auto immediately_after = run.source.sweep(at_ms(7100), kDwell);
    EXPECT_FALSE(immediately_after.loaded_next);
    EXPECT_EQ(run.source.scenario(), Scenario::FirstSetupToReady);
}

TEST(ScenarioSweepTests, SequenceBaseMatchesWhatStepsWereDispatchedWith) {
    // A driver re-reads a dispatched step to label it in a log, and must use the same
    // sequence offset step_at() was called with or it will read a different event.
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};
    run.source.load(Scenario::FullPrintCycle, at_ms(1000));

    const std::uint32_t base = run.source.sequence_base();
    const auto expected = step_at(Scenario::FullPrintCycle, 0, at_ms(1000), base);

    const auto advance = run.source.advance();

    EXPECT_TRUE(advance.applied);
    EXPECT_EQ(advance.label.view(), expected.label.view());
    EXPECT_EQ(run.store.state().last_event_at, expected.event.observed_at);
}

TEST(ScenarioControlTests, PauseSurvivesReachingTheEndButNotALoad) {
    // Two properties, and the second one is a trap a consumer has to know about.
    //
    // A finished scenario stays paused: nothing advances, so pause is still in force.
    // But load() deliberately clears it, because loading is an explicit new start —
    // and that combination is what broke the bench pause/resume toggle in Phase B.
    // The scenario flow auto-loaded the next scenario once the current one finished,
    // without checking pause, so the flag was cleared underneath the user and their
    // next long press paused again instead of resuming.
    //
    // Pinned here so the next consumer of this class inherits the warning rather than
    // rediscovering it on hardware.
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};
    run.play();
    ASSERT_TRUE(run.source.finished());

    run.source.pause();
    EXPECT_TRUE(run.source.paused());

    // Still paused, and still finished: reaching the end changes neither.
    EXPECT_EQ(run.source.advance_to(at_ms(100000)), 0U);
    EXPECT_TRUE(run.source.paused());

    // A load is an explicit restart and clears the pause. A caller that auto-loads
    // must therefore check paused() itself first.
    run.source.load(Scenario::PrintingToStopped, at_ms(600000));
    EXPECT_FALSE(run.source.paused());
}

TEST(ScenarioControlTests, RestartClearsPauseSoAReplayCanActuallyRun) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};
    run.source.advance_to(at_ms(500));

    run.source.pause();
    ASSERT_TRUE(run.source.paused());

    run.source.reset(at_ms(600000));

    EXPECT_FALSE(run.source.paused());
    EXPECT_EQ(run.source.step_index(), 0U);
    EXPECT_TRUE(run.source.advance().applied);
}

TEST(ScenarioControlTests, RestartReplaysTheWholeScenario) {
    ScenarioRun run{Scenario::PrintingToStopped, at_ms(0)};
    run.play();

    const auto first_pass = run.view();
    const std::uint32_t generation = run.source.generation();

    // A restart is a re-run at a later time with fresh observation generations, not a
    // rewind: the store's history only moves forward, and §8.1 would otherwise reject
    // the entire second pass as stale.
    run.source.reset(at_ms(600000));
    EXPECT_GT(run.source.generation(), generation);
    EXPECT_EQ(run.source.step_index(), 0U);
    EXPECT_FALSE(run.source.finished());

    run.play();

    EXPECT_EQ(run.store.state().diagnostics.events_rejected_stale, 0U);
    EXPECT_EQ(run.view(), first_pass);
}

TEST(ScenarioControlTests, RepeatedRestartsStayDeterministic) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};
    run.play();
    const auto reference = run.view();

    std::int64_t base = 600000;
    for (int pass = 0; pass < 4; ++pass) {
        run.source.reset(at_ms(base));
        run.play();

        EXPECT_EQ(run.view(), reference) << "pass " << pass;
        EXPECT_EQ(run.store.state().diagnostics.events_rejected_stale, 0U) << "pass " << pass;

        base += 600000;
    }
}

TEST(ScenarioControlTests, LoadSwitchesScenarioAndRestartsIt) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};
    run.play();

    run.source.load(Scenario::RecoveryAndDeviceFault, at_ms(600000));

    EXPECT_EQ(run.source.scenario(), Scenario::RecoveryAndDeviceFault);
    EXPECT_EQ(run.source.step_index(), 0U);
    EXPECT_FALSE(run.source.paused());

    run.play();

    EXPECT_EQ(run.view().layer, PresentationLayer::DeviceFault);
    EXPECT_EQ(run.store.state().diagnostics.events_rejected_stale, 0U);
}

// --- What each scenario proves --------------------------------------------

TEST(ScenarioOutcomeTests, ColdBootReachesReady) {
    ScenarioRun run{Scenario::ColdBootToReady, at_ms(0)};
    run.play();

    const auto view = run.view();
    EXPECT_EQ(view.layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(view.subject, SubjectState::Ready);
    EXPECT_EQ(view.confidence, Confidence::Current);
    EXPECT_TRUE(view.show_operational_form);
}

TEST(ScenarioOutcomeTests, FirstSetupPassesThroughProvisioningIntoReady) {
    ScenarioRun run{Scenario::FirstSetupToReady, at_ms(0)};

    // The boot prelude ends with no configuration, so setup is required.
    run.source.advance_to(at_ms(500));
    EXPECT_EQ(run.view().layer, PresentationLayer::RecoveryOrProvisioning);
    EXPECT_EQ(run.view().provisioning, ProvisioningPhase::SetupRequired);

    run.source.advance_to(at_ms(2500));
    EXPECT_EQ(run.view().provisioning, ProvisioningPhase::ValidatingConfiguration);

    run.play();

    const auto view = run.view();
    EXPECT_EQ(view.provisioning, ProvisioningPhase::Inactive);
    EXPECT_EQ(view.layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(view.subject, SubjectState::Ready);
    EXPECT_TRUE(view.printer_name_known);
}

TEST(ScenarioOutcomeTests, FullPrintCycleWalksEveryOperationalSubject) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};

    std::vector<SubjectState> seen;
    while (!run.source.finished()) {
        run.source.advance();
        const auto subject = run.view().subject;

        if (seen.empty() || seen.back() != subject) {
            seen.push_back(subject);
        }
    }

    const std::vector<SubjectState> expected = {
        SubjectState::AcquiringState, SubjectState::Ready,  SubjectState::Preparing,
        SubjectState::Printing,       SubjectState::Paused, SubjectState::Printing,
        SubjectState::Completed,      SubjectState::Ready,
    };

    EXPECT_EQ(seen, expected);
}

TEST(ScenarioOutcomeTests, APausedPrintKeepsItsJobAndItsProgress) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};

    run.source.advance_to(at_ms(4000));
    const auto paused = run.view();

    // Production UI §9 Paused: telemetry stays visible and the progress value does
    // not move.
    EXPECT_EQ(paused.subject, SubjectState::Paused);
    EXPECT_TRUE(paused.show_telemetry);
    EXPECT_TRUE(paused.show_operational_form);
    ASSERT_TRUE(paused.progress.known);
    EXPECT_EQ(paused.progress.percent, 40U);
    EXPECT_TRUE(paused.job_name_known);
}

TEST(ScenarioOutcomeTests, CompletionPersistsTheJobAndSettlesToReady) {
    ScenarioRun run{Scenario::FullPrintCycle, at_ms(0)};
    run.play();

    const auto state = run.store.state();
    ASSERT_TRUE(state.last_completed_job.present);
    EXPECT_EQ(state.last_completed_job.terminal_state, PrinterState::Finished);
    EXPECT_EQ(state.last_completed_job.progress.percent, 100U);

    EXPECT_EQ(run.view().subject, SubjectState::Ready);
}

TEST(ScenarioOutcomeTests, StoppedKeepsContextThenSettles) {
    ScenarioRun run{Scenario::PrintingToStopped, at_ms(0)};

    run.source.advance_to(at_ms(4000));
    const auto stopped = run.view();

    EXPECT_EQ(stopped.subject, SubjectState::Stopped);
    EXPECT_TRUE(stopped.show_telemetry);
    ASSERT_TRUE(stopped.layers.known);
    EXPECT_EQ(stopped.layers.current, 86U);

    run.play();
    EXPECT_EQ(run.view().subject, SubjectState::Ready);
}

TEST(ScenarioOutcomeTests, ConfidenceDecaysCurrentThenDegradedThenUnavailable) {
    ScenarioRun run{Scenario::ConfidenceDecay, at_ms(0)};

    run.source.advance_to(at_ms(1100));
    EXPECT_EQ(run.view().confidence, Confidence::Current);
    EXPECT_FALSE(run.view().stale_indicator);

    // Aging, and deliberately still Current with nothing on screen to show for it.
    // A missed poll or two is routine, and UI Behaviour Decision 001 §4 forbids
    // routine retries becoming visible. This step asserted Degraded until
    // 2026-08-13; the change here is the correction reaching the scenario suite.
    run.source.advance_to(at_ms(6000));
    EXPECT_EQ(run.view().confidence, Confidence::Current);
    EXPECT_FALSE(run.view().stale_indicator);

    // Stale: now confidence falls, and it says so.
    run.source.advance_to(at_ms(30000));
    EXPECT_EQ(run.view().confidence, Confidence::Degraded);
    EXPECT_TRUE(run.view().stale_indicator);
    EXPECT_TRUE(run.view().show_telemetry);

    run.source.advance_to(at_ms(70000));
    const auto expired = run.view();
    EXPECT_EQ(expired.confidence, Confidence::Unavailable);
    EXPECT_FALSE(expired.show_telemetry);
    EXPECT_FALSE(expired.remaining.known);

    // The subject is remembered even though it may not be presented.
    EXPECT_EQ(run.store.state().subject, SubjectState::Printing);
}

TEST(ScenarioOutcomeTests, AuthenticationRecoveryReopensAcquisition) {
    ScenarioRun run{Scenario::AuthenticationRecovery, at_ms(0)};

    run.source.advance_to(at_ms(100));
    EXPECT_EQ(run.view().layer, PresentationLayer::AccessCondition);
    EXPECT_EQ(run.view().access_condition, AccessCondition::AuthenticationRequired);

    // §16.2: a corrected credential is a configuration change, and only that reopens
    // the attempt.
    run.source.advance_to(at_ms(1000));
    EXPECT_EQ(run.view().access_condition, AccessCondition::None);

    run.play();
    EXPECT_EQ(run.view().subject, SubjectState::Ready);
    EXPECT_EQ(run.view().confidence, Confidence::Current);
}

TEST(ScenarioOutcomeTests, FirmwareUpdateKeepsTheScreenThroughRequiredReboot) {
    ScenarioRun run{Scenario::FirmwareUpdateLifecycle, at_ms(0)};

    run.source.advance_to(at_ms(2000));
    // Checking and Available are information, not an operation (§18.6).
    EXPECT_EQ(run.view().layer, PresentationLayer::SubjectPresentation);

    run.source.advance_to(at_ms(3000));
    EXPECT_EQ(run.view().layer, PresentationLayer::ExclusiveOperation);
    EXPECT_EQ(run.view().device_operation, DeviceOperation::FirmwareUpdating);
    ASSERT_TRUE(run.view().update_progress.known);
    EXPECT_EQ(run.view().update_progress.percent, 20U);

    // Production UI §9: update progress is never the operational form.
    EXPECT_FALSE(run.view().show_operational_form);

    run.play();
    // Owner-approved lifecycle: successful selection retains the update surface
    // and mutation guards until the communicated reboot, not ordinary operation.
    EXPECT_EQ(run.view().layer, PresentationLayer::ExclusiveOperation);
    EXPECT_EQ(run.view().device_operation, DeviceOperation::FirmwareUpdating);
}

TEST(ScenarioOutcomeTests, RecoveryClearsButADeviceFaultDoesNot) {
    ScenarioRun run{Scenario::RecoveryAndDeviceFault, at_ms(0)};

    run.source.advance_to(at_ms(1000));
    EXPECT_EQ(run.view().layer, PresentationLayer::RecoveryOrProvisioning);
    EXPECT_EQ(run.view().device_operation, DeviceOperation::RecoveryMode);

    run.source.advance_to(at_ms(5000));
    EXPECT_EQ(run.view().layer, PresentationLayer::SubjectPresentation);

    run.source.advance_to(at_ms(6000));
    EXPECT_EQ(run.view().layer, PresentationLayer::DeviceFault);
    EXPECT_EQ(run.view().device_fault, DeviceFaultReason::LowMemory);
}

TEST(ScenarioOutcomeTests, AModalDrawsOverALivePrintAndGivesItBack) {
    ScenarioRun run{Scenario::ModalOverOperationalSubject, at_ms(0)};

    run.source.advance_to(at_ms(2000));
    const auto settings = run.view();

    EXPECT_EQ(settings.layer, PresentationLayer::ModalSurface);
    EXPECT_EQ(settings.modal_surface, ModalSurface::FactoryResetConfirmation);

    // UIAD 001 §9: the subject underneath continues to exist.
    EXPECT_EQ(settings.subject, SubjectState::Printing);
    EXPECT_TRUE(settings.show_operational_form);
    EXPECT_TRUE(settings.show_telemetry);

    // Opening the confirmation is not performing the reset.
    EXPECT_FALSE(run.store.state().factory_resetting);

    run.source.advance_to(at_ms(5000));
    EXPECT_EQ(run.view().layer, PresentationLayer::SubjectPresentation);
    EXPECT_EQ(run.view().subject, SubjectState::Printing);

    // ⬅️ ONE MODAL PAIR SINCE MILESTONE 3.10. The scenario used to open Settings first and the
    // confirmation second; owner ruling 1 unassigned Settings from the button, so the Settings pair
    // was removed rather than scripted against a gesture that no longer opens anything.
    run.play();
    EXPECT_EQ(run.view().modal_surface, ModalSurface::None);
    EXPECT_EQ(run.view().subject, SubjectState::Printing);

    // ⛔ AND THE SCENARIO NEVER PERFORMS A RESET. It presses the very long press ONCE; a second
    // press inside the confirmation window would confirm one, and a development scenario that
    // wiped a device's configuration while demonstrating a modal would be a trap.
    EXPECT_FALSE(run.store.state().factory_resetting);
}
