// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "fake_source/fixtures.hpp"

// The fixture suite.
//
// Two things are being protected. First, that every accepted MVP presentation is
// reachable — twenty-one of them, each asserted against the layer, subject, condition,
// confidence, access condition and modal surface it is supposed to resolve to. A
// fixture that quietly resolves to the wrong layer would take a whole scene with it.
//
// Second, and the reason the fixtures are scripts rather than states: that every one
// of them is reachable *only* through real reducer events. Nothing here hand-builds an
// AppState, so no fixture can describe a state the product cannot enter.

using namespace prd::app_model;
using prd::fake_source::Fixture;
using prd::fake_source::fixture_at;
using prd::fake_source::kFixtureCount;
using prd::fake_source::state_for;

namespace {

std::vector<Event> collect(const Fixture fixture, const Timestamp base = Timestamp::zero()) {
    std::vector<Event> events;

    prd::fake_source::emit_script(
        fixture, base,
        [](const Event& event, void* const context) {
            static_cast<std::vector<Event>*>(context)->push_back(event);
        },
        &events);

    return events;
}

struct Expectation {
    Fixture fixture;
    PresentationLayer layer;
    SubjectState subject;
    Confidence confidence;
    AccessCondition access;
    ModalSurface modal;
};

// The accepted presentation for each fixture. This table is the contract the scene
// layer will be built against, so it is written out in full rather than derived.
const Expectation kExpectations[] = {
    {Fixture::BootStarting, PresentationLayer::SystemLifecycle, SubjectState::AcquiringState,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},

    {Fixture::SetupRequired, PresentationLayer::RecoveryOrProvisioning,
     SubjectState::AcquiringState, Confidence::Unavailable, AccessCondition::None,
     ModalSurface::None},
    {Fixture::SetupActive, PresentationLayer::RecoveryOrProvisioning, SubjectState::AcquiringState,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},
    {Fixture::ValidatingConfiguration, PresentationLayer::RecoveryOrProvisioning,
     SubjectState::AcquiringState, Confidence::Unavailable, AccessCondition::None,
     ModalSurface::None},
    {Fixture::SetupFailed, PresentationLayer::RecoveryOrProvisioning, SubjectState::AcquiringState,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},

    {Fixture::AcquiringPrinterState, PresentationLayer::SubjectPresentation,
     SubjectState::AcquiringState, Confidence::Unavailable, AccessCondition::None,
     ModalSurface::None},
    {Fixture::Ready, PresentationLayer::SubjectPresentation, SubjectState::Ready,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::Preparing, PresentationLayer::SubjectPresentation, SubjectState::Preparing,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::Printing, PresentationLayer::SubjectPresentation, SubjectState::Printing,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::Paused, PresentationLayer::SubjectPresentation, SubjectState::Paused,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::Completed, PresentationLayer::SubjectPresentation, SubjectState::Completed,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::Stopped, PresentationLayer::SubjectPresentation, SubjectState::Stopped,
     Confidence::Current, AccessCondition::None, ModalSurface::None},

    // Still Printing, and still presentable: §11.5 keeps a stale value visible behind
    // an explicit indicator, which is §7's Degraded rather than Unavailable.
    {Fixture::PrintingDegraded, PresentationLayer::SubjectPresentation, SubjectState::Printing,
     Confidence::Degraded, AccessCondition::None, ModalSurface::None},
    // The same reduced confidence at the resting end of the axis, so §10 can be judged as a rule
    // over the family rather than as a Printing behaviour.
    {Fixture::ReadyDegraded, PresentationLayer::SubjectPresentation, SubjectState::Ready,
     Confidence::Degraded, AccessCondition::None, ModalSurface::None},
    // Confidence is perfect; it is the job record that has not arrived.
    {Fixture::PrintingUnknownProgress, PresentationLayer::SubjectPresentation,
     SubjectState::Printing, Confidence::Current, AccessCondition::None, ModalSurface::None},

    // The subject is remembered; it simply may no longer be presented as current.
    {Fixture::PrinterUnavailable, PresentationLayer::SubjectPresentation, SubjectState::Printing,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},

    {Fixture::AuthenticationRequired, PresentationLayer::AccessCondition,
     SubjectState::AcquiringState, Confidence::Unavailable, AccessCondition::AuthenticationRequired,
     ModalSurface::None},
    {Fixture::UnsupportedInterface, PresentationLayer::AccessCondition,
     SubjectState::AcquiringState, Confidence::Unavailable, AccessCondition::UnsupportedInterface,
     ModalSurface::None},

    {Fixture::FirmwareUpdating, PresentationLayer::ExclusiveOperation, SubjectState::Printing,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::FactoryResetting, PresentationLayer::ExclusiveOperation, SubjectState::Printing,
     Confidence::Current, AccessCondition::None, ModalSurface::None},
    {Fixture::RecoveryMode, PresentationLayer::RecoveryOrProvisioning, SubjectState::AcquiringState,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},
    {Fixture::DeviceFault, PresentationLayer::DeviceFault, SubjectState::AcquiringState,
     Confidence::Unavailable, AccessCondition::None, ModalSurface::None},

    {Fixture::FactoryResetConfirmation, PresentationLayer::ModalSurface, SubjectState::Printing,
     Confidence::Current, AccessCondition::None, ModalSurface::FactoryResetConfirmation},
    {Fixture::FactoryResetConfirmation, PresentationLayer::ModalSurface, SubjectState::Printing,
     Confidence::Current, AccessCondition::None, ModalSurface::FactoryResetConfirmation},
};

static_assert(sizeof(kExpectations) / sizeof(kExpectations[0]) == kFixtureCount,
              "every fixture needs an expected presentation");

} // namespace

