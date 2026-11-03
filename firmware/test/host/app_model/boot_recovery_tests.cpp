// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"
#include "fake_source/fixtures.hpp"
#include "fake_source/observations.hpp"
#include "fake_source/scenarios.hpp"
#include "fake_source/source.hpp"
#include "support/state_builders.hpp"

// Entering BootPhase::Starting clears the device-owned conditions.
//
// WHY THIS SUITE EXISTS. A device fault is terminal by design — UIAD 001 §10 gives it the
// highest presentation priority and §16.1 makes it a device error no printer observation
// clears — because on real hardware the recovery is a reboot. The model had no
// representation of that reboot, so a replayed boot left a fault standing and every
// subsequent presentation resolved to Device Fault forever.
//
// It was found on the physical unit, not here: 160 presentations at layer=device-fault
// against 58 at subject-presentation over a ten-minute run, with the product scene never
// drawn again after the eighth scenario. These tests are what stop it returning.

using namespace prd::app_model;
using prd::test::at_ms;
using prd::test::booted_device;

namespace {

AppState faulted_device() {
    AppState state = booted_device(at_ms(0));
    state = reduce(state, Event::device_fault(at_ms(1000), DeviceFaultReason::StorageFailure));
    return state;
}

} // namespace

// --- The rule ---------------------------------------------------------------

TEST(BootRecoveryTests, ADeviceFaultSurvivesEverythingExceptABoot) {
    // The property that makes the fault terminal, asserted first — otherwise the test
    // below would pass for a reducer that simply never held a fault.
    auto state = faulted_device();
    ASSERT_EQ(state.device_fault, DeviceFaultReason::StorageFailure);

    state = reduce(state, Event::printer_snapshot(
                              at_ms(2000), prd::fake_source::snapshot(PrinterState::Ready, 1)));
    state =
        reduce(state, Event::network_observed(at_ms(2100), prd::fake_source::network_connected()));
    state = reduce(state, Event::confidence_aged(at_ms(2200)));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::StorageFailure)
        << "no ordinary observation may clear a device fault";
}

// ⛔ AUDIT ITEM 5's FAULT IS TERMINAL ON THE SAME RULE. It is what holds an incomplete reset's
// exclusive ownership, so anything that could lower it would release a device whose journal may
// still be armed. Only a boot clears it, and a boot is the restart the panel asks the user for.
TEST(BootRecoveryTests, TheIncompleteResetFaultIsTerminalToo) {
    auto state = booted_device(at_ms(0));
    state =
        reduce(state, Event::device_fault(at_ms(1000), DeviceFaultReason::FactoryResetIncomplete));
    ASSERT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete);
    ASSERT_EQ(state.device_operation, DeviceOperation::DeviceFault);

    state = reduce(state, Event::printer_snapshot(
                              at_ms(2000), prd::fake_source::snapshot(PrinterState::Ready, 1)));
    state =
        reduce(state, Event::network_observed(at_ms(2100), prd::fake_source::network_connected()));
    state = reduce(state, Event::confidence_aged(at_ms(2200)));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault);

    // And the restart the copy names really is what clears it.
    state = reduce(state, Event::boot_phase(at_ms(3000), BootPhase::Starting));
    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
}

// --- Fault precedence: a held fault is not silently replaced — audit item 5 ---

TEST(FaultPrecedenceTests, A_HELD_FAULT_IS_NOT_REPLACED_BY_ONE_THAT_DOES_NOT_HOLD_THE_DEVICE) {
    for (const DeviceFaultReason held :
         {DeviceFaultReason::FactoryResetIncomplete, DeviceFaultReason::StorageFailure}) {
        ASSERT_TRUE(fault_holds_device(held)) << "control: it really does hold the device";

        for (const DeviceFaultReason later :
             {DeviceFaultReason::WifiUnavailable, DeviceFaultReason::BoardMismatch,
              DeviceFaultReason::LowMemory, DeviceFaultReason::TaskWatchdog,
              DeviceFaultReason::DisplayInitializationFailure,
              DeviceFaultReason::OtaValidationFailure}) {
            auto state = booted_device(at_ms(0));
            state = reduce(state, Event::device_fault(at_ms(1000), held));
            state = reduce(state, Event::device_fault(at_ms(2000), later));

            EXPECT_EQ(state.device_fault, held)
                << "reason " << static_cast<int>(later) << " replaced a held fault";
        }
    }
}

