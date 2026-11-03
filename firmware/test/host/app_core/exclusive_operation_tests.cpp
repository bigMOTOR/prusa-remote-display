// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "exclusive_operation.hpp"
#include "printer_session.hpp"
#include "provisioning_flow.hpp"

// The exclusive-operation barrier — who owns the product state, and what that forbids.
//
// ⛔ THE PATH THESE CLOSE. The application loop runs the factory-reset executor before
// printer_supervise(), and printer_supervise() returned as soon as a Validate session was armed —
// before it looked at factory_resetting. So a confirmed reset could erase the store while a
// validation worker was still live, and a queued success could then activate a candidate AFTER the
// erase, leaving the device carrying storage state instead of true first-boot state.
//
// ⬅️ WHY THE TESTS LIVE HERE. app_core.cpp is not host-compiled, so the decision was extracted into
// the unit production calls — three call sites (report ingress, worker launch, the activation
// boundary) all ask these functions rather than a copy of their conditions.

namespace prd::app_core {
namespace {

using app_model::DeviceFaultReason;
using app_model::NetworkPhase;
using app_model::ProvisioningPhase;
using exclusive::Conditions;
using exclusive::Owner;

// An ordinary, fully operational device: polling permitted, nothing exclusive.
[[nodiscard]] Conditions operational() {
    Conditions conditions;
    conditions.provisioning = ProvisioningPhase::Inactive;
    conditions.network = NetworkPhase::Connected;
    conditions.has_printer_configuration = true;
    return conditions;
}

TEST(ExclusiveOperationTests, AnOrdinaryDeviceIsOwnedByNothingAndMayDoEverything) {
    const Conditions conditions = operational();

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::None);
    EXPECT_FALSE(exclusive::active(conditions));
    EXPECT_TRUE(exclusive::printer_reports_permitted(conditions));
    EXPECT_TRUE(exclusive::candidate_activation_permitted(conditions));
    EXPECT_TRUE(exclusive::printer_worker_permitted(conditions));
}

TEST(ExclusiveOperationTests, A_CONFIRMED_FACTORY_RESET_OWNS_THE_DEVICE_AND_FORBIDS_EVERYTHING) {
    Conditions conditions = operational();
    conditions.factory_resetting = true;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::FactoryReset);
    EXPECT_TRUE(exclusive::active(conditions));

    // ⛔ ALL THREE, TOGETHER. A report cannot be acted on, a candidate cannot be activated, and no
    // worker can be launched — for the whole interval, not just during the erase call.
    EXPECT_FALSE(exclusive::printer_reports_permitted(conditions));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
}

TEST(ExclusiveOperationTests, THE_POST_ERASE_HOLD_BEFORE_RESTART_IS_STILL_EXCLUSIVE) {
    // ⬅️ THE TWO-SECOND PRESENTATION HOLD. §9's progress is on the panel and the erase is done; the
    // device is describing a configuration that no longer exists. Relaunching a worker or accepting a
    // report here would write over a store the user asked to be emptied.
    Conditions conditions = operational();
    conditions.factory_resetting = true;
    conditions.reset_restart_scheduled = true;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::FactoryReset);
    EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_reports_permitted(conditions));

    // ⛔ AND THE SCHEDULED RESTART IS AN INDEPENDENT WITNESS. Either term alone holds the device, so
    // the barrier does not rest on one flag remaining set for the whole interval.
    Conditions restart_only = operational();
    restart_only.reset_restart_scheduled = true;
    EXPECT_EQ(exclusive::owner_of(restart_only), Owner::FactoryReset);
    EXPECT_FALSE(exclusive::printer_worker_permitted(restart_only));
}

TEST(ExclusiveOperationTests, AnArmedConfirmationPromptDoesNotTakeTheDevice) {
    // ⬅️ DELIBERATE. An armed prompt is a question, not an operation: it may expire or be cancelled.
    // Blanking the panel's printer data because the user was ASKED something would present a working
    // device as disconnected.
    Conditions conditions = operational();
    conditions.factory_reset_confirmation_armed = true;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::None);
    EXPECT_TRUE(exclusive::printer_worker_permitted(conditions));
    EXPECT_TRUE(exclusive::printer_reports_permitted(conditions));
}

