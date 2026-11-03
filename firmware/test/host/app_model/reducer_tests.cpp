// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <cstdint>

#include "app_model/presentation.hpp"
#include "app_model/reducer.hpp"
#include "support/state_builders.hpp"

// The reducer suite.
//
// Organised by the rule each group protects rather than by function, because the
// rules are what the accepted architecture states and the functions are an
// implementation detail. The groups that matter most are the ones where a plausible
// simplification would break the architecture: Attention modifying a subject rather
// than replacing it, connection state never being inferred from printer state, and
// stale data never becoming current.

using namespace prd::app_model;
using prd::test::at_ms;
using prd::test::booted_device;
using prd::test::printer_snapshot;
using prd::test::printing_device;
using prd::test::printing_job;
using prd::test::with_connection;
using prd::test::with_printer_state;

// --- Purity ----------------------------------------------------------------

TEST(ReducerPurityTests, DoesNotModifyTheInputState) {
    const AppState before = printing_device(at_ms(1000));
    const AppState copy = before;

    const AppState after = reduce(before, Event::confidence_aged(at_ms(2000)));

    EXPECT_EQ(before, copy) << "reduce() mutated the state it was given";
    EXPECT_NE(after.last_event_at, before.last_event_at);
}

TEST(ReducerPurityTests, SameInputsProduceIdenticalOutputs) {
    const AppState state = printing_device(at_ms(1000));
    const Event event =
        Event::printer_snapshot(at_ms(3000), printer_snapshot(PrinterState::Paused, 2));

    const AppState first = reduce(state, event);
    const AppState second = reduce(state, event);

    EXPECT_EQ(first, second);
}

TEST(ReducerPurityTests, ReplayIsDeterministic) {
    // The property fixtures depend on: the same event sequence from the same start
    // must produce the same state, down to the revision counter, on every run and
    // in any test order.
    const auto replay = []() {
        AppState state;
        state = booted_device(at_ms(0));
        state = with_connection(state, ConnectionPhase::Connected, at_ms(100));
        state = with_printer_state(state, PrinterState::Preparing, at_ms(200), 1);
        state = with_printer_state(state, PrinterState::Printing, at_ms(400), 2);
        state = reduce(state, Event::job_observed(at_ms(500), printing_job(1)));
        state = reduce(state, Event::confidence_aged(at_ms(6000)));
        state = with_printer_state(state, PrinterState::Paused, at_ms(7000), 3);
        state = with_printer_state(state, PrinterState::Finished, at_ms(9000), 4);
        return state;
    };

    EXPECT_EQ(replay(), replay());
}

TEST(ReducerPurityTests, NoneEventChangesNothingButTime) {
    const AppState before = printing_device(at_ms(1000));

    Event nothing;
    nothing.observed_at = at_ms(1500);

    const AppState after = reduce(before, nothing);

    EXPECT_TRUE(content_equals(before, after));
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.diagnostics.events_applied, before.diagnostics.events_applied);
}

// --- The reset latch is CONTENT, not bookkeeping — audit item 5 --------------

TEST(ReducerPurityTests, TWO_STATES_DIFFERING_ONLY_IN_THE_RESET_LATCH_ARE_NOT_EQUAL) {
    // ⛔ WHY THIS IS A CONTENT COMPARISON AND NOT AN OPTIMISATION QUESTION. factory_reset_failed
    // decides whether the destructive executor may run again and whether the reset still owns the
    // device. A store that treated these two as the same state would report that nothing had
    // happened at the exact moment a reset became terminal — and would not re-project, so the panel
    // would keep drawing whatever it drew before.
    AppState healthy = printing_device(at_ms(1000));
    healthy.factory_resetting = true;
    healthy.device_fault = DeviceFaultReason::FactoryResetIncomplete;
    healthy.factory_reset_failed = false;

    AppState latched = healthy;
    latched.factory_reset_failed = true;

    // Everything else really is identical, so the assertion below is about the latch alone.
    ASSERT_EQ(healthy.factory_resetting, latched.factory_resetting);
    ASSERT_EQ(healthy.device_fault, latched.device_fault);
    ASSERT_EQ(healthy.device_operation, latched.device_operation);
    ASSERT_EQ(healthy.modal_surface, latched.modal_surface);
    ASSERT_EQ(healthy.recovery_mode, latched.recovery_mode);

    EXPECT_FALSE(content_equals(healthy, latched))
        << "the equality contract silently ignores the reset latch";
    EXPECT_FALSE(content_equals(latched, healthy)) << "and it must be symmetric";
    EXPECT_NE(healthy, latched);

    // POSITIVE CONTROL: identical states really do compare equal, so the assertions above are not
    // passing because content_equals() rejects everything.
    EXPECT_TRUE(content_equals(healthy, healthy));
    EXPECT_TRUE(content_equals(latched, latched));
}

TEST(ReducerPurityTests, EVERY_DEVICE_OWNED_FLOW_FIELD_PARTICIPATES_IN_CONTENT_EQUALITY) {
    // ⛔ THE NEIGHBOURING FIELDS, PINNED TOO — found by a negative control while adding the latch.
    // Removing `factory_resetting` from content_equals() broke NO test in the whole suite, so the
    // comparison was correct but unprotected. Each field below decides something the panel or the
    // executor acts on; a store that treated a change in one as "nothing happened" would withhold
    // the revision, the notification and the re-projection.
    const AppState base = printing_device(at_ms(1000));

    {
        AppState changed = base;
        changed.factory_resetting = !base.factory_resetting;
        EXPECT_FALSE(content_equals(base, changed)) << "factory_resetting";
    }
    {
        AppState changed = base;
        changed.factory_reset_failed = !base.factory_reset_failed;
        EXPECT_FALSE(content_equals(base, changed)) << "factory_reset_failed";
    }
    {
        AppState changed = base;
        changed.recovery_mode = !base.recovery_mode;
        EXPECT_FALSE(content_equals(base, changed)) << "recovery_mode";
    }
    {
        AppState changed = base;
        changed.device_fault = DeviceFaultReason::StorageFailure;
        ASSERT_NE(changed.device_fault, base.device_fault);
        EXPECT_FALSE(content_equals(base, changed)) << "device_fault";
    }
    {
        AppState changed = base;
        changed.modal_surface = ModalSurface::FactoryResetConfirmation;
        ASSERT_NE(changed.modal_surface, base.modal_surface);
        EXPECT_FALSE(content_equals(base, changed)) << "modal_surface";
    }
    {
        AppState changed = base;
        changed.device_operation = DeviceOperation::RecoveryMode;
        ASSERT_NE(changed.device_operation, base.device_operation);
        EXPECT_FALSE(content_equals(base, changed)) << "device_operation";
    }
    {
        AppState changed = base;
        changed.provisioning = ProvisioningPhase::SetupRequired;
        ASSERT_NE(changed.provisioning, base.provisioning);
        EXPECT_FALSE(content_equals(base, changed)) << "provisioning";
    }

    // POSITIVE CONTROL: an untouched copy still compares equal.
    EXPECT_TRUE(content_equals(base, base));
}

TEST(ReducerPurityTests, THE_CONFIRMATION_MOMENT_STAYS_OUT_OF_CONTENT_EQUALITY) {
    // ⬅️ THE OTHER HALF, UNCHANGED. factory_reset_armed_at is the confirmation window's internal
    // timing — the transition a viewer must see is modal_surface, which IS compared. Including the
    // moment would bump the revision and re-project the whole tree for a value nothing draws.
    //
    // ⛔ IT IS PINNED HERE SO THE TWO EXCLUSIONS CANNOT BE CONFUSED. One is timing bookkeeping; the
    // other changes what the device is allowed to do.
    AppState first = printing_device(at_ms(1000));
    first.modal_surface = ModalSurface::FactoryResetConfirmation;
    first.factory_reset_armed_at = at_ms(2000);

    AppState second = first;
    second.factory_reset_armed_at = at_ms(9000);

    EXPECT_TRUE(content_equals(first, second));
}

