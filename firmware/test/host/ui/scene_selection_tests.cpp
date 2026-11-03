// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "fake_source/fixtures.hpp"
#include "scene_selection.hpp"
#include "support/state_builders.hpp"

// The scene-selection suite.
//
// What is protected: the six operational subject states resolve to the operational
// composition and everything else resolves to nothing. The mapping is walked exhaustively
// over both enumerations, so a subject state or a layer added later cannot quietly acquire a
// scene it was never given.
//
// Driven from the real fixtures where possible. A hand-built Presentation could express a
// resolution the projection cannot produce, and a selector tested against one would be tested
// against a fiction.

using namespace prd::ui;
using prd::app_model::Confidence;
using prd::app_model::Presentation;
using prd::app_model::PresentationLayer;
using prd::app_model::project;
using prd::app_model::ProvisioningPhase;
using prd::app_model::SubjectState;
using prd::fake_source::Fixture;

TEST(SceneSelectionTests, FirstPrinterErrorNeedsNoInventedSubjectAndDoesNotOutrankAccess) {
    Presentation view;
    view.layer = PresentationLayer::SubjectPresentation;
    view.confidence = Confidence::Current;
    view.subject = SubjectState::AcquiringState;
    view.subject_condition = prd::app_model::SubjectCondition::Error;
    EXPECT_EQ(select(view), Scene::Operational);
    view.confidence = Confidence::Unavailable;
    EXPECT_EQ(select(view), Scene::Unimplemented);
    view.confidence = Confidence::Current;
    view.layer = PresentationLayer::AccessCondition;
    view.access_condition = prd::app_model::AccessCondition::AuthenticationRequired;
    EXPECT_EQ(select(view), Scene::Access);
}

TEST(SceneSelectionTests, FirstAttentionNeedsNoInventedSubjectAndDoesNotOutrankAccess) {
    Presentation view;
    view.layer = PresentationLayer::SubjectPresentation;
    view.confidence = Confidence::Current;
    view.subject = SubjectState::AcquiringState;
    view.subject_condition = prd::app_model::SubjectCondition::Attention;
    EXPECT_EQ(select(view), Scene::Operational);
    view.confidence = Confidence::Unavailable;
    EXPECT_EQ(select(view), Scene::Unimplemented);
    view.confidence = Confidence::Current;
    view.layer = PresentationLayer::AccessCondition;
    view.access_condition = prd::app_model::AccessCondition::AuthenticationRequired;
    EXPECT_EQ(select(view), Scene::Access);
}

TEST(SceneSelectionTests, FirmwareUpdatingOwnsItsDedicatedScene) {
    Presentation view;
    view.layer = PresentationLayer::ExclusiveOperation;
    view.device_operation = prd::app_model::DeviceOperation::FirmwareUpdating;
    view.ota_phase = prd::app_model::OtaPhase::Downloading;

    EXPECT_EQ(select(view), Scene::FirmwareUpdate);

    view.ota_phase = prd::app_model::OtaPhase::Verifying;
    EXPECT_EQ(select(view), Scene::FirmwareUpdate);
    view.ota_phase = prd::app_model::OtaPhase::Installing;
    EXPECT_EQ(select(view), Scene::FirmwareUpdate);
    view.ota_phase = prd::app_model::OtaPhase::RebootRequired;
    EXPECT_EQ(select(view), Scene::FirmwareUpdate);
}