TEST(ExclusiveOperationTests, ARESET_FAILURE_RELEASES_THE_DEVICE_SO_IT_IS_NOT_STUCK) {
    // A failed erase ends the operation deliberately — the device must not be locked in an exclusive
    // state for ever — and the journal is what finishes the job on the next boot.
    Conditions conditions = operational();
    conditions.factory_resetting = false;
    conditions.reset_restart_scheduled = false;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::None);
    EXPECT_TRUE(exclusive::printer_worker_permitted(conditions));
}

TEST(ExclusiveOperationTests, TheOrdinaryEligibilityTermsStillApplyOnTheirOwn) {
    // The exclusivity term is additional, not a replacement: every pre-existing reason not to poll
    // still holds. One expression, one place — these used to be spelled out twice.
    {
        Conditions conditions = operational();
        conditions.provisioning = ProvisioningPhase::SetupActive;
        EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    }
    {
        Conditions conditions = operational();
        conditions.network = NetworkPhase::Reconnecting;
        EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    }
    {
        // ⛔ A PENDING CANDIDATE IS NOT AN ACTIVE CONFIGURATION. printer_configuration_pending is
        // deliberately not part of this condition.
        Conditions conditions = operational();
        conditions.has_printer_configuration = false;
        EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    }
}

// --- Provisioning, which the loop reaches FIRST ----------------------------
//
// ⛔ THE GAP THESE CLOSE. The loop is provisioning_tick() → local_interface_tick() →
// printer_supervise() → printer_ingest() → printer_launch(). provisioning_tick() had no exclusivity
// guard at all, and it runs before the erase and before the barrier's later re-assertion. Two paths
// were reachable: the reducer sets factory_resetting during the same iteration's event drain, so a
// queued submission was consumed on the confirming tick; and the server task can enqueue the instant
// after the barrier drains, which the next tick then processed.
//
// ⬅️ AND WHAT THESE PROVE, PRECISELY. They exercise the extracted decision that production calls —
// exclusive::provisioning_work_permitted() — at every call site's condition. app_core.cpp is not
// host-compiled, so they do NOT execute the binding: that this decision is consulted at the top of
// provisioning_tick(), before the queue is read, is verified by reading the production call site, and
// the negative control below discriminates the decision itself.

TEST(ProvisioningExclusivityTests, A_CONFIRMED_RESET_FORBIDS_ALL_PROVISIONING_WORK) {
    Conditions conditions = operational();
    conditions.factory_resetting = true;

    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));

    // The same answer for every stage of the interval, which is what "the complete interval" in
    // ADR-0008 decision 11 means.
    conditions.reset_restart_scheduled = true;
    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));

    Conditions hold_only = operational();
    hold_only.reset_restart_scheduled = true;
    EXPECT_FALSE(exclusive::provisioning_work_permitted(hold_only));
}

TEST(ProvisioningExclusivityTests, RESET_ALREADY_ACTIVE_WHEN_PROVISIONING_RUNS_REFUSES_EVERYTHING) {
    // ⛔ PATH ONE. The reducer sets factory_resetting during the event drain at the top of the
    // iteration, so by the time provisioning_tick() runs the state already says the reset owns the
    // device — which is exactly the condition the guard reads, and it is false before anything is
    // consumed.
    Conditions confirming_tick = operational();
    confirming_tick.factory_resetting = true;
    confirming_tick.provisioning = ProvisioningPhase::SetupActive;

    EXPECT_FALSE(exclusive::provisioning_work_permitted(confirming_tick));

    // And a transaction already validating does not change the answer: no dwell, no verdict, no
    // settlement, and therefore no false setup failure published over the reset's presentation.
    Conditions mid_validation = operational();
    mid_validation.factory_resetting = true;
    mid_validation.provisioning = ProvisioningPhase::ValidatingConfiguration;
    EXPECT_FALSE(exclusive::provisioning_work_permitted(mid_validation));
}