// --- Every subject state ---------------------------------------------------

TEST(SubjectStateTests, AcquiringUntilTheFirstObservation) {
    const AppState state = booted_device(at_ms(0));

    // §6: AcquiringState is the acquisition of the *first* trustworthy state, and
    // nothing has been observed, so there is nothing to be confident about.
    EXPECT_EQ(state.subject, SubjectState::AcquiringState);
    EXPECT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_FALSE(state.printer.ever_observed);
}

TEST(SubjectStateTests, MapsEveryPrinterStateThatResolvesASubject) {
    struct Case {
        PrinterState reported;
        SubjectState expected;
    };

    const Case cases[] = {
        {PrinterState::Idle, SubjectState::Ready},
        {PrinterState::Ready, SubjectState::Ready},
        {PrinterState::Preparing, SubjectState::Preparing},
        {PrinterState::Printing, SubjectState::Printing},
        {PrinterState::Paused, SubjectState::Paused},
        {PrinterState::Finished, SubjectState::Completed},
        {PrinterState::Stopped, SubjectState::Stopped},
    };

    std::uint32_t sequence = 0;
    for (const auto& scenario : cases) {
        ++sequence;

        const AppState state =
            with_printer_state(booted_device(at_ms(0)), scenario.reported, at_ms(0), sequence);

        EXPECT_EQ(state.subject, scenario.expected)
            << "printer state " << log_label(scenario.reported);
        EXPECT_EQ(state.confidence, Confidence::Current);
    }
}

TEST(SubjectStateTests, EveryArchitecturalSubjectStateIsReachable) {
    // UIAD 001 §6 lists seven. All seven must be producible from printer facts, or
    // one of them is a state the product can never enter.
    AppState state = booted_device(at_ms(0));
    EXPECT_EQ(state.subject, SubjectState::AcquiringState);

    state = with_printer_state(state, PrinterState::Ready, at_ms(100), 1);
    EXPECT_EQ(state.subject, SubjectState::Ready);

    state = with_printer_state(state, PrinterState::Preparing, at_ms(200), 2);
    EXPECT_EQ(state.subject, SubjectState::Preparing);

    state = with_printer_state(state, PrinterState::Printing, at_ms(300), 3);
    EXPECT_EQ(state.subject, SubjectState::Printing);

    state = with_printer_state(state, PrinterState::Paused, at_ms(400), 4);
    EXPECT_EQ(state.subject, SubjectState::Paused);

    state = with_printer_state(state, PrinterState::Finished, at_ms(500), 5);
    EXPECT_EQ(state.subject, SubjectState::Completed);

    state = with_printer_state(state, PrinterState::Stopped, at_ms(600), 6);
    EXPECT_EQ(state.subject, SubjectState::Stopped);
}

TEST(SubjectStateTests, UnknownDoesNotRegressAResolvedSubject) {
    // §6: AcquiringState is "not used for routine retries while a valid state
    // remains representable". A response the normalizer could not map must not send
    // a printing machine back to acquiring.
    AppState state =
        with_printer_state(booted_device(at_ms(0)), PrinterState::Printing, at_ms(100), 1);

    PrinterSnapshotObservation unmapped;
    unmapped.sequence = 2;
    unmapped.normalized = PrinterState::Unknown;
    unmapped.raw.assign("SOME_FUTURE_STATE");

    state = reduce(state, Event::printer_snapshot(at_ms(200), unmapped));

    EXPECT_EQ(state.subject, SubjectState::Printing);

    // §24.8: the unrecognised value is preserved for diagnostics rather than lost.
    EXPECT_EQ(state.printer.raw.view(), "SOME_FUTURE_STATE");
    EXPECT_EQ(state.diagnostics.unknown_printer_states_seen, 1U);
}

// --- Conditions modify, never replace --------------------------------------

TEST(SubjectConditionTests, AttentionModifiesTheSubjectRatherThanReplacingIt) {
    AppState state = printing_device(at_ms(0));
    ASSERT_EQ(state.subject, SubjectState::Printing);

    state = with_printer_state(state, PrinterState::Attention, at_ms(2000), 2);

    // UIAD 001 §7: "They never replace Ready, Preparing, Printing or Paused."
    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.subject_condition, SubjectCondition::Attention);
    EXPECT_EQ(state.confidence, Confidence::Current);
}

TEST(SubjectConditionTests, ErrorModifiesTheSubjectRatherThanReplacingIt) {
    AppState state = printing_device(at_ms(0));

    state = with_printer_state(state, PrinterState::Error, at_ms(2000), 2);

    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.subject_condition, SubjectCondition::Error);
}

TEST(SubjectConditionTests, ConditionClearsWhenThePrinterStopsReportingIt) {
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Attention, at_ms(2000), 2);
    ASSERT_EQ(state.subject_condition, SubjectCondition::Attention);

    state = with_printer_state(state, PrinterState::Printing, at_ms(4000), 3);

    EXPECT_EQ(state.subject_condition, SubjectCondition::None);
    EXPECT_EQ(state.subject, SubjectState::Printing);
}

TEST(SubjectConditionTests, RequiresAttentionFlagAloneRaisesTheCondition) {
    AppState state = printing_device(at_ms(0));

    PrinterSnapshotObservation observation = printer_snapshot(PrinterState::Printing, 2);
    observation.requires_attention = true;

    state = reduce(state, Event::printer_snapshot(at_ms(2000), observation));

    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.subject_condition, SubjectCondition::Attention);
}

TEST(SubjectConditionTests, AlertSeverityRaisesAndOutranks) {
    AppState state = printing_device(at_ms(0));

    AlertObservation warning;
    warning.severity = AlertSeverity::Warning;
    warning.source = AlertSource::StatusPrinter;
    warning.title.assign("Filament runout");
    warning.message.assign("Insert filament to continue");

    state = reduce(state, Event::alert_raised(at_ms(1000), warning));
    EXPECT_EQ(state.subject_condition, SubjectCondition::Attention);
    EXPECT_EQ(state.subject, SubjectState::Printing);

    AlertObservation failure = warning;
    failure.severity = AlertSeverity::Error;

    state = reduce(state, Event::alert_raised(at_ms(2000), failure));

    // §7.10's severity order: an error must not be understated as a warning.
    EXPECT_EQ(state.subject_condition, SubjectCondition::Error);

    state = reduce(state, Event::alert_cleared(at_ms(3000)));
    EXPECT_EQ(state.subject_condition, SubjectCondition::None);
    EXPECT_FALSE(state.alert.active);
}

TEST(SubjectConditionTests, AlertIsNotActionableUnlessSaidToBe) {
    // §7.10 and §24.18: the product does not invent printer instructions.
    AppState state = printing_device(at_ms(0));

    AlertObservation alert;
    alert.severity = AlertSeverity::Error;
    alert.title.assign("Printer error");

    state = reduce(state, Event::alert_raised(at_ms(1000), alert));

    EXPECT_TRUE(state.alert.active);
    EXPECT_FALSE(state.alert.is_actionable);
}

// --- Confidence ------------------------------------------------------------

TEST(ConfidenceTests, ProgressesCurrentThenDegradedThenUnavailable) {
    AppState state = printing_device(at_ms(0));
    EXPECT_EQ(state.confidence, Confidence::Current);
    EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Fresh);

    // Past the fresh window and into aging — and confidence is STILL Current.
    //
    // UI Behaviour Decision 001 §4: "Routine polling, retries and small telemetry
    // changes shall not become visible." At the 2 s active interval this point is
    // one to two missed polls after the last success, and §11.5 calls the level
    // "recent enough to retain visible state". Degrading here made a routine retry
    // repaint the panel.
    state = reduce(state, Event::confidence_aged(at_ms(5000)));
    EXPECT_EQ(state.confidence, Confidence::Current);
    EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Aging);

    // Stale is still Degraded: §11.5 keeps stale values presentable behind an
    // explicit indicator, which is what §7's Degraded means.
    state = reduce(state, Event::confidence_aged(at_ms(30000)));
    EXPECT_EQ(state.confidence, Confidence::Degraded);
    EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Stale);

    state = reduce(state, Event::confidence_aged(at_ms(120000)));
    EXPECT_EQ(state.confidence, Confidence::Unavailable);
    EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Expired);
}