TEST(SceneSelectionTests, NonPendingUpdateFailureReusesTheSharedStatementScene) {
    Presentation view;
    view.layer = PresentationLayer::SubjectPresentation;
    view.subject = SubjectState::Ready;
    view.confidence = Confidence::Current;
    view.ota_phase = prd::app_model::OtaPhase::Failed;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

TEST(SceneSelectionTests, FirstSafeReadyUpdateNoticeReusesTheSharedStatementScene) {
    Presentation view;
    view.layer = PresentationLayer::SubjectPresentation;
    view.subject = SubjectState::Ready;
    view.confidence = Confidence::Current;
    view.ota_phase = prd::app_model::OtaPhase::Available;
    view.update_notice = true;

    EXPECT_EQ(select(view), Scene::Provisioning);
    view.update_notice = false;
    EXPECT_EQ(select(view), Scene::Operational);
}

TEST(SceneSelectionTests, PendingImageValidationFaultStillWinsOverUpdateFailure) {
    Presentation view;
    view.layer = PresentationLayer::DeviceFault;
    view.device_fault = prd::app_model::DeviceFaultReason::OtaValidationFailure;
    view.ota_phase = prd::app_model::OtaPhase::Failed;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

namespace {

constexpr std::array<PresentationLayer, 7> kAllLayers{
    PresentationLayer::DeviceFault,         PresentationLayer::ExclusiveOperation,
    PresentationLayer::SystemLifecycle,     PresentationLayer::RecoveryOrProvisioning,
    PresentationLayer::AccessCondition,     PresentationLayer::ModalSurface,
    PresentationLayer::SubjectPresentation,
};

// The operational family — Production UI §9's Ready, Preparing, Printing, Paused, Completed
// and Stopped, which are the states the one composition covers.
constexpr std::array<SubjectState, 6> kOperationalStates{
    SubjectState::Ready,  SubjectState::Preparing, SubjectState::Printing,
    SubjectState::Paused, SubjectState::Completed, SubjectState::Stopped,
};

constexpr std::array<SubjectState, 7> kAllSubjects{
    SubjectState::AcquiringState, SubjectState::Ready,  SubjectState::Preparing,
    SubjectState::Printing,       SubjectState::Paused, SubjectState::Completed,
    SubjectState::Stopped,
};

constexpr std::array<Confidence, 3> kAllConfidences{Confidence::Current, Confidence::Degraded,
                                                    Confidence::Unavailable};

// The fixtures whose presentations the operational composition draws.
//
// NINE FIXTURES, SIX STATES. The last three add no member to the family: two are Printing — one
// at reduced confidence, one with no job record — and one is Ready at reduced confidence, so all
// three are drawn by the same composition. Their presence here is the point: the selector's
// condition is the subject state and the layer, and neither confidence (UIDD 004 §10) nor a
// missing record (§3.2) may narrow it — at either end of the energy axis.
constexpr std::array<Fixture, 9> kOperationalFixtures{
    Fixture::Ready,
    Fixture::Preparing,
    Fixture::Printing,
    Fixture::Paused,
    Fixture::Completed,
    Fixture::Stopped,
    Fixture::PrintingDegraded,
    Fixture::ReadyDegraded,
    Fixture::PrintingUnknownProgress,
};

[[nodiscard]] bool is_operational_fixture(const Fixture fixture) {
    for (const auto candidate : kOperationalFixtures) {
        if (candidate == fixture) {
            return true;
        }
    }

    return false;
}

// A presentation assembled directly, used ONLY to walk the enumerations exhaustively.
// Every behavioural assertion below goes through a real fixture instead.
Presentation resolved(const PresentationLayer layer, const SubjectState subject,
                      const Confidence confidence) {
    Presentation view;
    view.layer = layer;
    view.subject = subject;
    view.confidence = confidence;
    view.acquiring_connection =
        layer == PresentationLayer::SubjectPresentation && subject == SubjectState::AcquiringState;
    return view;
}

} // namespace

// --- The family the composition covers --------------------------------------

TEST(SceneSelectionTests, EveryOperationalFixtureSelectsTheOperationalScene) {
    for (const auto fixture : kOperationalFixtures) {
        const auto state = prd::fake_source::state_for(fixture);
        EXPECT_EQ(select(project(state)), Scene::Operational)
            << prd::fake_source::name(fixture) << " must resolve to the operational composition";
    }
}

TEST(SceneSelectionTests, NoOtherFixtureSelectsTheOperationalScene) {
    // All 24 fixtures, so a selector that widened its condition fails here rather than on the
    // panel — and one that narrowed it fails the test above.
    //
    // Since Milestone 3.6 two of them resolve to the provisioning composition rather than to
    // nothing. Named explicitly rather than folded into a "not operational" bucket: the point
    // of this test is that every fixture's scene is stated, and a fixture that quietly changed
    // scene should fail here.
    for (std::size_t index = 0; index < prd::fake_source::kFixtureCount; ++index) {
        const auto fixture = prd::fake_source::fixture_at(index);
        const auto presentation = project(prd::fake_source::state_for(fixture));
        const auto scene = select(presentation);

        Scene expected = Scene::Unimplemented;
        if (presentation.device_operation == prd::app_model::DeviceOperation::FactoryResetting ||
            presentation.modal_surface == prd::app_model::ModalSurface::FactoryResetConfirmation) {
            // Production UI §9's Factory Reset screen — Milestone 3.10. Checked before the
            // operational family, because a fixture that opens the confirmation over a live print
            // has an operational subject underneath and the modal is what resolves.
            expected = Scene::FactoryReset;
        } else if (presentation.device_operation ==
                   prd::app_model::DeviceOperation::FirmwareUpdating) {
            expected = Scene::FirmwareUpdate;
        } else if (is_operational_fixture(fixture)) {
            expected = Scene::Operational;
        } else if (presentation.layer == PresentationLayer::AccessCondition ||
                   fixture == Fixture::AcquiringPrinterState) {
            expected = Scene::Access;
        } else if (presentation.layer == PresentationLayer::RecoveryOrProvisioning &&
                   (presentation.provisioning != ProvisioningPhase::Inactive ||
                    presentation.device_operation ==
                        prd::app_model::DeviceOperation::RecoveryMode)) {
            // Every provisioning phase but Inactive draws the provisional scene from
            // Milestone 3.8 onward — validating and failed included, because the setup
            // network is still up through both.
            //
            // ⬅️ AND RECOVERY FROM MILESTONE 3.10. UIAD 001 §12.3 already grouped Recovery Mode
            // with the provisioning phases in one tier; it now shares their composition, drawing
            // its title and Production UI §8's Status Message with no session rows. A device that
            // enters recovery on its own must not land on a blank panel.
            expected = Scene::Provisioning;
        }

        EXPECT_EQ(scene, expected) << prd::fake_source::name(fixture);
    }
}

// --- Exhaustive mapping -----------------------------------------------------

TEST(SceneSelectionTests, TheOperationalSceneIsSelectedForExactlyTheFamily) {
    // 7 layers × 7 subjects × 3 confidences = 147 combinations. Exactly those with the subject
    // layer, an operational subject state, and confidence that permits presenting it.
    std::size_t selections = 0;

    for (const auto layer : kAllLayers) {
        for (const auto subject : kAllSubjects) {
            for (const auto confidence : kAllConfidences) {
                const Scene scene = select(resolved(layer, subject, confidence));

                const bool operational = layer == PresentationLayer::SubjectPresentation &&
                                         subject != SubjectState::AcquiringState &&
                                         confidence != Confidence::Unavailable;

                const bool acquiring = layer == PresentationLayer::SubjectPresentation &&
                                       subject == SubjectState::AcquiringState;
                EXPECT_EQ(scene, operational ? Scene::Operational
                                             : (acquiring ? Scene::Access : Scene::Unimplemented))
                    << "layer=" << prd::app_model::log_label(layer)
                    << " subject=" << static_cast<int>(subject)
                    << " confidence=" << static_cast<int>(confidence);

                if (scene == Scene::Operational) {
                    ++selections;
                }
            }
        }
    }

    // Six operational states × two presentable confidences.
    EXPECT_EQ(selections, kOperationalStates.size() * 2U);
}

TEST(SceneSelectionTests, EveryLayerAboveTheSubjectYieldsNoSceneExceptProvisioning) {
    // UIAD 001 §12's higher tiers are compositions this phase does not build — with one
    // exception since Milestone 3.6, which builds §12.3's provisioning screen. That tier is
    // covered exhaustively by its own tests below; here it is skipped rather than silently
    // relied upon to be inert, which is what `resolved()` leaving the phase Inactive would
    // otherwise make it.
    //
    // Asserted with the subject set to Printing throughout, so what is tested is that the
    // layer wins and not that the subject happened to be uninteresting.
    for (const auto layer : kAllLayers) {
        if (layer == PresentationLayer::SubjectPresentation ||
            layer == PresentationLayer::RecoveryOrProvisioning) {
            continue;
        }

        EXPECT_EQ(select(resolved(layer, SubjectState::Printing, Confidence::Current)),
                  Scene::Unimplemented)
            << "layer=" << prd::app_model::log_label(layer);
    }
}

// --- The provisioning tier (Milestone 3.6) ----------------------------------

namespace {

constexpr std::array<ProvisioningPhase, 5> kAllProvisioningPhases{
    ProvisioningPhase::Inactive,    ProvisioningPhase::SetupRequired,
    ProvisioningPhase::SetupActive, ProvisioningPhase::ValidatingConfiguration,
    ProvisioningPhase::SetupFailed,
};

Presentation provisioning(const ProvisioningPhase phase) {
    Presentation view = resolved(PresentationLayer::RecoveryOrProvisioning,
                                 SubjectState::AcquiringState, Confidence::Unavailable);
    view.provisioning = phase;
    return view;
}

} // namespace

TEST(SceneSelectionTests, TheTwoSetupPhasesSelectTheProvisioningScene) {
    // The two phases Milestone 3.5 actually produces: SetupRequired before the access point
    // is up, SetupActive once it is.
    EXPECT_EQ(select(provisioning(ProvisioningPhase::SetupRequired)), Scene::Provisioning);
    EXPECT_EQ(select(provisioning(ProvisioningPhase::SetupActive)), Scene::Provisioning);
}

TEST(SceneSelectionTests, EveryProvisioningPhaseButInactiveDrawsTheProvisionalScene) {
    // ⬅️ REVERSED AT MILESTONE 3.8. These two used to select no scene, on the reasoning
    // that drawing the setup screen would "show a network to join at the moment the device
    // is no longer offering one". That reasoning turned out to be wrong about the flow: the
    // setup access point stays up through validation AND through a recoverable failure
    // (owner rulings D-2 and D-6), so the network on the panel is joinable throughout.
    EXPECT_EQ(select(provisioning(ProvisioningPhase::ValidatingConfiguration)),
              Scene::Provisioning);
    EXPECT_EQ(select(provisioning(ProvisioningPhase::SetupFailed)), Scene::Provisioning);

    // The layer resolved to provisioning while the phase says there is none. Not reachable
    // through the projection, and not a screen.
    EXPECT_EQ(select(provisioning(ProvisioningPhase::Inactive)), Scene::Unimplemented);
}

TEST(SceneSelectionTests, TheProvisioningSceneIsSelectedForExactlyFourPhases) {
    std::size_t selections = 0;

    for (const auto phase : kAllProvisioningPhases) {
        if (select(provisioning(phase)) == Scene::Provisioning) {
            ++selections;
        }
    }

    // Two at Milestone 3.6, four from 3.8: validating and failed joined them when the flow
    // that produces those phases arrived. Inactive is the only one without a scene.
    EXPECT_EQ(selections, 4U) << "a provisioning phase gained or lost a scene";
}

TEST(SceneSelectionTests, NoOtherLayerReachesTheProvisioningScene) {
    // The provisioning phase is carried on every presentation (UIAD 001 §9 keeps lower
    // layers alive), so a selector that read the phase without checking the layer would put
    // the setup screen over a live Printing composition.
    for (const auto layer : kAllLayers) {
        if (layer == PresentationLayer::RecoveryOrProvisioning) {
            continue;
        }

        for (const auto phase : kAllProvisioningPhases) {
            Presentation view = resolved(layer, SubjectState::Printing, Confidence::Current);
            view.provisioning = phase;

            EXPECT_NE(select(view), Scene::Provisioning)
                << "layer=" << prd::app_model::log_label(layer);
        }
    }
}

TEST(SceneSelectionTests, TheOperationalFamilyIsUnaffectedByTheProvisioningPhase) {
    // The regression this checkpoint most needs to not have: M3.6 must not disturb the
    // Operational Family. Every operational state, at every provisioning phase, still
    // resolves to the operational composition.
    for (const auto subject : kOperationalStates) {
        for (const auto phase : kAllProvisioningPhases) {
            Presentation view =
                resolved(PresentationLayer::SubjectPresentation, subject, Confidence::Current);
            view.provisioning = phase;

            EXPECT_EQ(select(view), Scene::Operational)
                << "subject=" << prd::app_model::log_label(subject);
        }
    }
}

TEST(SceneSelectionTests, AcquiringStateIsNotPartOfTheFamily) {
    // §9's Connecting is its own screen — connection status and printer identity, with the
    // ring and the telemetry hidden. It is not the operational composition with empty fields,
    // so routing it here would draw a frame for a state that has nothing to put in it.
    for (const auto confidence : kAllConfidences) {
        EXPECT_EQ(select(resolved(PresentationLayer::SubjectPresentation,
                                  SubjectState::AcquiringState, confidence)),
                  Scene::Access);
    }

    EXPECT_EQ(select(project(prd::fake_source::state_for(Fixture::AcquiringPrinterState))),
              Scene::Access);
}

TEST(SceneSelectionTests, AccessFailuresRespectResolvedHigherPriorityAndDoNotStealLoss) {
    for (const auto condition : {prd::app_model::AccessCondition::AuthenticationRequired,
                                 prd::app_model::AccessCondition::UnsupportedInterface}) {
        Presentation view;
        view.layer = PresentationLayer::AccessCondition;
        view.access_condition = condition;
        EXPECT_EQ(select(view), Scene::Access);
        for (const auto layer :
             {PresentationLayer::DeviceFault, PresentationLayer::ExclusiveOperation,
              PresentationLayer::SystemLifecycle, PresentationLayer::ModalSurface}) {
            view.layer = layer;
            EXPECT_EQ(select(view), Scene::Unimplemented);
        }
    }
    Presentation lost;
    lost.layer = PresentationLayer::SubjectPresentation;
    lost.observation_lost = true;
    EXPECT_EQ(select(lost), Scene::Unimplemented);
}

TEST(SceneSelectionTests, InitialAcquisitionEndsOnFailureAndDoesNotReturnDuringRetries) {
    using namespace prd::app_model;
    auto state = prd::fake_source::state_for(Fixture::AcquiringPrinterState);
    auto view = project(state);
    ASSERT_TRUE(view.acquiring_connection);
    EXPECT_FALSE(view.observation_lost);
    EXPECT_FALSE(state.loss_episode.active);
    EXPECT_EQ(select(view), Scene::Access);

    ConnectionObservation observation;
    observation.phase = ConnectionPhase::PrinterUnavailable;
    state = reduce(state, Event::connection_observed(prd::test::at_ms(10000), observation));
    view = project(state);
    EXPECT_FALSE(view.acquiring_connection);
    EXPECT_TRUE(view.observation_lost);
    EXPECT_TRUE(view.observation_lost_acknowledgement);
    EXPECT_FALSE(view.printer_name_known);
    EXPECT_EQ(select(view), Scene::Unimplemented);
    const auto generation = view.observation_lost_generation;

    observation.phase = ConnectionPhase::Probing;
    state = reduce(state, Event::connection_observed(prd::test::at_ms(12000), observation));
    view = project(state);
    EXPECT_FALSE(view.acquiring_connection);
    EXPECT_EQ(select(view), Scene::Unimplemented);
    EXPECT_EQ(view.observation_lost_generation, generation);

    observation.phase = ConnectionPhase::Connected;
    state = reduce(state, Event::connection_observed(prd::test::at_ms(13000), observation));
    EXPECT_TRUE(project(state).acquiring_connection);
    EXPECT_FALSE(project(state).observation_lost_acknowledgement);
}

TEST(SceneSelectionTests, RoutineProbingNeverReplacesAnObservedSubjectWithConnecting) {
    using namespace prd::app_model;
    auto state = prd::test::printing_device(prd::test::at_ms(1000));
    ConnectionObservation probing;
    probing.phase = ConnectionPhase::Probing;
    state = reduce(state, Event::connection_observed(prd::test::at_ms(2000), probing));
    EXPECT_FALSE(project(state).acquiring_connection);
    EXPECT_EQ(select(project(state)), Scene::Operational);
}

// --- Confidence (UIAD 001 §7) -----------------------------------------------

TEST(SceneSelectionTests, DegradedConfidenceStillSelectsTheWholeFamily) {
    // §7: a recent known state "may still be represented with reduced confidence", and §11.5
    // permits it behind an explicit stale indicator — which the composition now has.
    for (const auto subject : kOperationalStates) {
        EXPECT_EQ(
            select(resolved(PresentationLayer::SubjectPresentation, subject, Confidence::Degraded)),
            Scene::Operational)
            << "subject=" << static_cast<int>(subject);
    }
}

TEST(SceneSelectionTests, UnavailableConfidenceSelectsNoSceneForAnyState) {
    // §7: "no previous Subject State may be presented as current." §9 gives this its own
    // screen — Printer Unavailable — rather than the operational composition with empty
    // fields.
    for (const auto subject : kOperationalStates) {
        EXPECT_EQ(select(resolved(PresentationLayer::SubjectPresentation, subject,
                                  Confidence::Unavailable)),
                  Scene::Unimplemented)
            << "subject=" << static_cast<int>(subject);
    }

    EXPECT_EQ(select(project(prd::fake_source::state_for(Fixture::PrinterUnavailable))),
              Scene::Unimplemented);
}

// --- Labels -----------------------------------------------------------------

TEST(SceneSelectionTests, EverySceneHasADistinctLabel) {
    // The labels reach the evidence log, where two scenes sharing one would make a
    // transition unreadable.
    constexpr std::array<Scene, 5> kAllScenes{Scene::Unimplemented, Scene::Operational,
                                              Scene::Provisioning, Scene::FactoryReset,
                                              Scene::Access};

    for (std::size_t left = 0; left < kAllScenes.size(); ++left) {
        EXPECT_NE(log_label(kAllScenes[left]), "unknown");

        for (std::size_t right = left + 1; right < kAllScenes.size(); ++right) {
            EXPECT_NE(log_label(kAllScenes[left]), log_label(kAllScenes[right]));
        }
    }
}

// --- Production UI §9's Factory Reset screen (Milestone 3.10) ----------------
//
// ⛔ THE POINT OF THESE IS THE LAST ONE. Architecture §5.6 requires the reset to be requested
// through an ON-SCREEN confirmation sequence. Before this scene existed, both halves of the
// operation resolved to no scene and the panel showed only the controller's diagnostic annotation —
// one very-long press away from erasing the device. These assert that the destructive state cannot
// be reached without the composition that asks for it having been on screen first.

TEST(SceneSelectionTests, TheConfirmationSelectsTheFactoryResetScreen) {
    prd::app_model::AppState state = prd::test::printing_device(prd::test::at_ms(0));
    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(1000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));

    ASSERT_EQ(state.modal_surface, prd::app_model::ModalSurface::FactoryResetConfirmation);
    EXPECT_EQ(select(project(state)), Scene::FactoryReset);
}

TEST(SceneSelectionTests, TheRunningResetSelectsTheFactoryResetScreen) {
    prd::app_model::AppState state = prd::test::printing_device(prd::test::at_ms(0));
    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(1000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));
    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(4000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));

    ASSERT_TRUE(state.factory_resetting);
    EXPECT_EQ(select(project(state)), Scene::FactoryReset);
}