TEST(ProvisioningExclusivityTests, A_LATE_SUBMISSION_DURING_THE_HOLD_IS_STILL_REFUSED) {
    // ⛔ PATH TWO. The server task enqueues after the barrier drained. The queue may hold it; the
    // application task may not act on it. The guard's answer is the same on the hold tick as on the
    // erase tick, which is what makes "only until the next opportunity to wipe it" true.
    Conditions holding = operational();
    holding.factory_resetting = true;
    holding.reset_restart_scheduled = true;
    holding.provisioning = ProvisioningPhase::SetupActive;

    EXPECT_FALSE(exclusive::provisioning_work_permitted(holding));

    // Neither Wi-Fi nor printer-stage work is permitted, because the permission is asked before the
    // stage is even looked at.
    EXPECT_FALSE(exclusive::provisioning_work_permitted(holding));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(holding));
    EXPECT_FALSE(exclusive::printer_reports_permitted(holding));
}

TEST(ProvisioningExclusivityTests, NO_ACCESS_POINT_OR_RECONFIGURATION_REOPENS_DURING_THE_HOLD) {
    // Reconfiguration and a fresh setup session are provisioning work. While the reset owns the
    // device the permission is false for the whole interval, so nothing can reopen the surface the
    // barrier closed.
    for (const bool restart_scheduled : {false, true}) {
        Conditions conditions = operational();
        conditions.factory_resetting = true;
        conditions.reset_restart_scheduled = restart_scheduled;

        EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions)) << restart_scheduled;
        EXPECT_FALSE(exclusive::printer_worker_permitted(conditions)) << restart_scheduled;
    }
}

TEST(ProvisioningExclusivityTests, NORMAL_PROVISIONING_IS_UNAFFECTED_WHEN_NOTHING_IS_EXCLUSIVE) {
    // ⬅️ THE CONTROL THAT KEEPS THE GUARD HONEST. Setup must work on a device that is not connected,
    // has no printer configuration and is mid-setup — which is every first boot. A guard that also
    // refused those would have broken provisioning entirely rather than protecting a reset.
    Conditions first_boot;
    first_boot.provisioning = ProvisioningPhase::SetupActive;
    first_boot.network = NetworkPhase::Disabled;
    first_boot.has_printer_configuration = false;

    EXPECT_TRUE(exclusive::provisioning_work_permitted(first_boot));
    EXPECT_EQ(exclusive::owner_of(first_boot), Owner::None);

    // Including with a confirmation prompt on screen, which is a question rather than an operation.
    Conditions prompted = first_boot;
    prompted.factory_reset_confirmation_armed = true;
    EXPECT_TRUE(exclusive::provisioning_work_permitted(prompted));

    // And mid-validation on a connected station.
    Conditions validating = operational();
    validating.provisioning = ProvisioningPhase::ValidatingConfiguration;
    EXPECT_TRUE(exclusive::provisioning_work_permitted(validating));
}

TEST(ProvisioningExclusivityTests, ProvisioningAndPrinterPermissionsAreSeparateForAReason) {
    // ⛔ THEY MUST NOT BE THE SAME PREDICATE. The printer permission also carries the polling
    // eligibility, so sharing it would mean a device that is merely disconnected could not run setup
    // — the opposite of what setup is for.
    Conditions disconnected = operational();
    disconnected.network = NetworkPhase::Disabled;

    EXPECT_FALSE(exclusive::printer_worker_permitted(disconnected));
    EXPECT_TRUE(exclusive::provisioning_work_permitted(disconnected));

    // And an exclusive operation stops both.
    Conditions resetting = operational();
    resetting.factory_resetting = true;
    EXPECT_FALSE(exclusive::printer_worker_permitted(resetting));
    EXPECT_FALSE(exclusive::provisioning_work_permitted(resetting));
}

TEST(ExclusiveOperationTests, EveryOwnerHasALogLabel) {
    for (const Owner owner : {Owner::None, Owner::FactoryReset}) {
        EXPECT_FALSE(exclusive::log_label(owner).empty());
        EXPECT_NE(exclusive::log_label(owner), "unknown");
    }
}

// --- What the barrier's effect means for report ingress --------------------