TEST(ConfidenceTests, AgingIsCurrentAcrossItsWholeWindow) {
    // Not just at one sampled instant. The whole point of the level is that nothing
    // becomes visible anywhere inside it, so both edges are pinned: the first tick
    // past `fresh_within`, and the last tick still inside `aging_within`.
    const FreshnessPolicy policy;

    for (const auto age :
         {policy.fresh_within + std::chrono::milliseconds{1},
          policy.aging_within - std::chrono::milliseconds{1}, policy.aging_within}) {
        AppState state = printing_device(at_ms(0));
        state = reduce(state, Event::confidence_aged(at_ms(age.count())));

        EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Aging) << age.count() << " ms";
        EXPECT_EQ(state.confidence, Confidence::Current) << age.count() << " ms";
    }
}

TEST(ConfidenceTests, TheStaleBoundaryIsWhereConfidenceFalls) {
    // The level below aging is unchanged and still Degraded — §11.5 keeps stale
    // values presentable behind an explicit indicator. Pinned one millisecond either
    // side of the boundary so a future threshold change cannot move the transition
    // without this failing.
    const FreshnessPolicy policy;

    AppState still_current = printing_device(at_ms(0));
    still_current =
        reduce(still_current, Event::confidence_aged(at_ms(policy.aging_within.count())));
    EXPECT_EQ(still_current.confidence, Confidence::Current);

    AppState degraded = printing_device(at_ms(0));
    degraded = reduce(degraded, Event::confidence_aged(at_ms(policy.aging_within.count() + 1)));
    EXPECT_EQ(degraded.printer_connection.freshness, DataFreshness::Stale);
    EXPECT_EQ(degraded.confidence, Confidence::Degraded);
}

TEST(ConfidenceTests, ExpiryStillReachesUnavailable) {
    // Guards the far end: the correction moved one level and must not have shifted
    // the other boundary. §16.3's "after expiry" branch depends on this.
    const FreshnessPolicy policy;

    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at_ms(policy.stale_within.count() + 1)));

    EXPECT_EQ(state.printer_connection.freshness, DataFreshness::Expired);
    EXPECT_EQ(state.confidence, Confidence::Unavailable);
}

TEST(ConfidenceTests, RecoversWhenAFreshObservationArrives) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at_ms(120000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);

    state = with_printer_state(state, PrinterState::Printing, at_ms(121000), 2);

    EXPECT_EQ(state.confidence, Confidence::Current);
    EXPECT_EQ(state.subject, SubjectState::Printing);
}

TEST(ConfidenceTests, SubjectSurvivesLossOfConfidence) {
    // §7: Unavailable means the state may not be *presented* as current. It does
    // not mean the last known state is forgotten — §16.3 requires retaining it.
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::confidence_aged(at_ms(120000)));

    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.confidence, Confidence::Unavailable);
}

TEST(ConfidenceTests, ExpiryRemovesLiveCountersButKeepsLastKnownTelemetry) {
    AppState state = printing_device(at_ms(0));
    ASSERT_TRUE(state.current_job.remaining.known);
    ASSERT_TRUE(state.current_job.elapsed.known);

    state = reduce(state, Event::confidence_aged(at_ms(120000)));

    // §11.5: "expired: sensitive dynamic values such as remaining time are
    // removed". Both counters mean "as of now" and cannot be shown later.
    EXPECT_FALSE(state.current_job.remaining.known);
    EXPECT_FALSE(state.current_job.elapsed.known);

    // §16.3 and Production UI §9 Print Stopped keep the last known context.
    EXPECT_TRUE(state.current_job.progress.known);
    EXPECT_TRUE(state.current_job.layers.known);
    EXPECT_TRUE(state.current_job.present);
}

TEST(ConfidenceTests, StaleZeroRemainingTimeNeverBecomesCompletion) {
    // §16.3, stated as a prohibition: "do not convert stale zero remaining time
    // into finished". Nothing in the reducer reads a duration to decide a state.
    AppState state = printing_device(at_ms(0));

    JobObservation finishing = printing_job(2);
    finishing.remaining = DurationValue{true, 0};
    finishing.progress = ProgressValue{true, 100};

    state = reduce(state, Event::job_observed(at_ms(1000), finishing));
    state = reduce(state, Event::confidence_aged(at_ms(120000)));

    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_FALSE(state.last_completed_job.present);
}

// --- Connection is never inferred from printer state -----------------------

TEST(ConnectionTests, PrinterSnapshotDoesNotTouchConnectionPhase) {
    // §24.5 and §7.5's closing rule. A snapshot arriving is evidence that a request
    // succeeded, but only the connection reports connection state.
    AppState state = booted_device(at_ms(0));
    ASSERT_EQ(state.printer_connection.phase, ConnectionPhase::Unconfigured);

    state = with_printer_state(state, PrinterState::Printing, at_ms(100), 1);

    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Unconfigured);
    EXPECT_EQ(state.printer_connection.last_success_at, Timestamp::zero());
}

TEST(ConnectionTests, ConnectionFailureDoesNotChangeTheSubject) {
    AppState state = printing_device(at_ms(0));

    state = with_connection(state, ConnectionPhase::PrinterUnavailable, at_ms(1000));

    // Printer unavailability is confidence, not a subject state. The subject stays
    // and ages out on its own schedule.
    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.printer_connection.consecutive_failures, 1U);
}

TEST(ConnectionTests, TransientFailuresCountAndSuccessResets) {
    AppState state = booted_device(at_ms(0));

    state = with_connection(state, ConnectionPhase::PrinterUnavailable, at_ms(100));
    state = with_connection(state, ConnectionPhase::InvalidResponse, at_ms(200));
    state = with_connection(state, ConnectionPhase::Degraded, at_ms(300));
    EXPECT_EQ(state.printer_connection.consecutive_failures, 3U);

    state = with_connection(state, ConnectionPhase::Connected, at_ms(400));
    EXPECT_EQ(state.printer_connection.consecutive_failures, 0U);
    EXPECT_EQ(state.printer_connection.last_success_at, at_ms(400));
}

TEST(ConnectionTests, NonTransientFailuresDoNotFeedBackoff) {
    // §16.2: authentication and unsupported-API errors "are not transient network
    // failures and must not use aggressive retries", so they do not drive backoff.
    AppState state = booted_device(at_ms(0));

    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(100));
    state = with_connection(state, ConnectionPhase::UnsupportedApi, at_ms(200));

    EXPECT_EQ(state.printer_connection.consecutive_failures, 0U);
}

// --- Access conditions -----------------------------------------------------

TEST(AccessConditionTests, UnauthorizedRaisesAuthenticationRequired) {
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::Unauthorized, at_ms(100));

    EXPECT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);
}

TEST(AccessConditionTests, UnsupportedApiRaisesUnsupportedInterface) {
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::UnsupportedApi, at_ms(100));

    EXPECT_EQ(state.access_condition, AccessCondition::UnsupportedInterface);
}

TEST(AccessConditionTests, PersistsThroughRetriesBecauseItIsNotTransient) {
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::Unauthorized, at_ms(100));

    // §8: it is superseded only by a positive reclassification. A retry that lands
    // in a transport phase must not silently clear it.
    state = with_connection(state, ConnectionPhase::Resolving, at_ms(200));
    EXPECT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);

    state = with_connection(state, ConnectionPhase::Probing, at_ms(300));
    EXPECT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);
}

TEST(AccessConditionTests, ClearedByASuccessfulConnection) {
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::Unauthorized, at_ms(100));

    state = with_connection(state, ConnectionPhase::Connected, at_ms(200));

    EXPECT_EQ(state.access_condition, AccessCondition::None);
}