TEST(SceneSelectionTests, ACancelledConfirmationReturnsToTheSubject) {
    prd::app_model::AppState state = prd::test::printing_device(prd::test::at_ms(0));
    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(1000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));
    ASSERT_EQ(select(project(state)), Scene::FactoryReset);

    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(2000),
                                                     prd::app_model::ButtonGesture::ShortPress));

    EXPECT_EQ(select(project(state)), Scene::Operational);
}

// ⛔ NO ERASE WITHOUT ITS PRODUCTION PRESENTATION. Walked as a sequence rather than asserted as a
// pair of independent facts: the destructive state is reachable only through a step whose
// presentation already selected the screen that asks the question.
TEST(SceneSelectionTests, TheDestructiveStateIsUnreachableWithoutTheConfirmationOnScreen) {
    prd::app_model::AppState state = prd::test::printing_device(prd::test::at_ms(0));

    // Before anything: no reset, and the subject is what is drawn.
    ASSERT_FALSE(state.factory_resetting);
    ASSERT_NE(select(project(state)), Scene::FactoryReset);

    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(1000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));

    // The first press arms nothing destructive AND puts the question on screen. Both halves matter:
    // an armed confirmation that drew nothing is the defect this closes.
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(select(project(state)), Scene::FactoryReset);

    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(4000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));

    // Only now, and still on a screen that belongs to the operation.
    EXPECT_TRUE(state.factory_resetting);
    EXPECT_EQ(select(project(state)), Scene::FactoryReset);
}