TEST(FaultPrecedenceTests, NEGATIVE_CONTROL_A_FAULT_THAT_DOES_NOT_HOLD_THE_DEVICE_IS_REPLACED) {
    // ⬅️ THE PRECEDENCE RULE IS A FLOOR, NOT A FREEZE. Ordinary reasons still replace one another
    // exactly as they did; without this the test above would pass for a reducer that had simply
    // stopped accepting faults.
    auto state = booted_device(at_ms(0));
    state = reduce(state, Event::device_fault(at_ms(1000), DeviceFaultReason::LowMemory));
    state = reduce(state, Event::device_fault(at_ms(2000), DeviceFaultReason::BoardMismatch));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::BoardMismatch);
}

TEST(FaultPrecedenceTests, A_HELD_FAULT_IS_STILL_REPLACED_BY_THE_OTHER_HELD_ONE_NEVER_LOWERED) {
    // FactoryResetIncomplete then StorageFailure: both hold the device, so the first stands — the
    // more specific statement about what happened is the one the user is shown.
    auto state = booted_device(at_ms(0));
    state =
        reduce(state, Event::device_fault(at_ms(1000), DeviceFaultReason::FactoryResetIncomplete));
    state = reduce(state, Event::device_fault(at_ms(2000), DeviceFaultReason::StorageFailure));
    EXPECT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete);

    // And the reverse order raises the reset statement, because StorageFailure was not yet held
    // when the more specific one arrived... it was, so it stands. Both directions pinned.
    auto other = booted_device(at_ms(0));
    other = reduce(other, Event::device_fault(at_ms(1000), DeviceFaultReason::StorageFailure));
    other =
        reduce(other, Event::device_fault(at_ms(2000), DeviceFaultReason::FactoryResetIncomplete));
    EXPECT_EQ(other.device_fault, DeviceFaultReason::StorageFailure);
}

TEST(FaultPrecedenceTests, RE_RAISING_THE_SAME_REASON_IS_IDEMPOTENT) {
    auto state = booted_device(at_ms(0));
    state =
        reduce(state, Event::device_fault(at_ms(1000), DeviceFaultReason::FactoryResetIncomplete));
    state =
        reduce(state, Event::device_fault(at_ms(2000), DeviceFaultReason::FactoryResetIncomplete));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::FactoryResetIncomplete);
    EXPECT_TRUE(state.factory_reset_failed);
}

TEST(FaultPrecedenceTests, THE_LATCH_IS_SET_ONLY_BY_THE_RESET_FAULT) {
    for (const DeviceFaultReason reason :
         {DeviceFaultReason::StorageFailure, DeviceFaultReason::WifiUnavailable,
          DeviceFaultReason::BoardMismatch, DeviceFaultReason::LowMemory,
          DeviceFaultReason::TaskWatchdog, DeviceFaultReason::OtaValidationFailure,
          DeviceFaultReason::DisplayInitializationFailure}) {
        auto state = booted_device(at_ms(0));
        state = reduce(state, Event::device_fault(at_ms(1000), reason));

        EXPECT_FALSE(state.factory_reset_failed) << "reason " << static_cast<int>(reason);
    }

    auto reset = booted_device(at_ms(0));
    reset =
        reduce(reset, Event::device_fault(at_ms(1000), DeviceFaultReason::FactoryResetIncomplete));
    EXPECT_TRUE(reset.factory_reset_failed);
}