TEST(AccessConditionTests, ClearedByAChangeOfPrinterAddress) {
    // §16.2: retry "only after configuration change". A different printer has not
    // rejected anyone yet.
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::Unauthorized, at_ms(100));
    ASSERT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);

    ConfigurationObservation configuration;
    configuration.status = ConfigurationStatus::Valid;
    configuration.has_wifi_credentials = true;
    configuration.has_printer_configuration = true;
    configuration.printer_address.assign("192.168.1.50");
    configuration.auth_method = AuthMethod::ApiKey;

    state = reduce(state, Event::configuration_observed(at_ms(200), configuration));

    EXPECT_EQ(state.access_condition, AccessCondition::None);

    // The stale classification's input is gone too. Leaving Unauthorized in the
    // connection phase would let the derivation immediately re-raise what was just
    // cleared.
    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Unconfigured);
    EXPECT_TRUE(state.printer_connection.endpoint.empty());
}

TEST(AccessConditionTests, ClearedByANewCredentialAtTheSameAddress) {
    // The case that matters most and is easiest to miss: the user corrected their
    // API key. Nothing about the address, the auth method or the flags changed, so
    // §13.3's credential generation is the only observable difference.
    AppState state =
        with_connection(booted_device(at_ms(0)), ConnectionPhase::Unauthorized, at_ms(100));
    ASSERT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);

    ConfigurationObservation reissued;
    reissued.status = ConfigurationStatus::Valid;
    reissued.schema_version = 1;
    reissued.has_wifi_credentials = true;
    reissued.has_printer_configuration = true;
    reissued.printer_address.assign("printer.local");
    reissued.auth_method = AuthMethod::ApiKey;
    reissued.credential_generation = 1;

    state = reduce(state, Event::configuration_observed(at_ms(200), reissued));

    EXPECT_EQ(state.access_condition, AccessCondition::None);
}

TEST(AccessConditionTests, AnUnchangedConfigurationReportDoesNotClearIt) {
    // A configuration is re-reported at every boot and after unrelated settings
    // change. §16.2 permits a retry only after a *change*, so an identical report
    // must not become a licence to hammer an endpoint that has already refused us.
    AppState state = booted_device(at_ms(0));
    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(100));

    ConfigurationObservation unchanged;
    unchanged.status = ConfigurationStatus::Valid;
    unchanged.schema_version = 1;
    unchanged.has_wifi_credentials = true;
    unchanged.has_printer_configuration = true;
    unchanged.printer_address.assign("printer.local");
    unchanged.auth_method = AuthMethod::ApiKey;

    state = reduce(state, Event::configuration_observed(at_ms(200), unchanged));

    EXPECT_EQ(state.access_condition, AccessCondition::AuthenticationRequired);
    EXPECT_EQ(state.printer_connection.phase, ConnectionPhase::Unauthorized);
}

// --- Boot, setup and acquisition progression -------------------------------

TEST(BootProgressionTests, WalksThePhasesAndResolves) {
    AppState state;
    EXPECT_EQ(state.boot.phase, BootPhase::Starting);

    const BootPhase phases[] = {
        BootPhase::HardwareInitializing,
        BootPhase::StorageInitializing,
        BootPhase::LoadingConfiguration,
        BootPhase::StartingServices,
        BootPhase::Ready,
    };

    std::int64_t moment = 100;
    for (const auto phase : phases) {
        state = reduce(state, Event::boot_phase(at_ms(moment), phase));
        EXPECT_EQ(state.boot.phase, phase);
        moment += 100;
    }

    EXPECT_EQ(state.device_operation, DeviceOperation::None);
    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
}

TEST(BootProgressionTests, HardwareProbeRecordsResultsAndFaultsOnMismatch) {
    // §12.4: a mismatch "should generate a clear diagnostic and, when possible, a
    // visible unsupported-hardware screen", and §16.1 classifies it as a device
    // error.
    HardwareProbeObservation probe;
    probe.board_target.assign("waveshare_esp32_s3_lcd_2_8");
    probe.reset_reason.assign("power-on");
    probe.flash_validation = ValidationState::Validated;
    probe.psram_validation = ValidationState::Failed;

    const AppState state = reduce(AppState{}, Event::hardware_probed(at_ms(0), probe));

    EXPECT_EQ(state.boot.board_target.view(), "waveshare_esp32_s3_lcd_2_8");
    EXPECT_EQ(state.boot.psram_validation, ValidationState::Failed);
    EXPECT_EQ(state.device_fault, DeviceFaultReason::BoardMismatch);
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault);
}

TEST(BootProgressionTests, UnvalidatedHardwareIsNotAFault) {
    // Unknown is "unvalidated", not "failed". The USB console reports exactly this
    // and it must not read as broken hardware.
    HardwareProbeObservation probe;
    probe.flash_validation = ValidationState::Validated;
    probe.psram_validation = ValidationState::Unknown;

    const AppState state = reduce(AppState{}, Event::hardware_probed(at_ms(0), probe));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
}

TEST(SetupProgressionTests, MissingConfigurationRequiresSetup) {
    // §14.1's entry condition: "no valid configuration exists".
    ConfigurationObservation configuration;
    configuration.status = ConfigurationStatus::Missing;

    const AppState state =
        reduce(AppState{}, Event::configuration_observed(at_ms(0), configuration));

    EXPECT_EQ(state.provisioning, ProvisioningPhase::SetupRequired);
    EXPECT_FALSE(state.configuration.has_printer_configuration);
}

TEST(SetupProgressionTests, WalksTheProvisioningFlowAndReturnsToInactive) {
    AppState state;

    ConfigurationObservation missing;
    missing.status = ConfigurationStatus::Missing;
    state = reduce(state, Event::configuration_observed(at_ms(0), missing));
    ASSERT_EQ(state.provisioning, ProvisioningPhase::SetupRequired);

    state = reduce(state, Event::provisioning_phase(at_ms(100), ProvisioningPhase::SetupActive));
    EXPECT_EQ(state.provisioning, ProvisioningPhase::SetupActive);

    state = reduce(
        state, Event::provisioning_phase(at_ms(200), ProvisioningPhase::ValidatingConfiguration));
    EXPECT_EQ(state.provisioning, ProvisioningPhase::ValidatingConfiguration);

    state = reduce(state, Event::provisioning_phase(at_ms(300), ProvisioningPhase::SetupFailed));
    EXPECT_EQ(state.provisioning, ProvisioningPhase::SetupFailed);

    // §5: successful validation returns the product to normal state acquisition.
    ConfigurationObservation valid;
    valid.status = ConfigurationStatus::Valid;
    valid.has_printer_configuration = true;
    state = reduce(state, Event::configuration_observed(at_ms(400), valid));

    EXPECT_EQ(state.provisioning, ProvisioningPhase::Inactive);
}

TEST(SetupProgressionTests, ConnectionFailureNeverEntersProvisioning) {
    // UIAD 001 §5: "Provisioning is distinct from temporary connectivity failure and
    // shall not replace an existing valid configuration". Structural here — no
    // connection event can reach the provisioning field.
    AppState state = booted_device(at_ms(0));
    ASSERT_EQ(state.provisioning, ProvisioningPhase::Inactive);

    state = with_connection(state, ConnectionPhase::PrinterUnavailable, at_ms(100));
    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(200));
    state = with_connection(state, ConnectionPhase::InvalidResponse, at_ms(300));

    EXPECT_EQ(state.provisioning, ProvisioningPhase::Inactive);
    EXPECT_EQ(state.configuration.status, ConfigurationStatus::Valid);
}

TEST(SetupProgressionTests, AmbiguousConfigurationStatusLeavesProvisioningAlone) {
    // §13.4 makes entering provisioning after a migration or storage failure a
    // decision with more inputs than this event carries, so it belongs to the
    // orchestrator rather than being guessed here.
    AppState state = booted_device(at_ms(0));

    ConfigurationObservation migration;
    migration.status = ConfigurationStatus::MigrationRequired;
    state = reduce(state, Event::configuration_observed(at_ms(100), migration));

    EXPECT_EQ(state.provisioning, ProvisioningPhase::Inactive);
    EXPECT_EQ(state.configuration.status, ConfigurationStatus::MigrationRequired);
}