TEST(SceneSelectionTests, TheFactoryResetScreenHasALogLabel) {
    EXPECT_FALSE(log_label(Scene::FactoryReset).empty());
    EXPECT_NE(log_label(Scene::FactoryReset), log_label(Scene::Provisioning));
    EXPECT_NE(log_label(Scene::FactoryReset), log_label(Scene::Unimplemented));
}

// --- The armed confirmation over a provisioning background (Milestone 3.11 T16) ------

// ⛔ WHY THESE EXIST. On an unprovisioned board the confirmation armed correctly, stayed live for
// its full fifteen seconds, and drew NOTHING: select() returned at its recovery-or-provisioning
// block, which is above the modal checks and returns unconditionally, so the panel kept showing
// "Setup required" while a second press would have erased. Architecture §5.6 requires the reset to
// sit behind an on-screen confirmation; in that background there was none.
//
// ⬅️ WALKED THROUGH THE REAL REDUCER AND THE REAL PROJECTION, not by assembling a Presentation.
// What failed was the composition of three production functions — reduce(), project(), select() —
// so asserting on select() alone with a hand-built input would not have caught it.

namespace {

// A device with no configuration, which is the state a factory reset leaves behind: the
// provisioning tier owns the panel and the subject has nothing to say.
prd::app_model::AppState unprovisioned_device(const ProvisioningPhase phase) {
    prd::app_model::AppState state = prd::test::booted_device(prd::test::at_ms(0));
    state.provisioning = phase;
    return state;
}

prd::app_model::AppState arm_confirmation(prd::app_model::AppState state) {
    return prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(1000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));
}

} // namespace