TEST(FaultPrecedenceTests, THE_LIST_OF_HELD_FAULTS_IS_EXACTLY_TWO) {
    EXPECT_TRUE(fault_holds_device(DeviceFaultReason::FactoryResetIncomplete));
    EXPECT_TRUE(fault_holds_device(DeviceFaultReason::StorageFailure));

    for (const DeviceFaultReason reason :
         {DeviceFaultReason::None, DeviceFaultReason::WifiUnavailable,
          DeviceFaultReason::BoardMismatch, DeviceFaultReason::DisplayInitializationFailure,
          DeviceFaultReason::LowMemory, DeviceFaultReason::TaskWatchdog,
          DeviceFaultReason::OtaValidationFailure}) {
        EXPECT_FALSE(fault_holds_device(reason)) << "reason " << static_cast<int>(reason);
    }
}

// ⬅️ AND THE FAULT OUTRANKS THE RESET'S OWN PRESENTATION, WHICH IS WHY THE PANEL DOES NOT STAY ON
// "Erasing settings". app_core keeps factory_resetting set after a failure so the exclusive barrier
// holds; resolve_device_operation() checks device_fault first, so the truthful statement wins.
TEST(BootRecoveryTests, TheIncompleteResetFaultOutranksFactoryResettingOnThePanel) {
    auto state = booted_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    state = reduce(state, Event::factory_reset_confirmed(at_ms(1500)));
    ASSERT_TRUE(state.factory_resetting);
    ASSERT_EQ(state.device_operation, DeviceOperation::FactoryResetting);

    state =
        reduce(state, Event::device_fault(at_ms(2000), DeviceFaultReason::FactoryResetIncomplete));

    EXPECT_TRUE(state.factory_resetting) << "ownership must be retained";
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault)
        << "the panel must say the reset could not finish, not that it is still erasing";
}

TEST(BootRecoveryTests, EnteringStartingClearsTheFault) {
    auto state = faulted_device();

    state = reduce(state, Event::boot_phase(at_ms(3000), BootPhase::Starting));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
}

TEST(BootRecoveryTests, EnteringStartingClearsRecoveryAndTheModalSurface) {
    auto state = booted_device(at_ms(0));
    state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));
    state = reduce(state, Event::button_gesture(at_ms(1100), ButtonGesture::VeryLongPress));

    ASSERT_TRUE(state.recovery_mode);
    ASSERT_NE(state.modal_surface, ModalSurface::None);

    state = reduce(state, Event::boot_phase(at_ms(2000), BootPhase::Starting));

    EXPECT_FALSE(state.recovery_mode);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
}

TEST(BootRecoveryTests, OnlyStartingClearsItAndNotTheLaterPhases) {
    // The later phases happen while the device is already up. If any of them cleared a
    // fault, a board mismatch raised by the hardware probe would be erased by the very
    // next phase transition — which is the whole point of raising it.
    const BootPhase later[] = {BootPhase::HardwareInitializing, BootPhase::StorageInitializing,
                               BootPhase::LoadingConfiguration, BootPhase::StartingServices,
                               BootPhase::Ready};

    for (const auto phase : later) {
        auto state = faulted_device();
        state = reduce(state, Event::boot_phase(at_ms(3000), phase));

        EXPECT_EQ(state.device_fault, DeviceFaultReason::StorageFailure)
            << "phase " << log_label(phase) << " must not clear a fault";
    }
}

TEST(BootRecoveryTests, AGenuineFaultIsReRaisedByTheProbeThatFollows) {
    // The reason clearing on Starting is safe: the hardware probe is the only thing that
    // can still see a hardware fault, and it runs moments later in every boot.
    auto state = faulted_device();
    state = reduce(state, Event::boot_phase(at_ms(3000), BootPhase::Starting));
    ASSERT_EQ(state.device_fault, DeviceFaultReason::None);

    state =
        reduce(state, Event::hardware_probed(at_ms(3100), prd::fake_source::mismatched_hardware()));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::BoardMismatch)
        << "a real hardware fault must come back on the next probe";
}