namespace {

[[nodiscard]] prusalink::Report validate_success(const prusalink::Generation generation,
                                                 const prusalink::Transaction transaction) {
    prusalink::Report report;
    report.purpose = prusalink::Purpose::Validate;
    report.generation = generation;
    report.transaction = transaction;
    report.verdict = prusalink::Verdict::Succeeded;
    return report;
}

[[nodiscard]] prusalink::Report poll_cycle(const prusalink::Generation generation,
                                           const prusalink::Transaction transaction) {
    prusalink::Report report;
    report.purpose = prusalink::Purpose::Poll;
    report.generation = generation;
    report.transaction = transaction;
    report.connection_present = true;
    report.identity_present = true;
    report.status_present = true;
    report.status.printer.generation = generation;
    report.status.printer.sequence = 1;
    return report;
}

TEST(FirmwareUpdateExclusivityTests, AdmissionReadsRefreshedLiveFactsAndLatchesUntilReboot) {
    auto live = operational();
    exclusive::FirmwareUpdateAdmission admission;
    live.configuration_in_flight = true;
    EXPECT_FALSE(admission.authorize(live));
    live.configuration_in_flight = false;
    live.first_boot_pending = true;
    EXPECT_FALSE(admission.authorize(live));
    live.first_boot_pending = false;
    ASSERT_TRUE(admission.authorize(live));
    ASSERT_TRUE(admission.acquire());
    EXPECT_TRUE(admission.held());
    EXPECT_FALSE(admission.acquire());
    // Same application owner feeds its authoritative reservation into guards
    // synchronously, before any asynchronous OTA observation could be processed.
    live.firmware_update_active = admission.held();
    EXPECT_EQ(exclusive::owner_of(live), Owner::FirmwareUpdate);
    EXPECT_FALSE(exclusive::provisioning_work_permitted(live));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(live));
    EXPECT_FALSE(exclusive::printer_reports_permitted(live));
    EXPECT_FALSE(exclusive::printer_worker_permitted(live));
    EXPECT_TRUE(exclusive::station_work_permitted(live));
    admission.release(false);
    EXPECT_FALSE(admission.held());
    live.firmware_update_active = admission.held();
    ASSERT_TRUE(admission.authorize(live));
    ASSERT_TRUE(admission.acquire());
    admission.release(true);
    admission.release(false);
    EXPECT_TRUE(admission.held());
    EXPECT_TRUE(admission.activation_locked());
    EXPECT_FALSE(admission.acquire());
}

TEST(FirmwareUpdateExclusivityTests, EveryCompetingMutationOrPendingValidationRefusesAdmission) {
    for (unsigned scenario = 0; scenario < 9; ++scenario) {
        auto live = operational();
        switch (scenario) {
        case 0:
            live.factory_resetting = true;
            break;
        case 1:
            live.factory_reset_confirmation_armed = true;
            break;
        case 2:
            live.reset_restart_scheduled = true;
            break;
        case 3:
            live.factory_reset_failed = true;
            break;
        case 4:
            live.device_fault = DeviceFaultReason::WifiUnavailable;
            break;
        case 5:
            live.provisioning = ProvisioningPhase::SetupActive;
            break;
        case 6:
            live.configuration_in_flight = true;
            break;
        case 7:
            live.first_boot_pending = true;
            break;
        case 8:
            live.firmware_update_active = true;
            break;
        }
        exclusive::FirmwareUpdateAdmission admission;
        EXPECT_FALSE(admission.authorize(live)) << scenario;
        EXPECT_FALSE(admission.held());
    }
}

TEST(FirmwareUpdateExclusivityTests, AuthorizedSetupSurfaceMayHandOffOnlyWhenNoConfigurationRuns) {
    auto live = operational();
    live.provisioning = ProvisioningPhase::SetupActive;
    exclusive::FirmwareUpdateAdmission admission;
    EXPECT_FALSE(admission.authorize(live));

    live.firmware_update_authorized_surface = true;
    EXPECT_TRUE(admission.authorize(live));
    admission.release(false);

    live.configuration_in_flight = true;
    EXPECT_FALSE(admission.authorize(live));
}

TEST(FirmwareUpdateExclusivityTests, OnlyFirmwareUpdatePreservesStationDuringExclusiveWork) {
    auto live = operational();
    EXPECT_TRUE(exclusive::station_work_permitted(live));
    live.firmware_update_active = true;
    EXPECT_TRUE(exclusive::station_work_permitted(live));
    live.device_fault = DeviceFaultReason::StorageFailure;
    EXPECT_FALSE(exclusive::station_work_permitted(live));
    live.factory_resetting = true;
    EXPECT_FALSE(exclusive::station_work_permitted(live));
    EXPECT_EQ(exclusive::owner_of(live), Owner::FactoryReset);
}

} // namespace