TEST(SceneSelectionTests, AnArmedConfirmationIsDrawnWhileSetupIsRequired) {
    // ⛔ THE T16 REGRESSION, IN THE EXACT STATE THE BOARD WAS IN.
    prd::app_model::AppState state = unprovisioned_device(ProvisioningPhase::SetupRequired);
    ASSERT_EQ(select(project(state)), Scene::Provisioning) << "the background before the press";

    state = arm_confirmation(state);

    ASSERT_EQ(state.modal_surface, prd::app_model::ModalSurface::FactoryResetConfirmation)
        << "the reducer must arm it — that half always worked";
    EXPECT_EQ(select(project(state)), Scene::FactoryReset)
        << "armed and invisible is what let a wipe happen with nothing on screen";
}

TEST(SceneSelectionTests, AnArmedConfirmationIsDrawnWhileSetupIsActive) {
    // The dangerous one: a PROVISIONED device in a live setup session sits in this same tier, so
    // here the erase would have taken real credentials with no prompt shown.
    prd::app_model::AppState state = unprovisioned_device(ProvisioningPhase::SetupActive);
    ASSERT_EQ(select(project(state)), Scene::Provisioning);

    state = arm_confirmation(state);

    EXPECT_EQ(select(project(state)), Scene::FactoryReset);
}