TEST(BootRecoveryTests, ClearingOnStartingIsANoOpForAFreshBoot) {
    // On real hardware a boot begins from a default-constructed AppState, so the branch
    // can never change observed behaviour. Asserted so the claim in the reducer's comment
    // is checked rather than believed.
    AppState fresh;
    const AppState after = reduce(fresh, Event::boot_phase(at_ms(0), BootPhase::Starting));

    // Compared against the state the branch ran on, not against hand-written values: the claim
    // is that every field it writes was already what it wrote, so a real boot cannot tell
    // whether it ran. A field added to the branch later is covered only if it is added here
    // too, which is the point of listing them.
    EXPECT_EQ(after.device_fault, fresh.device_fault);
    EXPECT_EQ(after.recovery_mode, fresh.recovery_mode);
    EXPECT_EQ(after.factory_resetting, fresh.factory_resetting);
    EXPECT_EQ(after.modal_surface, fresh.modal_surface);
    EXPECT_EQ(after.last_printer_sequence, fresh.last_printer_sequence);
    EXPECT_EQ(after.last_job_sequence, fresh.last_job_sequence);
    EXPECT_EQ(after.current_job, fresh.current_job) << "clearing the job is a no-op on a boot";
    EXPECT_EQ(after.last_completed_job, fresh.last_completed_job);
}

// --- The job the device no longer holds (G.2) -------------------------------

TEST(BootRecoveryTests, EnteringStartingClearsTheCurrentJob) {
    // UI Design Decision 004 §7: a component is present only if the machine supplies its
    // value "and that value is true of the machine now". A job carried across a reboot has
    // not been reported since the device started, so nothing about it is true of the device
    // now — it is not even certainly the same printer.
    AppState state = prd::test::printing_device(at_ms(0));
    ASSERT_TRUE(state.current_job.present);
    ASSERT_TRUE(state.current_job.layers.known);

    state = reduce(state, Event::boot_phase(at_ms(1000), BootPhase::Starting));

    EXPECT_FALSE(state.current_job.present);
    EXPECT_FALSE(state.current_job.progress.known);
    EXPECT_FALSE(state.current_job.layers.known);
    EXPECT_FALSE(state.current_job.remaining.known);
    EXPECT_FALSE(state.current_job.elapsed.known);
    EXPECT_TRUE(state.current_job.display_name.empty());
    EXPECT_TRUE(state.current_job.id.empty());
}

TEST(BootRecoveryTests, EnteringStartingLeavesTheCompletionSnapshotAlone) {
    // §8.1's completion snapshot has a different lifetime and a different owner. Architecture
    // Spec §15.1 loads it from storage during boot (step 11), so a boot that cleared it would
    // erase the record the boot sequence had just restored.
    AppState state = prd::test::printing_device(at_ms(0));
    state = reduce(state, Event::printer_snapshot(
                              at_ms(1000), prd::fake_source::snapshot(PrinterState::Finished, 2)));

    ASSERT_TRUE(state.last_completed_job.present);
    const CompletedJobSnapshot before = state.last_completed_job;

    state = reduce(state, Event::boot_phase(at_ms(2000), BootPhase::Starting));

    ASSERT_FALSE(state.current_job.present) << "the current job goes";
    EXPECT_EQ(state.last_completed_job, before) << "and the completion snapshot does not";
}

TEST(BootRecoveryTests, OnlyStartingClearsTheJobAndNotTheLaterPhases) {
    // The later phases happen while the device is already up, with a printer being polled.
    // If any of them cleared the job, an ordinary phase transition would erase a live print.
    const BootPhase later[] = {BootPhase::HardwareInitializing, BootPhase::StorageInitializing,
                               BootPhase::LoadingConfiguration, BootPhase::StartingServices,
                               BootPhase::Ready};

    for (const auto phase : later) {
        AppState state = prd::test::printing_device(at_ms(0));
        state = reduce(state, Event::boot_phase(at_ms(1000), phase));

        EXPECT_TRUE(state.current_job.present)
            << "phase " << log_label(phase) << " must not clear the job";
        EXPECT_EQ(state.current_job.layers.current, 156U) << log_label(phase);
    }
}