TEST(ResetBarrierTests, A_QUEUED_VALIDATION_SUCCESS_CANNOT_ACTIVATE_ONCE_A_RESET_OWNS_THE_DEVICE) {
    // ⛔ THE EXACT DEFECT. The verdict was published BEFORE the reset, matches the armed session
    // perfectly, and would have called complete_printer_validation() — writing the recovery record
    // and both activation pointers after the erase.
    const prusalink::Transaction transaction{2, 1};
    const printer_session::Session session =
        printer_session::armed(prusalink::Purpose::Validate, 7, transaction);
    const prusalink::Report queued = validate_success(7, transaction);

    // Without the reset this is exactly the report that promotes a candidate.
    ASSERT_EQ(printer_session::classify(session, queued, false), printer_session::Ingress::Accept);

    // ⬅️ AND WITH IT, THE IDENTITY NO LONGER MATTERS. Exclusivity is checked before the session is
    // looked at, so a perfectly matching report is still refused.
    Conditions resetting = operational();
    resetting.factory_resetting = true;
    const bool exclusive_operation = !exclusive::printer_reports_permitted(resetting);

    EXPECT_EQ(printer_session::classify(session, queued, exclusive_operation),
              printer_session::Ingress::ExclusiveOperation);
    EXPECT_FALSE(exclusive::candidate_activation_permitted(resetting));
}

TEST(ResetBarrierTests, A_VALIDATION_SUCCESS_ARRIVING_AFTER_THE_RESET_STARTS_CANNOT_ACTIVATE) {
    // The other half: a report published DURING the erase, after the barrier's drain. The barrier
    // disarmed the session, so identity alone already refuses it — and exclusivity refuses it whatever
    // the session says, which is what still holds if anything ever re-armed one mid-reset.
    const prusalink::Transaction transaction{2, 1};
    const prusalink::Report late = validate_success(7, transaction);

    const printer_session::Session disarmed;
    EXPECT_EQ(printer_session::classify(disarmed, late, false),
              printer_session::Ingress::NoSession);

    Conditions resetting = operational();
    resetting.factory_resetting = true;
    EXPECT_EQ(printer_session::classify(
                  printer_session::armed(prusalink::Purpose::Validate, 7, transaction), late,
                  !exclusive::printer_reports_permitted(resetting)),
              printer_session::Ingress::ExclusiveOperation);
}

TEST(ResetBarrierTests, A_POLL_REPORT_AFTER_THE_RESET_STARTS_MUTATES_NOTHING) {
    // ⛔ NOT ONE OF THE THREE OBSERVATIONS, AND THEREFORE NOT observed_at EITHER. A rejected report is
    // never translated, so freshness cannot be refreshed on a device whose configuration is gone.
    const prusalink::Transaction transaction{4, 0};
    const printer_session::Session session =
        printer_session::armed(prusalink::Purpose::Poll, 9, transaction);
    const prusalink::Report cycle = poll_cycle(9, transaction);

    ASSERT_TRUE(cycle.connection_present);
    ASSERT_TRUE(cycle.identity_present);
    ASSERT_TRUE(cycle.status_present);
    ASSERT_EQ(printer_session::classify(session, cycle, false), printer_session::Ingress::Accept);

    Conditions resetting = operational();
    resetting.factory_resetting = true;

    EXPECT_EQ(
        printer_session::classify(session, cycle, !exclusive::printer_reports_permitted(resetting)),
        printer_session::Ingress::ExclusiveOperation);
}

TEST(ResetBarrierTests, NO_WORKER_CAN_RELAUNCH_ACROSS_THE_WHOLE_RESET_INTERVAL) {
    // Confirmed, erasing, and holding before the restart — every stage of the interval.
    Conditions erasing = operational();
    erasing.factory_resetting = true;
    EXPECT_FALSE(exclusive::printer_worker_permitted(erasing));

    Conditions holding = operational();
    holding.factory_resetting = true;
    holding.reset_restart_scheduled = true;
    EXPECT_FALSE(exclusive::printer_worker_permitted(holding));

    // POSITIVE CONTROL: the same device with no reset in progress does launch, so the refusals above
    // are the reset and not an eligibility rule that never permits anything.
    EXPECT_TRUE(exclusive::printer_worker_permitted(operational()));
}