TEST(SceneSelectionTests, EveryProvisioningTierPhaseDrawsAnArmedConfirmation) {
    // Every phase that has a provisioning screen must yield to the armed modal, so a phase added
    // later cannot quietly reintroduce an invisible confirmation.
    for (const auto phase : kAllProvisioningPhases) {
        if (phase == ProvisioningPhase::Inactive) {
            continue; // no provisioning screen, and not reachable through the projection
        }

        const prd::app_model::AppState state = arm_confirmation(unprovisioned_device(phase));

        EXPECT_EQ(select(project(state)), Scene::FactoryReset)
            << "phase=" << prd::app_model::log_label(phase);
    }
}

TEST(SceneSelectionTests, ProvisioningWithoutTheModalStillDrawsTheProvisioningScene) {
    // The fix must not capture the tier. Without an armed modal, nothing changed.
    for (const auto phase : kAllProvisioningPhases) {
        const prd::app_model::AppState state = unprovisioned_device(phase);
        const Scene expected =
            phase == ProvisioningPhase::Inactive ? Scene::Unimplemented : Scene::Provisioning;

        EXPECT_EQ(select(project(state)), expected) << "phase=" << prd::app_model::log_label(phase);
    }
}

TEST(SceneSelectionTests, AReleasedConfirmationGivesTheProvisioningBackgroundBackImmediately) {
    // UIAD 001 §9: the background continues to exist beneath the modal. Cancelling returns to it
    // rather than to a blank panel — the state underneath was never replaced.
    prd::app_model::AppState state =
        arm_confirmation(unprovisioned_device(ProvisioningPhase::SetupRequired));
    ASSERT_EQ(select(project(state)), Scene::FactoryReset);

    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(2000),
                                                     prd::app_model::ButtonGesture::ShortPress));

    EXPECT_EQ(state.modal_surface, prd::app_model::ModalSurface::None);
    EXPECT_EQ(select(project(state)), Scene::Provisioning);
    EXPECT_EQ(state.provisioning, ProvisioningPhase::SetupRequired) << "the background is intact";
}