// --- Validity --------------------------------------------------------------

TEST(FixtureTests, EveryFixtureIsIndexAddressableAndNamed) {
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);
        EXPECT_FALSE(prd::fake_source::name(fixture).empty()) << "index " << index;
    }
}

TEST(FixtureTests, EveryFixtureEmitsEventsAndReachesAMutatedState) {
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);
        const auto events = collect(fixture);
        const auto state = state_for(fixture);

        EXPECT_FALSE(events.empty()) << prd::fake_source::name(fixture);

        // A fixture that changed nothing would be describing the default state, not
        // a presentation.
        EXPECT_GT(state.revision, 0U) << prd::fake_source::name(fixture);
        EXPECT_GT(state.diagnostics.events_applied, 0U) << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, EveryFixtureStateComesFromTheRealReducer) {
    // The property the whole design rests on. Folding the fixture's own script
    // through reduce() by hand must land exactly where state_for() does — so
    // state_for cannot be quietly hand-assembling anything, now or later.
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);

        AppState folded;
        for (const auto& event : collect(fixture)) {
            folded = reduce(folded, event);
        }

        EXPECT_EQ(folded, state_for(fixture)) << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, EveryFixtureIsDeterministic) {
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);
        EXPECT_EQ(state_for(fixture), state_for(fixture)) << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, EventTimestampsNeverGoBackwards) {
    // An out-of-order timestamp inside a fixture would be counted as disorder by the
    // reducer and would make the fixture's own diagnostics dirty.
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);
        const auto events = collect(fixture);

        Timestamp previous = Timestamp::zero();
        for (const auto& event : events) {
            EXPECT_GE(event.observed_at, previous) << prd::fake_source::name(fixture);
            previous = event.observed_at;
        }

        EXPECT_EQ(state_for(fixture).diagnostics.events_out_of_order, 0U)
            << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, NoFixtureRejectsItsOwnObservations) {
    // A sequence number reused inside one fixture would have the reducer discard the
    // second observation, and the fixture would silently describe the first.
    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);

        EXPECT_EQ(state_for(fixture).diagnostics.events_rejected_stale, 0U)
            << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, RebasingShiftsTimeWithoutChangingThePresentation) {
    const auto offset = std::chrono::duration_cast<Timestamp>(std::chrono::seconds{500});

    for (std::size_t index = 0; index < kFixtureCount; ++index) {
        const auto fixture = fixture_at(index);

        const auto early = state_for(fixture, Timestamp::zero());
        const auto late = state_for(fixture, offset);

        EXPECT_NE(early.last_event_at, late.last_event_at) << prd::fake_source::name(fixture);
        const auto early_view = project(early);
        auto late_view = project(late);
        ASSERT_EQ(early_view.stale_observed_at.has_value(),
                  late_view.stale_observed_at.has_value());
        if (late_view.stale_observed_at) {
            EXPECT_EQ(*late_view.stale_observed_at - *early_view.stale_observed_at, offset);
            late_view.stale_observed_at = *late_view.stale_observed_at - offset;
        }
        EXPECT_EQ(early_view, late_view) << prd::fake_source::name(fixture);
    }
}