// --- Job lifecycle ---------------------------------------------------------

TEST(JobLifecycleTests, PrintingThenPausedThenCompleted) {
    AppState state = printing_device(at_ms(0));
    ASSERT_EQ(state.subject, SubjectState::Printing);

    state = with_printer_state(state, PrinterState::Paused, at_ms(2000), 2);
    EXPECT_EQ(state.subject, SubjectState::Paused);

    // Production UI §9 Paused: "Progress value remains unchanged."
    EXPECT_EQ(state.current_job.progress.percent, 62U);

    state = with_printer_state(state, PrinterState::Printing, at_ms(4000), 3);
    EXPECT_EQ(state.subject, SubjectState::Printing);

    state = with_printer_state(state, PrinterState::Finished, at_ms(6000), 4);
    EXPECT_EQ(state.subject, SubjectState::Completed);

    // §8.1: "persist completion snapshot when printing → finished".
    ASSERT_TRUE(state.last_completed_job.present);
    EXPECT_EQ(state.last_completed_job.id.view(), "job-1");
    EXPECT_EQ(state.last_completed_job.terminal_state, PrinterState::Finished);
    EXPECT_EQ(state.last_completed_job.completed_at, at_ms(6000));
}

TEST(JobLifecycleTests, PrintingThenStoppedPersistsTheTerminalContext) {
    AppState state = printing_device(at_ms(0));

    state = with_printer_state(state, PrinterState::Stopped, at_ms(2000), 2);

    EXPECT_EQ(state.subject, SubjectState::Stopped);
    ASSERT_TRUE(state.last_completed_job.present);
    EXPECT_EQ(state.last_completed_job.terminal_state, PrinterState::Stopped);
    EXPECT_EQ(state.last_completed_job.layers.current, 156U);
}

TEST(JobLifecycleTests, CompletionIsPersistedOnceNotOnEveryRepeat) {
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Finished, at_ms(2000), 2);
    const Timestamp first = state.last_completed_job.completed_at;

    state = with_printer_state(state, PrinterState::Finished, at_ms(4000), 3);

    EXPECT_EQ(state.last_completed_job.completed_at, first);
}

TEST(JobLifecycleTests, ANewJobIdentityDiscardsThePreviousJobsFigures) {
    AppState state = printing_device(at_ms(0));
    ASSERT_EQ(state.current_job.layers.current, 156U);

    JobObservation next;
    next.sequence = 2;
    next.id.assign("job-2");
    next.display_name.assign("bracket.gcode");
    next.normalized_state = PrinterState::Printing;

    state = reduce(state, Event::job_observed(at_ms(1000), next));

    EXPECT_EQ(state.current_job.id.view(), "job-2");
    EXPECT_FALSE(state.current_job.layers.known);
    EXPECT_FALSE(state.current_job.progress.known);
    EXPECT_EQ(state.current_job.locally_inferred_started_at, at_ms(1000));
}

TEST(JobLifecycleTests, MissingJobDataDoesNotImmediatelyEraseTheJob) {
    // §7.8, stated as a prohibition.
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::job_unavailable(at_ms(1000), 2));

    EXPECT_TRUE(state.current_job.present);
    EXPECT_TRUE(state.current_job.reported_missing);
    EXPECT_EQ(state.current_job.display_name.view(), "CORE One.gcode");
}

TEST(JobLifecycleTests, AJobMissingLongEnoughIsEventuallyCleared) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::job_unavailable(at_ms(1000), 2));

    state = reduce(state, Event::confidence_aged(at_ms(120000)));

    EXPECT_FALSE(state.current_job.present);
}

TEST(JobLifecycleTests, AReturningJobClearsTheMissingMark) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::job_unavailable(at_ms(1000), 2));
    ASSERT_TRUE(state.current_job.reported_missing);

    state = reduce(state, Event::job_observed(at_ms(2000), printing_job(3)));

    EXPECT_FALSE(state.current_job.reported_missing);
    EXPECT_TRUE(state.current_job.present);
}

TEST(JobLifecycleTests, OutOfRangeProgressIsRecordedNotCorrected) {
    // §7.8: clamping is for display only, so the state keeps what arrived and the
    // discrepancy is counted.
    AppState state = printing_device(at_ms(0));

    JobObservation impossible = printing_job(2);
    impossible.progress = ProgressValue{true, 137};

    state = reduce(state, Event::job_observed(at_ms(1000), impossible));

    EXPECT_EQ(state.current_job.progress.percent, 137U);
    EXPECT_EQ(state.diagnostics.out_of_range_progress_seen, 1U);
}

// --- Repeated, stale and out-of-order events -------------------------------

TEST(EventOrderingTests, ARepeatedEventChangesNothing) {
    const AppState state = printing_device(at_ms(1000));
    const Event event =
        Event::printer_snapshot(at_ms(2000), printer_snapshot(PrinterState::Paused, 2));

    const AppState once = reduce(state, event);
    const AppState twice = reduce(once, event);

    EXPECT_EQ(twice.revision, once.revision) << "an identical repeat published a new revision";
    EXPECT_TRUE(content_equals(once, twice));
    EXPECT_EQ(twice.diagnostics.events_rejected_stale, 1U);
}

TEST(EventOrderingTests, AStaleSequenceIsRejected) {
    AppState state = printing_device(at_ms(0));
    state = with_printer_state(state, PrinterState::Paused, at_ms(1000), 5);
    const std::uint32_t revision = state.revision;

    // Sequence 4 is older than the accepted 5: a late response overtaken by a newer
    // one. Applying it would move the product backwards.
    state = with_printer_state(state, PrinterState::Printing, at_ms(2000), 4);

    EXPECT_EQ(state.subject, SubjectState::Paused);
    EXPECT_EQ(state.revision, revision);
    EXPECT_EQ(state.diagnostics.events_rejected_stale, 1U);
}

TEST(EventOrderingTests, AnUnsequencedObservationIsAlwaysAccepted) {
    // Sequence 0 means "no generation counter", which a fixture or a fake source
    // legitimately has.
    AppState state = booted_device(at_ms(0));

    state = with_printer_state(state, PrinterState::Printing, at_ms(100), 0);
    EXPECT_EQ(state.subject, SubjectState::Printing);

    state = with_printer_state(state, PrinterState::Paused, at_ms(200), 0);
    EXPECT_EQ(state.subject, SubjectState::Paused);
    EXPECT_EQ(state.diagnostics.events_rejected_stale, 0U);
}

TEST(EventOrderingTests, TimeIsClampedForwardAndTheDisorderIsCounted) {
    AppState state = printing_device(at_ms(10000));
    ASSERT_EQ(state.last_event_at, at_ms(10000));

    // An event from the past. Rewinding would make an old observation look fresh.
    state = reduce(state, Event::confidence_aged(at_ms(5000)));

    EXPECT_EQ(state.last_event_at, at_ms(10000));
    EXPECT_EQ(state.diagnostics.events_out_of_order, 1U);
}

TEST(EventOrderingTests, AMisstampedObservationIsTreatedAsArrivingNow) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::confidence_aged(at_ms(120000)));
    ASSERT_EQ(state.confidence, Confidence::Unavailable);

    // A snapshot carrying a timestamp from the past. Its clock is clamped to the
    // latest time already seen, so it counts as arriving now rather than as an
    // observation 120 seconds old — the alternative would let one bad timestamp
    // make a live reading look ancient.
    state = with_printer_state(state, PrinterState::Printing, at_ms(500), 2);

    EXPECT_EQ(state.diagnostics.events_out_of_order, 1U);
    EXPECT_EQ(state.printer.observed_at, at_ms(120000));

    // Confidence recovers, and should: §8.1 makes the sequence — not the timestamp
    // — the guard against a stale response, and this sequence is newer.
    EXPECT_EQ(state.confidence, Confidence::Current);
}