TEST(SceneSelectionTests, AResetInProgressStillResolvesThroughTheExclusiveOperationPath) {
    // §12.2 must keep winning, and by its own route: device_operation, not the modal — which the
    // reducer clears the moment the reset is confirmed.
    prd::app_model::AppState state =
        arm_confirmation(unprovisioned_device(ProvisioningPhase::SetupRequired));

    state = prd::app_model::reduce(
        state, prd::app_model::Event::button_gesture(prd::test::at_ms(2000),
                                                     prd::app_model::ButtonGesture::VeryLongPress));

    ASSERT_TRUE(state.factory_resetting);
    EXPECT_EQ(state.modal_surface, prd::app_model::ModalSurface::None)
        << "the question is no longer being asked";
    EXPECT_EQ(project(state).layer, PresentationLayer::ExclusiveOperation);
    EXPECT_EQ(select(project(state)), Scene::FactoryReset);
}

TEST(SceneSelectionTests, TheModalDoesNotDisturbLayersRankedAboveTheProvisioningTier) {
    // The reordering must not let an armed modal outrank a device fault or an install. Both are
    // unreachable with a modal armed — blocks_user_mutations() refuses to arm one — so this asserts
    // the layer ranking still holds for the states that CAN coexist with it.
    for (const auto layer : kAllLayers) {
        if (layer == PresentationLayer::SubjectPresentation ||
            layer == PresentationLayer::RecoveryOrProvisioning) {
            continue;
        }

        // No modal: unchanged from before the fix.
        EXPECT_EQ(select(resolved(layer, SubjectState::Printing, Confidence::Current)),
                  Scene::Unimplemented)
            << "layer=" << prd::app_model::log_label(layer);
    }
}

TEST(SceneSelectionTests, TheOperationalFamilyIsUntouchedByTheReordering) {
    // The regression this fix most needs to not have.
    for (const auto subject : kOperationalStates) {
        const Presentation view =
            resolved(PresentationLayer::SubjectPresentation, subject, Confidence::Current);

        EXPECT_EQ(select(view), Scene::Operational)
            << "subject=" << prd::app_model::log_label(subject);
    }
}

// --- Checkpoint 3c: the Wi-Fi Device Fault owns the panel -----------------------

TEST(SceneSelectionTests, TheWifiDeviceFaultSelectsTheStatementCompositionFromEveryLayer) {
    // ⛔ §12.1 IS FIRST, ALWAYS, AND THE RULING SAYS BOTH DURING AND OUTSIDE PROVISIONING. Whatever
    // else the projection resolved — a provisioning session, an exclusive operation, an operational
    // subject — a local device fault outranks it. The composition drawn is the title-plus-Status-
    // Message one; what makes it the fault's and not recovery's is the typed reason.
    for (const auto layer : kAllLayers) {
        Presentation view = resolved(layer, SubjectState::Printing, Confidence::Current);
        view.device_fault = prd::app_model::DeviceFaultReason::WifiUnavailable;

        EXPECT_EQ(select(view), Scene::Provisioning)
            << "layer=" << prd::app_model::log_label(layer);
    }
}

TEST(SceneSelectionTests, TheWifiDeviceFaultOutranksAProvisioningSessionAndAFactoryReset) {
    // The two tiers that would otherwise have claimed the panel, named explicitly rather than left
    // to the exhaustive sweep above.
    {
        Presentation view = resolved(PresentationLayer::RecoveryOrProvisioning, SubjectState::Ready,
                                     Confidence::Current);
        view.provisioning = prd::app_model::ProvisioningPhase::SetupActive;
        view.device_fault = prd::app_model::DeviceFaultReason::WifiUnavailable;

        EXPECT_EQ(select(view), Scene::Provisioning);
    }

    {
        Presentation view = resolved(PresentationLayer::ExclusiveOperation, SubjectState::Ready,
                                     Confidence::Current);
        view.device_operation = prd::app_model::DeviceOperation::FactoryResetting;
        view.device_fault = prd::app_model::DeviceFaultReason::WifiUnavailable;

        EXPECT_EQ(select(view), Scene::Provisioning)
            << "a device fault outranks an exclusive operation — UIAD 001 §12.1 above §12.2";
    }
}

TEST(SceneSelectionNegativeControlTests, AnotherDeviceFaultDoesNotBorrowTheWifiCopy) {
    // ⛔ THE SELECTION IS KEYED ON THE TYPED REASON, NOT ON THE LAYER. Every other device fault has
    // no composition yet and must resolve to Unimplemented rather than silently drawing words
    // written for a Wi-Fi quarantine — which is what a layer-only test would have let through.
    //
    // ⬅️ StorageFailure LEFT THIS LIST IN AUDIT ITEM 5. It had been raised since Milestone 3.10 and
    // drew a blank panel, which is not a fault the user can act on; it now has its own copy.
    //
    // ⬅️ AND OtaValidationFailure LEFT IT ON 2026-09-12, for the same reason and with a sharper
    // edge: holds_awake() is true for every device fault, so this one drew a blank panel that
    // never dimmed. The list below is the remaining set that still, correctly, has no copy.
    const prd::app_model::DeviceFaultReason others[] = {
        prd::app_model::DeviceFaultReason::BoardMismatch,
        prd::app_model::DeviceFaultReason::DisplayInitializationFailure,
        prd::app_model::DeviceFaultReason::LowMemory,
        prd::app_model::DeviceFaultReason::TaskWatchdog,
    };

    for (const auto reason : others) {
        Presentation view =
            resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
        view.device_fault = reason;

        EXPECT_EQ(select(view), Scene::Unimplemented)
            << "reason=" << prd::app_model::log_label(reason);
    }

    // And the positive half, so the negative control is not vacuous.
    Presentation wifi =
        resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
    wifi.device_fault = prd::app_model::DeviceFaultReason::WifiUnavailable;
    EXPECT_EQ(select(wifi), Scene::Provisioning);
}