// --- Expected presentations ------------------------------------------------

TEST(FixtureTests, EveryFixtureProjectsToItsAcceptedPresentation) {
    for (const auto& expected : kExpectations) {
        const auto state = state_for(expected.fixture);
        const auto view = project(state);
        const auto label = prd::fake_source::name(expected.fixture);

        EXPECT_EQ(view.layer, expected.layer) << label;
        EXPECT_EQ(view.subject, expected.subject) << label;
        EXPECT_EQ(view.confidence, expected.confidence) << label;
        EXPECT_EQ(view.access_condition, expected.access) << label;
        EXPECT_EQ(view.modal_surface, expected.modal) << label;

        // None of the fixtures carries a subject condition; attention and error are
        // exercised by the reducer suite, which is where the rule lives.
        EXPECT_EQ(view.subject_condition, SubjectCondition::None) << label;
    }
}

TEST(FixtureTests, EveryLayerIsCoveredByAtLeastOneFixture) {
    // If a presentation layer had no fixture, the scene for it would have nothing to
    // be built or tested against.
    bool covered[7] = {};

    for (const auto& expected : kExpectations) {
        covered[static_cast<std::size_t>(expected.layer)] = true;
    }

    for (std::size_t index = 0; index < 7; ++index) {
        EXPECT_TRUE(covered[index])
            << "no fixture covers layer " << log_label(static_cast<PresentationLayer>(index));
    }
}

TEST(FixtureTests, EverySubjectStateIsCoveredByAtLeastOneFixture) {
    bool covered[7] = {};

    for (const auto& expected : kExpectations) {
        covered[static_cast<std::size_t>(expected.subject)] = true;
    }

    for (std::size_t index = 0; index < 7; ++index) {
        EXPECT_TRUE(covered[index])
            << "no fixture covers subject " << log_label(static_cast<SubjectState>(index));
    }
}

// --- Content rules ---------------------------------------------------------

TEST(FixtureTests, OperationalFixturesCarryTheTelemetryAScenePresents) {
    const auto printing = project(state_for(Fixture::Printing));

    EXPECT_TRUE(printing.show_telemetry);
    EXPECT_TRUE(printing.show_operational_form);
    EXPECT_TRUE(printing.progress.known);
    EXPECT_TRUE(printing.remaining.known);
    EXPECT_TRUE(printing.layers.known);
    EXPECT_TRUE(printing.nozzle.current.known);
    EXPECT_TRUE(printing.bed.current.known);
    EXPECT_TRUE(printing.job_name_known);
    EXPECT_TRUE(printing.printer_name_known);
}