TEST(ResetBarrierTests, THE_BARRIERS_RESULTING_STATE_FORGETS_THE_STAGED_PAIR_AND_ACCEPTS_NOTHING) {
    // What quiesce_for_exclusive_operation() leaves behind, as values: a default Validation and a
    // default Session. Both must be inert on every axis — no identity to match, no candidate to
    // activate, no deadline to fire and settle a transaction over the reset's own presentation.
    const printer_session::Validation forgotten;

    EXPECT_FALSE(forgotten.session.armed);
    EXPECT_FALSE(forgotten.staged.staged);
    EXPECT_EQ(forgotten.staged.profile, 0U);
    EXPECT_EQ(forgotten.staged.configuration_generation, config::kNoGeneration);
    EXPECT_EQ(forgotten.staged.secret_generation, secrets::kNoGeneration);
    EXPECT_EQ(forgotten.deadline_us, 0);

    EXPECT_FALSE(printer_session::expired(forgotten, 0));
    EXPECT_FALSE(printer_session::expired(forgotten, 999999999));

    // A staged pair that is forgotten cannot produce a transaction identity, so nothing can be armed
    // from it again.
    EXPECT_FALSE(printer_session::transaction_for(forgotten.staged, 1).valid());

    EXPECT_EQ(printer_session::classify(forgotten.session, validate_success(7, {2, 1}), false),
              printer_session::Ingress::NoSession);
    EXPECT_EQ(printer_session::classify(printer_session::Session{}, poll_cycle(9, {4, 0}), false),
              printer_session::Ingress::NoSession);
}

// --- Device faults that must not release the device — audit item 5 -----------
//
// ⛔ WHY A SECOND OWNER EXISTS AT ALL. A resume that fails AT BOOT sets neither of the terms above:
// AppState::factory_resetting comes only from a confirmed very-long press, and no restart is ever
// scheduled on a failure. So on the one path where the device most needed holding — a journal that
// may still be armed, and a store that has just refused — there was no term that could hold it.

TEST(FaultOwnershipTests, AN_INCOMPLETE_RESET_OWNS_THE_DEVICE_AND_FORBIDS_EVERYTHING) {
    Conditions conditions = operational();
    conditions.device_fault = DeviceFaultReason::FactoryResetIncomplete;

    EXPECT_TRUE(exclusive::fault_owns_device(conditions.device_fault));
    EXPECT_EQ(exclusive::owner_of(conditions), Owner::DeviceFault);
    EXPECT_TRUE(exclusive::active(conditions));

    EXPECT_FALSE(exclusive::printer_reports_permitted(conditions));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));
}

TEST(FaultOwnershipTests, A_BOOT_STORAGE_FAILURE_OWNS_THE_DEVICE_TOO) {
    // The journal could not be read or held a value this firmware did not write, so nothing about
    // the stored state is established. Nothing may be written on the strength of it.
    Conditions conditions = operational();
    conditions.device_fault = DeviceFaultReason::StorageFailure;

    EXPECT_TRUE(exclusive::fault_owns_device(conditions.device_fault));
    EXPECT_EQ(exclusive::owner_of(conditions), Owner::DeviceFault);

    EXPECT_FALSE(exclusive::printer_reports_permitted(conditions));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));
}

TEST(FaultOwnershipTests, WIFI_UNAVAILABLE_IS_NOT_AN_OWNER_AND_THE_ACCEPTED_3C_BEHAVIOUR_STANDS) {
    // ⛔ THE DISCRIMINATING NEGATIVE CONTROL. If this passed by treating "any device fault" as an
    // owner, the two tests above would prove nothing about which faults were chosen — and the 3c
    // ruling would be silently reversed: it keeps the setup access point and its server physically
    // alive under this fault so a browser mid-flow is not stranded, and an exclusive barrier would
    // refuse the very submissions that surface is up to accept.
    Conditions conditions = operational();
    conditions.device_fault = DeviceFaultReason::WifiUnavailable;

    EXPECT_FALSE(exclusive::fault_owns_device(conditions.device_fault));
    EXPECT_EQ(exclusive::owner_of(conditions), Owner::None);
    EXPECT_FALSE(exclusive::active(conditions));
    EXPECT_TRUE(exclusive::provisioning_work_permitted(conditions));
    EXPECT_TRUE(exclusive::printer_reports_permitted(conditions));
}