TEST(EventOrderingTests, DiagnosticCountersDoNotPublishARevision) {
    AppState state = printing_device(at_ms(1000));
    const std::uint32_t revision = state.revision;

    // Rejected duplicate: the counter moves, the product does not.
    state = with_printer_state(state, PrinterState::Printing, at_ms(1000), 1);

    EXPECT_EQ(state.revision, revision);
    EXPECT_EQ(state.diagnostics.events_rejected_stale, 1U);
}

// --- Device operations -----------------------------------------------------

TEST(DeviceOperationTests, DeviceFaultOutranksEverythingElse) {
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::recovery_mode_entered(at_ms(1000)));
    EXPECT_EQ(state.device_operation, DeviceOperation::RecoveryMode);

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    installing.progress = ProgressValue{true, 40};
    state = reduce(state, Event::ota_observed(at_ms(2000), installing));
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);

    state = reduce(state, Event::device_fault(at_ms(3000), DeviceFaultReason::LowMemory));
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault);
}

TEST(DeviceOperationTests, InstallRemainsExclusiveThroughRequiredReboot) {
    // §18.6 forbids silent installation, so an available update is information
    // rather than an operation, and a background check must not take the screen.
    AppState state = printing_device(at_ms(0));

    OtaObservation checking;
    checking.phase = OtaPhase::Checking;
    state = reduce(state, Event::ota_observed(at_ms(1000), checking));
    EXPECT_EQ(state.device_operation, DeviceOperation::None);

    OtaObservation available;
    available.phase = OtaPhase::Available;
    available.available_version.assign("0.2.0");
    state = reduce(state, Event::ota_observed(at_ms(2000), available));
    EXPECT_EQ(state.device_operation, DeviceOperation::None);

    OtaObservation downloading;
    downloading.phase = OtaPhase::Downloading;
    downloading.progress = ProgressValue{true, 10};
    state = reduce(state, Event::ota_observed(at_ms(3000), downloading));
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);

    OtaObservation done;
    done.phase = OtaPhase::RebootRequired;
    state = reduce(state, Event::ota_observed(at_ms(4000), done));
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);
}

TEST(DeviceOperationTests, AvailabilityNoticeWaitsForReadyUsesOneDisplayWindowAndDoesNotReplay) {
    AppState state = printing_device(at_ms(0));
    OtaObservation available;
    available.phase = OtaPhase::Available;
    available.available_version.assign("0.2.0");
    state = reduce(state, Event::ota_observed(at_ms(1000), available));
    EXPECT_FALSE(state.ota.notice_presenting);
    EXPECT_FALSE(project(state).update_notice);

    state = with_printer_state(state, PrinterState::Ready, at_ms(2000), 2);
    EXPECT_TRUE(state.ota.notice_presenting);
    EXPECT_TRUE(state.ota.notice_spent);
    EXPECT_TRUE(project(state).update_notice);

    state = reduce(state, Event::display_idle_timer_fired(at_ms(22000)));
    EXPECT_FALSE(state.ota.notice_presenting);
    EXPECT_TRUE(state.ota.notice_spent);
    EXPECT_FALSE(project(state).update_notice);

    state = reduce(state, Event::display_wake(at_ms(23000)));
    state = reduce(state, Event::ota_observed(at_ms(24000), available));
    EXPECT_FALSE(state.ota.notice_presenting) << "the same release must not replay";

    available.available_version.assign("0.3.0");
    state = reduce(state, Event::ota_observed(at_ms(25000), available));
    EXPECT_TRUE(state.ota.notice_presenting) << "a distinct release opens a new episode";
}

TEST(DeviceOperationTests, RecoveryModeIsNotAnExclusiveOperation) {
    // UIAD 001 §12 groups Recovery Mode with provisioning at tier 3, below the
    // exclusive operations at tier 2.
    AppState state = booted_device(at_ms(0));

    state = reduce(state, Event::recovery_mode_entered(at_ms(100)));
    EXPECT_EQ(state.device_operation, DeviceOperation::RecoveryMode);

    state = reduce(state, Event::recovery_mode_exited(at_ms(200)));
    EXPECT_EQ(state.device_operation, DeviceOperation::None);
}

TEST(DeviceOperationTests, RebootRequiredCannotArmOrConfirmFactoryReset) {
    AppState state = printing_device(at_ms(0));
    OtaObservation selected;
    selected.phase = OtaPhase::RebootRequired;
    state = reduce(state, Event::ota_observed(at_ms(1000), selected));
    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::VeryLongPress));
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    state = reduce(state, Event::factory_reset_confirmed(at_ms(3000)));
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);
}

TEST(DeviceOperationTests, AcceptedDownloadCannotArmOrConfirmFactoryReset) {
    AppState state = printing_device(at_ms(0));
    OtaObservation accepted;
    accepted.phase = OtaPhase::Downloading;
    accepted.available_version.assign("0.2.0");
    accepted.progress = ProgressValue{true, 0};
    state = reduce(state, Event::ota_observed(at_ms(1000), accepted));

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::VeryLongPress));
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    state = reduce(state, Event::factory_reset_confirmed(at_ms(3000)));
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);
}

TEST(DeviceOperationTests, AStorageFailureIsADeviceFault) {
    const AppState state =
        reduce(AppState{}, Event::storage_observed(at_ms(0), StorageStatus::Failed));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::StorageFailure);
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault);
}

TEST(DeviceOperationTests, RecoveredStorageIsNotAFault) {
    // §13.4: "migration failure must not erase secrets automatically" — recovering
    // with defaults is a degraded success, not a broken device.
    const AppState state =
        reduce(AppState{}, Event::storage_observed(at_ms(0), StorageStatus::RecoveredWithDefaults));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
}

TEST(DeviceOperationTests, PrinterProblemsNeverBecomeADeviceFault) {
    // UIAD 001 §10: a device fault "shall never represent subject errors,
    // authentication failures, unsupported interfaces or ordinary connectivity
    // loss". Structural — DeviceFaultReason cannot express any of them.
    AppState state = printing_device(at_ms(0));

    state = with_printer_state(state, PrinterState::Error, at_ms(1000), 2);
    state = with_connection(state, ConnectionPhase::Unauthorized, at_ms(2000));
    state = with_connection(state, ConnectionPhase::UnsupportedApi, at_ms(3000));
    state = with_connection(state, ConnectionPhase::PrinterUnavailable, at_ms(4000));

    AlertObservation printer_error;
    printer_error.severity = AlertSeverity::Error;
    printer_error.source = AlertSource::PrinterState;
    printer_error.title.assign("Thermal runaway");
    state = reduce(state, Event::alert_raised(at_ms(5000), printer_error));

    EXPECT_EQ(state.device_fault, DeviceFaultReason::None);
    EXPECT_NE(state.device_operation, DeviceOperation::DeviceFault);
    EXPECT_EQ(state.subject_condition, SubjectCondition::Error);
}

// --- Modal surfaces and gestures ------------------------------------------

TEST(GestureTests, ShortPressWakesTheDisplayWithoutOpeningAnything) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(1000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::ShortPress));

    // §17.8: a button press is a wake condition.
    EXPECT_EQ(state.display.power, DisplayPower::On);
    EXPECT_FALSE(state.display.idle_mode);
    EXPECT_EQ(state.display.last_interaction_at, at_ms(2000));

    // §5.6's "cycle a limited secondary view" half is deliberately unimplemented:
    // the accepted state architecture defines no secondary view.
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
}

TEST(GestureTests, LongPressNoLongerOpensSettings) {
    // ⬅️ CHANGED BY OWNER RULING 1 OF MILESTONE 3.10, and this test is the record of it.
    //
    // §5.6's "long press: enter local setup/diagnostics flow" was read as UIAD 001 §9's Settings
    // in Milestone 2; the ruling settles it the other way — the long press is now the deliberate
    // physical-presence gesture that enters reconfiguration, which is R1's requirement.
    //
    // ⛔ AND THE REDUCER RECORDS NOTHING FOR IT. Re-entry starts a radio and a privileged server;
    // §5.2 forbids this function from performing side effects, so app_core observes the same event
    // and acts on it. The absence asserted here is deliberate, not an unimplemented case.
    //
    // Settings remains in UIAD 001's architecture and has no Production UI §9 screen. It is simply
    // no longer reachable from the button.
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::LongPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::None);

    // The subject continues undisturbed: a long press changes no application state at all beyond
    // the wake the gesture handler performs for every press.
    EXPECT_EQ(state.subject, SubjectState::Printing);
    EXPECT_EQ(state.display.power, DisplayPower::On);
}