TEST(FixtureTests, ProvisioningFixturesPresentNoPrinterTelemetry) {
    // Production UI §9 Initial Setup: "No simulated printer information shall be
    // displayed." A fixture is exactly where a simulation would creep in, so this is
    // asserted on the fixtures themselves.
    const Fixture provisioning[] = {Fixture::SetupRequired, Fixture::SetupActive,
                                    Fixture::ValidatingConfiguration, Fixture::SetupFailed};

    for (const auto fixture : provisioning) {
        const auto view = project(state_for(fixture));

        EXPECT_FALSE(view.show_telemetry) << prd::fake_source::name(fixture);
        EXPECT_FALSE(view.progress.known) << prd::fake_source::name(fixture);
        EXPECT_FALSE(view.layers.known) << prd::fake_source::name(fixture);
        EXPECT_FALSE(view.nozzle.current.known) << prd::fake_source::name(fixture);
        EXPECT_FALSE(view.show_operational_form) << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, TheDegradedFixtureShowsTheIndicatorWithoutChangingTheComposition) {
    // The composition this fixture exists to put on the panel, asserted here so that
    // what a reviewer is shown at E.6 is a checked claim rather than whatever the
    // model happens to produce.
    //
    // UI Design Decision 004 §10: "Confidence shall change no geometry, luminance,
    // typography, layout or energy... A degraded Printing remains Printing at maximum
    // energy. The machine has not slowed; it has become unobservable."
    const auto view = project(state_for(Fixture::PrintingDegraded));

    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.confidence, Confidence::Degraded);
    EXPECT_TRUE(view.stale_indicator);
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.show_telemetry);

    // Owner ruling 2026-09-08: retain historical progress with the explicit age indicator;
    // live measurements still disappear. The expiry boundary is in projection_tests.cpp.
    EXPECT_TRUE(view.progress.known);
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.nozzle.current.known);

    // What the composition keeps is the point of the state: it is still Printing, still at
    // maximum energy, retaining the last percentage with an explicit indicator.
    EXPECT_TRUE(view.job_record_supplied);
}

TEST(FixtureTests, TheRestingDegradedFixtureCarriesTheSameConfidenceAtTheOtherEnergy) {
    // Added in Phase G.1, and its whole reason for existing is the comparison it makes possible.
    // §10 is a rule over the family — "Confidence shall change no geometry, luminance,
    // typography, layout or energy" — and a rule observed at one energy is indistinguishable
    // from a Printing behaviour. This is the same ageing step, at rest.
    const auto view = project(state_for(Fixture::ReadyDegraded));

    EXPECT_EQ(view.subject, SubjectState::Ready);
    EXPECT_EQ(view.confidence, Confidence::Degraded);
    EXPECT_TRUE(view.stale_indicator);

    // Presence is membership of the family, so the form and the column are both here — at the
    // resting energy, which is where the indicator and the horizon are known to meet. Measuring
    // that is Phase G.6's, and it is undrawable without this fixture.
    EXPECT_TRUE(view.show_operational_form);
    EXPECT_TRUE(view.show_telemetry);
    EXPECT_TRUE(view.printer_name_known);

    // No job was ever observed, so the record is absent for §7's reason and not §10's — and
    // the two are distinguishable, which is what lets the composition keep its shape.
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.job_record_supplied) << "nothing decayed here; nothing was ever supplied";

    // The machine's own readings decay with everything else at Degraded (§10), so this state
    // reaches the panel as a horizon, a printer name and an explicit indicator. That sparseness
    // is the composition Phase G.6 judges.
    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.bed.current.known);
}

TEST(FixtureTests, TheUnknownProgressFixtureKeepsTheFormWithoutARecord) {
    // §3.2: "The record shall never determine the form's shape." The projection
    // therefore reports the form as present while reporting no progress, which is the
    // separation E.4 built and the one composition on the walk that can show it.
    const auto view = project(state_for(Fixture::PrintingUnknownProgress));

    EXPECT_EQ(view.subject, SubjectState::Printing);
    EXPECT_EQ(view.confidence, Confidence::Current);
    EXPECT_FALSE(view.stale_indicator);

    EXPECT_TRUE(view.show_operational_form);
    EXPECT_FALSE(view.progress.known);

    // Nothing about the job is known, because no job has been observed — but the
    // machine's own telemetry and identity are unaffected, which is what keeps this a
    // presence question (§7) rather than a confidence one.
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.job_name_known);
    EXPECT_TRUE(view.printer_name_known);
}