// --- Audit item 5's two faults reach their composition -----------------------

TEST(SceneSelectionTests, THE_INCOMPLETE_RESET_FAULT_REACHES_THE_STATEMENT_COMPOSITION) {
    Presentation view =
        resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
    view.device_fault = prd::app_model::DeviceFaultReason::FactoryResetIncomplete;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

TEST(SceneSelectionTests, THE_UPDATE_FAULT_NO_LONGER_DRAWS_A_BLANK_LIT_PANEL) {
    // ⛔ THE DELIVERY BLOCKER, AS AN ASSERTION. A pending image that failed to validate resolved to
    // Unimplemented, and because holds_awake() is true for a device fault the panel stayed lit and
    // empty rather than dimming — the one outcome that says a firmware update did not take, shown
    // as nothing at all.
    Presentation view =
        resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
    view.device_fault = prd::app_model::DeviceFaultReason::OtaValidationFailure;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

TEST(SceneSelectionTests, THE_UPDATE_FAULT_OUTRANKS_EVERY_LOWER_TIER) {
    // §12.1 is first. The subject beneath, a live setup session and an armed confirmation all
    // continue to exist; none of them may draw over it.
    for (const auto surface : {prd::app_model::ModalSurface::None,
                               prd::app_model::ModalSurface::FactoryResetConfirmation}) {
        Presentation view =
            resolved(PresentationLayer::DeviceFault, SubjectState::Printing, Confidence::Current);
        view.device_fault = prd::app_model::DeviceFaultReason::OtaValidationFailure;
        view.modal_surface = surface;
        view.provisioning = prd::app_model::ProvisioningPhase::SetupActive;

        EXPECT_EQ(select(view), Scene::Provisioning);
    }
}

TEST(SceneSelectionTests, THE_STORAGE_FAULT_NO_LONGER_DRAWS_A_BLANK_PANEL) {
    // ⛔ THE BLANK-PANEL REPAIR, AS AN ASSERTION. Before audit item 5 this resolved to Unimplemented,
    // so the one fault that says the device cannot read its own settings showed the user nothing.
    Presentation view =
        resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
    view.device_fault = prd::app_model::DeviceFaultReason::StorageFailure;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

TEST(SceneSelectionTests, AN_INCOMPLETE_RESET_OUTRANKS_THE_FACTORY_RESET_PROGRESS_SCREEN) {
    // app_core keeps factory_resetting set after a failure so the exclusive barrier holds. The panel
    // must nonetheless stop saying "Erasing settings": resolve_device_operation() gives the fault
    // priority, so the presentation the selector sees is DeviceFault, and this asserts the selector
    // agrees rather than falling into the FactoryResetting branch below it.
    Presentation view =
        resolved(PresentationLayer::DeviceFault, SubjectState::Ready, Confidence::Current);
    view.device_fault = prd::app_model::DeviceFaultReason::FactoryResetIncomplete;
    view.device_operation = prd::app_model::DeviceOperation::DeviceFault;

    EXPECT_EQ(select(view), Scene::Provisioning);
}

// ⛔ RECOVERY'S "Nothing has been erased" MUST BE UNREACHABLE AFTER A FAILED RESET. That sentence is
// true on recovery's own route and FALSE once a reset's removals have begun, so the two must not be
// able to coexist. They cannot, and this is where that is proved: the reducer gives a device fault
// priority over recovery_mode, so a state carrying both projects as DeviceFault — and the
// provisioning composition's fault branch returns before its recovery branch is reached.
TEST(SceneSelectionTests, RECOVERY_COPY_IS_UNREACHABLE_WHILE_AN_INCOMPLETE_RESET_FAULT_STANDS) {
    prd::app_model::AppState state;
    state = prd::app_model::reduce(
        state, prd::app_model::Event::recovery_mode_entered(prd::app_model::Timestamp{}));
    ASSERT_EQ(state.device_operation, prd::app_model::DeviceOperation::RecoveryMode)
        << "control: recovery really is reachable without the fault";

    state = prd::app_model::reduce(state,
                                   prd::app_model::Event::device_fault(
                                       prd::app_model::Timestamp{},
                                       prd::app_model::DeviceFaultReason::FactoryResetIncomplete));

    const Presentation view = project(state);

    EXPECT_TRUE(state.recovery_mode) << "the underlying condition is not erased, only outranked";
    EXPECT_EQ(view.device_operation, prd::app_model::DeviceOperation::DeviceFault);
    EXPECT_EQ(view.layer, PresentationLayer::DeviceFault);
    EXPECT_EQ(view.device_fault, prd::app_model::DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_EQ(select(view), Scene::Provisioning);
}