TEST(GestureTests, VeryLongPressOpensConfirmationRatherThanResetting) {
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::None);
}

TEST(GestureTests, ADismissalClosesTheSurface) {
    // Opened with the gesture that still opens a modal after owner ruling 1 — the very long press
    // and its factory-reset confirmation.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    ASSERT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);

    state = reduce(state, Event::modal_surface_dismissed(at_ms(2000)));

    EXPECT_EQ(state.modal_surface, ModalSurface::None);
}

TEST(FactoryResetTests, RequiresTheConfirmationSurface) {
    // §5.6: factory reset "must be difficult to trigger accidentally". The
    // confirmation is a precondition in the state machine, not a UI convention.
    AppState state = printing_device(at_ms(0));
    ASSERT_EQ(state.modal_surface, ModalSurface::None);

    state = reduce(state, Event::factory_reset_confirmed(at_ms(1000)));

    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::None);
}

TEST(FactoryResetTests, ProceedsFromTheConfirmationSurface) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    state = reduce(state, Event::factory_reset_confirmed(at_ms(2000)));

    EXPECT_TRUE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::FactoryResetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);

    // ⬅️ AND NOTHING ENDS IT FROM HERE — audit item 5, 2026-08-28. FactoryResetCompleted was deleted:
    // a successful reset ends by restarting, and a failed one raises a device fault and keeps
    // ownership. There is no event that clears `factory_resetting` on a running device, which is the
    // property this now asserts instead of the removed one.
    state = reduce(state, Event::confidence_aged(at_ms(3000)));
    state = reduce(state, Event::confidence_aged(at_ms(3100)));

    EXPECT_TRUE(state.factory_resetting)
        << "an ordinary observation must not end a confirmed reset";
    EXPECT_EQ(state.device_operation, DeviceOperation::FactoryResetting);
}

// --- Owner ruling A of Milestone 3.10: the two-press confirmation sequence ---
//
// ⛔ THIS IS THE ONLY PATH TO A WIPE. app_core runs the erase when factory_resetting becomes true,
// and the reducer is the only thing that sets it. Every precondition a factory reset has is
// asserted below; §5.6 requires the behaviour to be "difficult to trigger accidentally", and these
// are what make that a property rather than an intention.

TEST(FactoryResetTests, TheFirstVeryLongPressArmsAndDoesNotReset) {
    AppState state = printing_device(at_ms(0));

    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_NE(state.factory_reset_armed_at, Timestamp{});
}

TEST(FactoryResetTests, ASecondVeryLongPressInsideTheWindowConfirms) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    state = reduce(state, Event::button_gesture(at_ms(6000), ButtonGesture::VeryLongPress));

    EXPECT_TRUE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::FactoryResetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_EQ(state.factory_reset_armed_at, Timestamp{});
}

TEST(FactoryResetTests, ConfirmingAtTheWindowBoundaryStillWorks) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    const auto armed = state.factory_reset_armed_at;
    const auto last_moment = armed + kFactoryResetConfirmationWindow;

    state = reduce(state, Event::button_gesture(last_moment, ButtonGesture::VeryLongPress));

    EXPECT_TRUE(state.factory_resetting);
}

// ⛔ A STALE CONFIRMATION IS NOT A CONFIRMATION. Without this the modal could be completed by a
// press arriving arbitrarily later, which is the accidental trigger §5.6 names, merely slowed down.
TEST(FactoryResetTests, ASecondPressAfterTheWindowWithdrawsInsteadOfResetting) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    const auto armed = state.factory_reset_armed_at;
    const auto too_late = armed + kFactoryResetConfirmationWindow + Timestamp{1};

    state = reduce(state, Event::button_gesture(too_late, ButtonGesture::VeryLongPress));

    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_EQ(state.factory_reset_armed_at, Timestamp{});
}

TEST(FactoryResetTests, TheConfirmedEventIsAlsoBoundedByTheWindow) {
    // The event path shares the guard rather than having its own. A second producer that checked
    // only the modal would be a second route to a wipe.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    const auto too_late =
        state.factory_reset_armed_at + kFactoryResetConfirmationWindow + Timestamp{1};

    state = reduce(state, Event::factory_reset_confirmed(too_late));

    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::None);
}

TEST(FactoryResetTests, AShortPressCancelsTheConfirmation) {
    // The cheapest input withdraws the destructive one — the asymmetry §5.6 asks for.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    ASSERT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::ShortPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.factory_reset_armed_at, Timestamp{});

    // And a press after the cancellation arms afresh rather than completing the old one.
    state = reduce(state, Event::button_gesture(at_ms(3000), ButtonGesture::VeryLongPress));
    EXPECT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);
    EXPECT_FALSE(state.factory_resetting);
}

TEST(FactoryResetTests, ALongPressNeverConfirms) {
    // Owner ruling A: "LongPress does not confirm reset". It is the reconfiguration gesture, and
    // inside the confirmation it must do nothing at all.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::LongPress));

    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation)
        << "a long press must neither confirm nor cancel; the prompt stands";
}

TEST(FactoryResetTests, NoBriefPressCanEverReachAWipe) {
    // The whole of §5.6's "difficult to trigger accidentally", stated as a property: no sequence
    // of short and long presses, of any length, reaches a reset.
    AppState state = printing_device(at_ms(0));

    for (int press = 0; press < 40; ++press) {
        const auto moment = at_ms(1000 + (press * 100));
        const ButtonGesture gesture =
            (press % 2 == 0) ? ButtonGesture::ShortPress : ButtonGesture::LongPress;

        state = reduce(state, Event::button_gesture(moment, gesture));

        ASSERT_FALSE(state.factory_resetting) << "reached a reset after " << press << " presses";
    }
}

TEST(FactoryResetTests, TheCancellationEventWithdrawsAnArmedConfirmation) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));

    state = reduce(state, Event::factory_reset_cancelled(at_ms(2000)));

    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.factory_reset_armed_at, Timestamp{});
}

TEST(FactoryResetTests, AnUnarmedConfirmationCannotBeCompletedByTimestampArithmetic) {
    // factory_reset_armed_at is zero when nothing is armed. `now - 0` is a very large interval,
    // so a comparison that only checked "within the window" the wrong way round would let an
    // unarmed device confirm. Reachable only by forcing the modal without arming it.
    AppState state = printing_device(at_ms(0));
    state.modal_surface = ModalSurface::FactoryResetConfirmation;
    state.factory_reset_armed_at = Timestamp{};

    state = reduce(state, Event::factory_reset_confirmed(at_ms(1000)));

    EXPECT_FALSE(state.factory_resetting);
}

TEST(FactoryResetTests, ATimestampThatMovesBackwardsIsClampedRatherThanRewindingTheWindow) {
    // ⬅️ WHAT ACTUALLY HAPPENS, AND WHY IT IS RIGHT. reduce() clamps every event's clock forward to
    // the last one seen (see its comment: "time only moves forward"), so a backwards-stamped event
    // is applied AT the arming moment rather than before it. The confirmation therefore reads as
    // arriving instantly — which is the same as pressing again immediately, and is correct.
    //
    // This test exists because the first version of it asserted the opposite, on the assumption
    // that the raw timestamp reached the window check. It does not, and the clamp is the stronger
    // guarantee: a producer with an unsynchronised clock cannot move the window at all, in either
    // direction. The out-of-order event is counted rather than hidden.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(10000), ButtonGesture::VeryLongPress));

    const auto before = state.diagnostics.events_out_of_order;

    state = reduce(state, Event::factory_reset_confirmed(at_ms(1000)));

    EXPECT_GT(state.diagnostics.events_out_of_order, before)
        << "a backwards timestamp must be counted, not silently accepted at face value";
    EXPECT_TRUE(state.factory_resetting)
        << "clamped to the arming moment, so it is inside the window";
}