TEST(FaultOwnershipTests, NO_OTHER_DEVICE_FAULT_TAKES_THE_DEVICE_EITHER) {
    // Every remaining reason, so "which faults own the device" is a closed list rather than a
    // side effect of whichever ones happened to be tested.
    for (const DeviceFaultReason reason :
         {DeviceFaultReason::None, DeviceFaultReason::BoardMismatch,
          DeviceFaultReason::DisplayInitializationFailure, DeviceFaultReason::LowMemory,
          DeviceFaultReason::TaskWatchdog, DeviceFaultReason::OtaValidationFailure}) {
        Conditions conditions = operational();
        conditions.device_fault = reason;

        EXPECT_FALSE(exclusive::fault_owns_device(reason)) << "reason " << static_cast<int>(reason);
        EXPECT_EQ(exclusive::owner_of(conditions), Owner::None)
            << "reason " << static_cast<int>(reason);
    }
}

TEST(FaultOwnershipTests, THE_RESET_FAILURE_LATCH_HOLDS_THE_DEVICE_ON_ITS_OWN) {
    // ⛔ THE TERM NO LATER OBSERVATION CAN CLEAR — audit item 5's fifth finding. Even with the fault
    // reason replaced and no reset flag set, a device whose reset executed and failed is still held.
    Conditions conditions = operational();
    conditions.factory_resetting = false;
    conditions.reset_restart_scheduled = false;
    conditions.device_fault = DeviceFaultReason::WifiUnavailable;
    conditions.factory_reset_failed = true;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::FactoryReset);
    EXPECT_TRUE(exclusive::active(conditions));
    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_worker_permitted(conditions));
    EXPECT_FALSE(exclusive::printer_reports_permitted(conditions));
    EXPECT_FALSE(exclusive::candidate_activation_permitted(conditions));
}

TEST(FaultOwnershipTests, NEGATIVE_CONTROL_WITHOUT_THE_LATCH_THAT_STATE_RELEASES_THE_DEVICE) {
    // ⬅️ THE SAME CONDITIONS WITH THE LATCH DOWN. If this also held the device, the test above would
    // be proving nothing about the latch.
    Conditions conditions = operational();
    conditions.device_fault = DeviceFaultReason::WifiUnavailable;
    conditions.factory_reset_failed = false;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::None);
    EXPECT_TRUE(exclusive::provisioning_work_permitted(conditions));
}

TEST(FaultOwnershipTests, THE_BARRIER_AND_THE_REDUCER_SHARE_ONE_LIST) {
    // ⛔ ONE ANSWER, NOT TWO COPIES. The reducer's precedence rule and this barrier must agree about
    // which faults hold the device, or one of them drifts.
    for (const DeviceFaultReason reason :
         {DeviceFaultReason::None, DeviceFaultReason::StorageFailure,
          DeviceFaultReason::BoardMismatch, DeviceFaultReason::DisplayInitializationFailure,
          DeviceFaultReason::LowMemory, DeviceFaultReason::TaskWatchdog,
          DeviceFaultReason::OtaValidationFailure, DeviceFaultReason::WifiUnavailable,
          DeviceFaultReason::FactoryResetIncomplete}) {
        EXPECT_EQ(exclusive::fault_owns_device(reason), app_model::fault_holds_device(reason))
            << "reason " << static_cast<int>(reason);
    }
}

TEST(FaultOwnershipTests, A_CONFIRMED_RESET_STILL_REPORTS_ITSELF_AS_THE_OWNER) {
    // After a runtime failure BOTH terms are true: factory_resetting stays set, and the fault is
    // raised. The reset is the more specific owner and must remain the reported one, so the log and
    // any future policy keyed on Owner do not start describing the interval differently.
    Conditions conditions = operational();
    conditions.factory_resetting = true;
    conditions.device_fault = DeviceFaultReason::FactoryResetIncomplete;

    EXPECT_EQ(exclusive::owner_of(conditions), Owner::FactoryReset);
    EXPECT_TRUE(exclusive::active(conditions));
    EXPECT_FALSE(exclusive::provisioning_work_permitted(conditions));
}

} // namespace
} // namespace prd::app_core