TEST(BootRecoveryTests, TheJobDisappearancePathIsUntouchedByTheBootRule) {
    // §7.8's rule — "missing job data does not immediately erase a recently active job" — is
    // the product path, and it is a different mechanism reached by a different event. A boot
    // is not a way for a printer to report an absent job, and an absent job is not a boot.
    //
    // Asserted here as well as in the job-lifecycle suite because both now clear the same
    // field, and a future simplification that merged them would break the §7.8 delay without
    // any test in that suite noticing the boot rule had become the cause.
    AppState state = prd::test::printing_device(at_ms(0));

    state = reduce(state, Event::job_unavailable(at_ms(1000), 2));
    EXPECT_TRUE(state.current_job.present) << "one absent report must not erase the job";
    EXPECT_TRUE(state.current_job.reported_missing);

    state = reduce(state, Event::confidence_aged(at_ms(120000)));
    EXPECT_FALSE(state.current_job.present) << "expiry, not the boot rule, is what clears it";
}

// --- The scenario sweep, which is where it bit --------------------------------

TEST(BootRecoveryTests, TheScenarioSweepRecoversAfterTheDeviceFaultScenario) {
    // The end-to-end regression, through the real store and the real reducer: run the
    // fault scenario to completion, then the next scenario, and require that the
    // presentation returns to the subject layer.
    //
    // Without the fix this fails on the last assertion with layer=device-fault, which is
    // exactly what the panel showed.
    using prd::fake_source::FakeSource;
    using prd::fake_source::Scenario;

    prd::app_store::Store store;
    FakeSource source{store, Scenario::RecoveryAndDeviceFault, at_ms(0)};

    while (!source.finished()) {
        ASSERT_TRUE(source.advance().applied);
    }

    ASSERT_NE(store.state().device_fault, DeviceFaultReason::None)
        << "the fault scenario must actually raise a fault, or this proves nothing";
    ASSERT_EQ(project(store.state()).layer, PresentationLayer::DeviceFault);

    source.load(Scenario::FullPrintCycle, at_ms(200000));
    while (!source.finished()) {
        ASSERT_TRUE(source.advance().applied);
    }

    EXPECT_EQ(store.state().device_fault, DeviceFaultReason::None)
        << "the next scenario's boot prelude must clear the fault";
    EXPECT_EQ(project(store.state()).layer, PresentationLayer::SubjectPresentation)
        << "a presentation must be reachable again after a fault scenario";
}

TEST(BootRecoveryTests, EveryScenarioReachesItsOwnLayerAfterAFault) {
    // Stronger than the pair above: after the fault, EVERY scenario must resolve to
    // something other than Device Fault. One that did not would pin the panel again.
    using prd::fake_source::FakeSource;
    using prd::fake_source::kScenarioCount;
    using prd::fake_source::Scenario;
    using prd::fake_source::scenario_at;

    prd::app_store::Store store;
    FakeSource source{store, Scenario::RecoveryAndDeviceFault, at_ms(0)};

    while (!source.finished()) {
        ASSERT_TRUE(source.advance().applied);
    }
    ASSERT_EQ(project(store.state()).layer, PresentationLayer::DeviceFault);

    for (std::size_t index = 0; index < kScenarioCount; ++index) {
        const auto scenario = scenario_at(index);
        if (scenario == Scenario::RecoveryAndDeviceFault) {
            continue;
        }

        source.load(scenario, at_ms(static_cast<std::int64_t>(300000 + (index * 100000))));
        while (!source.finished()) {
            ASSERT_TRUE(source.advance().applied);
        }

        EXPECT_NE(project(store.state()).layer, PresentationLayer::DeviceFault)
            << "scenario " << prd::fake_source::name(scenario) << " stayed in device fault";
    }
}