TEST(ExclusiveOperationTests, BlocksModalOpeningButNotWaking) {
    // §18.5: during an install, "disable factory reset and ordinary setup
    // mutations". Waking the panel is not a mutation — the user pressed a button and
    // is entitled to see what the device is doing.
    AppState state = printing_device(at_ms(0));

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    installing.progress = ProgressValue{true, 55};
    state = reduce(state, Event::ota_observed(at_ms(1000), installing));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(1500)));

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::VeryLongPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::None);
    EXPECT_EQ(state.display.power, DisplayPower::On);
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);
}

TEST(ExclusiveOperationTests, BlocksFactoryResetConfirmation) {
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(500), ButtonGesture::VeryLongPress));
    ASSERT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);

    OtaObservation installing;
    installing.phase = OtaPhase::Installing;
    state = reduce(state, Event::ota_observed(at_ms(1000), installing));

    state = reduce(state, Event::factory_reset_confirmed(at_ms(2000)));

    EXPECT_FALSE(state.factory_resetting);
    EXPECT_EQ(state.device_operation, DeviceOperation::FirmwareUpdating);
}

// --- Display power --------------------------------------------------------

TEST(DisplayPowerTests, IdleThenSleepAndNeverBackUpOnATimer) {
    AppState state = printing_device(at_ms(0));
    ASSERT_EQ(state.display.power, DisplayPower::On);

    state = reduce(state, Event::display_idle_timer_fired(at_ms(1000)));
    EXPECT_EQ(state.display.power, DisplayPower::Dimmed);
    EXPECT_TRUE(state.display.idle_mode);

    state = reduce(state, Event::display_sleep_timer_fired(at_ms(2000)));
    EXPECT_EQ(state.display.power, DisplayPower::Off);

    // An idle timer must never wake a sleeping panel.
    state = reduce(state, Event::display_idle_timer_fired(at_ms(3000)));
    EXPECT_EQ(state.display.power, DisplayPower::Off);
}

TEST(DisplayPowerTests, MonitoringContinuesWhileTheDisplayIsOff) {
    // §7.11's closing rule and §17.8: display power does not stop monitoring.
    AppState state = printing_device(at_ms(0));
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(1000)));

    state = with_printer_state(state, PrinterState::Paused, at_ms(2000), 2);

    EXPECT_EQ(state.display.power, DisplayPower::Off);
    EXPECT_EQ(state.subject, SubjectState::Paused);
    EXPECT_EQ(state.confidence, Confidence::Current);
}

// --- Secrets -------------------------------------------------------------

TEST(SecretTests, ConfigurationCarriesFlagsAndMetadataOnly) {
    // §7.3, §13.5, §24.3: secrets are never in AppState. The type system is the
    // guarantee — there is nowhere to put one — and this pins what is carried
    // instead.
    AppState state = booted_device(at_ms(0));

    EXPECT_TRUE(state.configuration.has_wifi_credentials);
    EXPECT_TRUE(state.configuration.has_printer_configuration);
    EXPECT_EQ(state.configuration.auth_method, AuthMethod::ApiKey);
    EXPECT_EQ(state.configuration.printer_address.view(), "printer.local");
}

// --- Checkpoint 3c: a quarantined Wi-Fi driver is a local Device Fault ---------
//
// ⛔ EVERY PRECONDITION IS BUILT THROUGH THE REAL REDUCER, never by assigning to AppState. The
// owner's DriverStalled ruling says the panel must show an actionable outcome at the highest
// presentation priority; these are the model facts that carry it there.

namespace {

[[nodiscard]] NetworkObservation station(const NetworkPhase phase, const NetworkFault fault) {
    NetworkObservation observation;
    observation.phase = phase;
    observation.fault = fault;
    return observation;
}

} // namespace

TEST(ReducerNetworkFaultTests, ADriverUnresponsiveObservationRaisesTheWifiDeviceFault) {
    const AppState state =
        reduce(booted_device(at_ms(1000)),
               Event::network_observed(at_ms(2000), station(NetworkPhase::Unavailable,
                                                            NetworkFault::DriverUnresponsive)));

    EXPECT_EQ(state.network.fault, NetworkFault::DriverUnresponsive);
    EXPECT_EQ(state.device_fault, DeviceFaultReason::WifiUnavailable);

    // ⛔ AND IT REACHES §12.1, WHICH IS THE POINT. UIAD 001 puts a device fault above every other
    // tier, including the provisioning session the station was being validated for.
    EXPECT_EQ(state.device_operation, DeviceOperation::DeviceFault);
}

TEST(ReducerNetworkFaultTests, OrdinaryWifiFaultsRaiseNoDeviceFault) {
    // §16.1 keeps a device error and ordinary connectivity loss apart, and so must this. A lost link
    // or a rejected credential is a network condition the device recovers from on its own.
    const NetworkFault ordinary[] = {NetworkFault::None, NetworkFault::LinkLost,
                                     NetworkFault::CredentialsRejected};

    for (const auto fault : ordinary) {
        const AppState state =
            reduce(booted_device(at_ms(1000)),
                   Event::network_observed(at_ms(2000), station(NetworkPhase::Unavailable, fault)));

        EXPECT_EQ(state.network.fault, fault);
        EXPECT_EQ(state.device_fault, DeviceFaultReason::None) << "fault=" << log_label(fault);
        EXPECT_NE(state.device_operation, DeviceOperation::DeviceFault);
    }
}

TEST(ReducerNetworkFaultTests, NothingTheStationReportsAfterwardsLowersTheFault) {
    // ⛔ THE RULING: recovery requires an explicit restart. A late completion, a successful
    // association on some other identity, even a full lease — none of them may clear it, because the
    // station is quarantined and will not issue another driver operation regardless.
    const AppState stalled =
        reduce(booted_device(at_ms(1000)),
               Event::network_observed(at_ms(2000), station(NetworkPhase::Unavailable,
                                                            NetworkFault::DriverUnresponsive)));

    ASSERT_EQ(stalled.device_fault, DeviceFaultReason::WifiUnavailable);

    NetworkObservation connected = station(NetworkPhase::Connected, NetworkFault::None);
    (void)connected.ip_address.assign("192.0.2.10");

    const AppState later = reduce(stalled, Event::network_observed(at_ms(3000), connected));

    EXPECT_EQ(later.device_fault, DeviceFaultReason::WifiUnavailable)
        << "only a boot or a factory reset clears a device fault";
    EXPECT_EQ(later.device_operation, DeviceOperation::DeviceFault);
}

TEST(ReducerNetworkFaultTests, TheFaultDoesNotDependOnTheDiagnosticText) {
    // ⛔ NO STRING MATCHING ANYWHERE ON THIS PATH. `last_error` is a §7.4 diagnostic; the decision is
    // taken from the typed enumerator. An observation with EMPTY text and the typed fault still
    // raises it, and an observation whose text says exactly the same thing without the typed fault
    // does not.
    const NetworkObservation typed =
        station(NetworkPhase::Unavailable, NetworkFault::DriverUnresponsive);
    ASSERT_TRUE(typed.error.empty());

    const AppState raised =
        reduce(booted_device(at_ms(1000)), Event::network_observed(at_ms(2000), typed));
    EXPECT_EQ(raised.device_fault, DeviceFaultReason::WifiUnavailable);

    NetworkObservation worded = station(NetworkPhase::Unavailable, NetworkFault::None);
    (void)worded.error.assign("wi-fi driver did not respond");

    const AppState quiet =
        reduce(booted_device(at_ms(1000)), Event::network_observed(at_ms(2000), worded));
    EXPECT_EQ(quiet.device_fault, DeviceFaultReason::None)
        << "the words must not be able to raise the product's highest-priority screen";
}