TEST(FixtureTests, UnavailableFixturePresentsNothingAsCurrent) {
    const auto view = project(state_for(Fixture::PrinterUnavailable));

    // §9 Printer Unavailable, as revised 2026-08-31: nothing about the printer is presented.
    //
    // ⛔ THIS LINE WAS REVERSED. It read "but the printer's identity survives" and asserted
    // printer_name_known — a correct reading of the OLD §9, whose Visible list carried "Printer
    // identity (if known)". The revision hides identity at all times, so the assertion is now the
    // negative control for the behaviour it used to require.
    EXPECT_FALSE(view.show_telemetry);
    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.remaining.known);
    EXPECT_FALSE(view.printer_name_known);
    EXPECT_TRUE(view.printer_name.empty());
    EXPECT_TRUE(view.observation_lost);
}

TEST(FixtureTests, ModalFixturesKeepTheOperationalSubjectAlive) {
    // UIAD 001 §9: the underlying subject state, conditions and confidence continue
    // to exist while a modal surface is presented.
    // One modal fixture now: owner ruling 1 of Milestone 3.10 unassigned Settings from the button,
    // so SettingsModal was retired rather than left unable to reach its own state.
    const Fixture modals[] = {Fixture::FactoryResetConfirmation};

    for (const auto fixture : modals) {
        const auto view = project(state_for(fixture));

        EXPECT_EQ(view.subject, SubjectState::Printing) << prd::fake_source::name(fixture);
        EXPECT_TRUE(view.show_telemetry) << prd::fake_source::name(fixture);
        EXPECT_TRUE(view.show_operational_form) << prd::fake_source::name(fixture);
    }
}

TEST(FixtureTests, TerminalFixturesCarryTheirCompletedJob) {
    // §8.1's completion snapshot, which is what Production UI §9 Completed and
    // Stopped present as residual context.
    const auto completed = state_for(Fixture::Completed);
    ASSERT_TRUE(completed.last_completed_job.present);
    EXPECT_EQ(completed.last_completed_job.terminal_state, PrinterState::Finished);
    EXPECT_EQ(completed.last_completed_job.progress.percent, 100U);

    const auto stopped = state_for(Fixture::Stopped);
    ASSERT_TRUE(stopped.last_completed_job.present);
    EXPECT_EQ(stopped.last_completed_job.terminal_state, PrinterState::Stopped);
    EXPECT_EQ(stopped.last_completed_job.layers.current, 86U);
}

TEST(FixtureTests, TheDeviceFaultFixtureCarriesADeviceCause) {
    // UIAD 001 §10: a device fault must never stand in for a printer problem. The
    // fixture reaches it through the §12.4 capacity mismatch, which is a device cause.
    const auto state = state_for(Fixture::DeviceFault);

    EXPECT_EQ(state.device_fault, DeviceFaultReason::BoardMismatch);
    EXPECT_EQ(state.subject_condition, SubjectCondition::None);
}

TEST(FixtureTests, TheFactoryResettingFixturePassedThroughConfirmation) {
    // The reset is only reachable via the confirmation surface, so a fixture that
    // reaches FactoryResetting is itself evidence that the precondition held.
    const auto state = state_for(Fixture::FactoryResetting);

    EXPECT_TRUE(state.factory_resetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_EQ(state.device_operation, DeviceOperation::FactoryResetting);
}

TEST(FixtureTests, NoFixtureCarriesASecretOrAPlaceholder) {
    // §7.3 and §24.3: the state carries has-credential flags and auth metadata only.
    // And UI Design Decision 003: absent information stays absent. Neither can be
    // asserted directly — there is nowhere in the type for a secret, and no string
    // field for a placeholder — so what is checked is that a fixture without
    // telemetry reports absence rather than a zero that a scene could render.
    const auto view = project(state_for(Fixture::AcquiringPrinterState));

    EXPECT_FALSE(view.progress.known);
    EXPECT_FALSE(view.layers.known);
    EXPECT_FALSE(view.nozzle.current.known);
    EXPECT_FALSE(view.bed.current.known);
    EXPECT_FALSE(view.job_name_known);

    const auto state = state_for(Fixture::AcquiringPrinterState);
    EXPECT_TRUE(state.configuration.has_printer_configuration);
    EXPECT_EQ(state.configuration.auth_method, AuthMethod::ApiKey);
}
