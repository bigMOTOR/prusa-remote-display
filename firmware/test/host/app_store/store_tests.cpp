// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "app_model/presentation.hpp"
#include "app_store/store.hpp"
#include "support/state_builders.hpp"

// The store suite.
//
// The store's job is narrow — own the state, run the reducer, publish revisions —
// so most of what is worth testing is about what it refuses: notifying for a
// non-mutation, growing its observer table, and allowing a reentrant dispatch that
// would publish revisions out of order.

using prd::app_model::AppState;
using prd::app_model::ButtonGesture;
using prd::app_model::Event;
using prd::app_model::FreshnessPolicy;
using prd::app_model::ModalSurface;
using prd::app_model::PrinterState;
using prd::app_model::ReducerPolicy;
using prd::app_model::SubjectState;
using prd::app_store::Store;
using prd::test::at_ms;
using prd::test::printer_snapshot;
using prd::test::printing_device;

namespace {

struct ObserverRecord {
    int calls{0};
    std::uint32_t last_revision{0};
    SubjectState last_subject{SubjectState::AcquiringState};
};

void record_observer(const AppState& state, void* const context) {
    auto* const record = static_cast<ObserverRecord*>(context);

    ++record->calls;
    record->last_revision = state.revision;
    record->last_subject = state.subject;
}

// An observer that tries to dispatch from inside its own notification. This is the
// failure the reentrancy guard exists for.
struct ReentrantObserver {
    Store* store{nullptr};
    int calls{0};
    bool nested_accepted{true};
};

void reentrant_observer(const AppState& /*state*/, void* const context) {
    auto* const observer = static_cast<ReentrantObserver*>(context);

    ++observer->calls;

    const auto outcome =
        observer->store->dispatch(Event::button_gesture(at_ms(9000), ButtonGesture::VeryLongPress));

    observer->nested_accepted = outcome.accepted;
}

// An observer that tries to register another observer while the table is being
// walked — the other way to corrupt a notification pass.
struct RegisteringObserver {
    Store* store{nullptr};
    ObserverRecord late;
    bool accepted{true};
};

void registering_observer(const AppState& /*state*/, void* const context) {
    auto* const registrar = static_cast<RegisteringObserver*>(context);

    registrar->accepted = registrar->store->add_observer(record_observer, &registrar->late);
}

} // namespace

TEST(StoreTests, StartsFromADefaultState) {
    const Store store;

    EXPECT_EQ(store.revision(), 0U);
    EXPECT_EQ(store.state().subject, SubjectState::AcquiringState);
    EXPECT_EQ(store.observer_count(), 0U);
    EXPECT_EQ(store.statistics().dispatched, 0U);
}

TEST(StoreTests, StartsFromAGivenState) {
    const AppState initial = printing_device(at_ms(1000));
    const Store store{initial};

    EXPECT_EQ(store.state(), initial);
    EXPECT_EQ(store.revision(), initial.revision);
}

TEST(StoreTests, DispatchAdvancesTheStateAndReportsTheOutcome) {
    Store store{printing_device(at_ms(0))};
    const std::uint32_t before = store.revision();

    const auto outcome = store.dispatch(
        Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)));

    EXPECT_TRUE(outcome.accepted);
    EXPECT_TRUE(outcome.mutated);
    EXPECT_EQ(outcome.revision, before + 1);
    EXPECT_EQ(store.state().subject, SubjectState::Paused);
}

TEST(StoreTests, AnAcceptedEventThatChangesNothingIsNotAMutation) {
    // §7.1 ties the revision to an "accepted state mutation". A duplicate is
    // accepted and does nothing, and the outcome says exactly that.
    Store store{printing_device(at_ms(0))};

    const Event duplicate =
        Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2));

    const auto first = store.dispatch(duplicate);
    const auto second = store.dispatch(duplicate);

    EXPECT_TRUE(first.mutated);
    EXPECT_TRUE(second.accepted);
    EXPECT_FALSE(second.mutated);
    EXPECT_EQ(second.revision, first.revision);

    EXPECT_EQ(store.statistics().dispatched, 2U);
    EXPECT_EQ(store.statistics().mutations, 1U);
}

TEST(StoreTests, A_TRANSITION_THAT_ONLY_LATCHES_THE_RESET_FAILURE_IS_RETAINED) {
    // ⛔ THE STORE MUST NOT DISCARD IT — audit item 5. §7.1 ties the revision to an "accepted state
    // mutation", and the store decides that with content_equals(). If the reset latch were left out
    // of that comparison, the one dispatch that makes a reset TERMINAL would be reported as changing
    // nothing: no revision bump, no observer notification, no re-projection — at the exact moment the
    // panel has to start saying the reset could not finish.
    //
    // ⬅️ THE TRANSITION IS ISOLATED ON PURPOSE. The state already carries the fault reason, so
    // re-raising it moves nothing except the latch — which is what makes this an assertion about the
    // latch rather than about the fault.
    AppState armed = printing_device(at_ms(0));
    armed.factory_resetting = true;
    armed.device_fault = prd::app_model::DeviceFaultReason::FactoryResetIncomplete;
    armed.factory_reset_failed = false;

    Store store{armed};
    ASSERT_FALSE(store.state().factory_reset_failed);

    const auto outcome = store.dispatch(Event::device_fault(
        at_ms(1000), prd::app_model::DeviceFaultReason::FactoryResetIncomplete));

    EXPECT_TRUE(outcome.accepted);
    EXPECT_TRUE(outcome.mutated) << "the store discarded the transition that ended the reset";
    EXPECT_EQ(outcome.revision, store.revision());
    EXPECT_TRUE(store.state().factory_reset_failed);

    // And the reason itself did not move, so nothing but the latch can account for the mutation.
    EXPECT_EQ(store.state().device_fault,
              prd::app_model::DeviceFaultReason::FactoryResetIncomplete);

    // POSITIVE CONTROL: dispatching it a second time now really does change nothing, which is what
    // makes the mutation above attributable to the latch rather than to the event being accepted.
    const auto again = store.dispatch(Event::device_fault(
        at_ms(2000), prd::app_model::DeviceFaultReason::FactoryResetIncomplete));

    EXPECT_TRUE(again.accepted);
    EXPECT_FALSE(again.mutated);
    EXPECT_EQ(store.statistics().mutations, 1U);
}

TEST(StoreTests, SnapshotIsAnIndependentCopy) {
    // §10.7: "no shared mutable domain state across tasks". A consumer on another
    // task holds this, and it must not change underneath it.
    Store store{printing_device(at_ms(0))};

    const AppState snapshot = store.snapshot();
    store.dispatch(Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)));

    EXPECT_EQ(snapshot.subject, SubjectState::Printing);
    EXPECT_EQ(store.state().subject, SubjectState::Paused);
}

TEST(StoreTests, ObserversAreNotifiedOnMutationOnly) {
    Store store{printing_device(at_ms(0))};

    ObserverRecord record;
    ASSERT_TRUE(store.add_observer(record_observer, &record));
    EXPECT_EQ(store.observer_count(), 1U);

    const Event change =
        Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2));

    store.dispatch(change);
    EXPECT_EQ(record.calls, 1);
    EXPECT_EQ(record.last_subject, SubjectState::Paused);
    EXPECT_EQ(record.last_revision, store.revision());

    // The same event again: accepted, changes nothing, wakes nobody. UI Behaviour
    // Decision 001 §4 requires routine polling not to become visible, and this is
    // where that starts.
    store.dispatch(change);
    EXPECT_EQ(record.calls, 1);
}

TEST(StoreTests, EveryRegisteredObserverIsNotified) {
    Store store{printing_device(at_ms(0))};

    ObserverRecord first;
    ObserverRecord second;
    ASSERT_TRUE(store.add_observer(record_observer, &first));
    ASSERT_TRUE(store.add_observer(record_observer, &second));

    store.dispatch(Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)));

    EXPECT_EQ(first.calls, 1);
    EXPECT_EQ(second.calls, 1);
    EXPECT_EQ(store.statistics().notifications, 2U);
}

TEST(StoreTests, TheObserverTableIsBounded) {
    // §10.7 requires bounded structures. A fifth registration fails rather than
    // allocating.
    Store store;
    ObserverRecord records[Store::kMaxObservers + 1];

    for (std::size_t index = 0; index < Store::kMaxObservers; ++index) {
        EXPECT_TRUE(store.add_observer(record_observer, &records[index])) << "index " << index;
    }

    EXPECT_FALSE(store.add_observer(record_observer, &records[Store::kMaxObservers]));
    EXPECT_EQ(store.observer_count(), Store::kMaxObservers);
    EXPECT_EQ(store.statistics().observers_rejected, 1U);
}

TEST(StoreTests, ANullObserverIsRejected) {
    Store store;

    EXPECT_FALSE(store.add_observer(nullptr, nullptr));
    EXPECT_EQ(store.observer_count(), 0U);
}

TEST(StoreTests, ReentrantDispatchIsRefusedRatherThanReordered) {
    Store store{printing_device(at_ms(0))};

    ReentrantObserver observer;
    observer.store = &store;
    ASSERT_TRUE(store.add_observer(reentrant_observer, &observer));

    const auto outcome = store.dispatch(
        Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)));

    EXPECT_TRUE(outcome.accepted);
    EXPECT_EQ(observer.calls, 1);

    // The nested dispatch was refused. Had it been allowed, it would have reduced a
    // second event against a state still being published, and observers would have
    // been notified in an order that did not match the revisions they received.
    EXPECT_FALSE(observer.nested_accepted);
    EXPECT_EQ(store.statistics().reentrant_rejections, 1U);

    // And it left no trace on the state.
    EXPECT_EQ(store.state().modal_surface, ModalSurface::None);
    EXPECT_EQ(store.revision(), outcome.revision);
}

TEST(StoreTests, RegistrationDuringNotificationIsRefused) {
    Store store{printing_device(at_ms(0))};

    RegisteringObserver registrar;
    registrar.store = &store;
    ASSERT_TRUE(store.add_observer(registering_observer, &registrar));

    store.dispatch(Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)));

    EXPECT_FALSE(registrar.accepted);
    EXPECT_EQ(store.observer_count(), 1U);
}

TEST(StoreTests, DispatchIsSerializedByConstructionAndPublishesInOrder) {
    // §5.2 and §10.2: a single consumer serializes mutations, so revisions arrive
    // monotonically and each notification carries the revision it caused.
    Store store{printing_device(at_ms(0))};

    ObserverRecord record;
    ASSERT_TRUE(store.add_observer(record_observer, &record));

    std::uint32_t previous = store.revision();

    const PrinterState sequence[] = {PrinterState::Paused, PrinterState::Printing,
                                     PrinterState::Attention, PrinterState::Finished};

    std::uint32_t generation = 1;
    std::int64_t moment = 1000;

    for (const auto value : sequence) {
        ++generation;

        const auto outcome = store.dispatch(
            Event::printer_snapshot(at_ms(moment), printer_snapshot(value, generation)));

        EXPECT_TRUE(outcome.accepted);
        EXPECT_GT(outcome.revision, previous);
        EXPECT_EQ(record.last_revision, outcome.revision);

        previous = outcome.revision;
        moment += 1000;
    }

    EXPECT_EQ(store.statistics().dispatched, 4U);
}

TEST(StoreTests, HonoursTheReducerPolicyItWasBuiltWith) {
    // The store fixes the policy for its lifetime, so two dispatches can never be
    // reduced under different rules.
    ReducerPolicy fast;
    fast.freshness.fresh_within = std::chrono::milliseconds{100};
    fast.freshness.aging_within = std::chrono::milliseconds{200};
    fast.freshness.stale_within = std::chrono::milliseconds{300};

    Store store{fast};

    store.dispatch(Event::printer_snapshot(at_ms(0), printer_snapshot(PrinterState::Printing, 1)));
    EXPECT_EQ(store.state().confidence, prd::app_model::Confidence::Current);

    // 250 ms is inside this policy's STALE window (200-300) and would still be Fresh
    // under the default 4 s one, so reaching Degraded here proves the custom policy is
    // the one in force. It was 150 ms until 2026-08-13, when Aging became Current.
    store.dispatch(Event::confidence_aged(at_ms(250)));
    EXPECT_EQ(store.state().confidence, prd::app_model::Confidence::Degraded);

    store.dispatch(Event::confidence_aged(at_ms(400)));
    EXPECT_EQ(store.state().confidence, prd::app_model::Confidence::Unavailable);
}

TEST(StoreTests, MatchesADirectReductionOfTheSameSequence) {
    // The store adds ownership and publication, not behaviour. Reducing by hand and
    // dispatching through the store must land in exactly the same place.
    const AppState initial = printing_device(at_ms(0));

    const Event events[] = {
        Event::printer_snapshot(at_ms(1000), printer_snapshot(PrinterState::Paused, 2)),
        Event::button_gesture(at_ms(2000), ButtonGesture::VeryLongPress),
        Event::confidence_aged(at_ms(8000)),
        Event::modal_surface_dismissed(at_ms(9000)),
    };

    AppState expected = initial;
    for (const auto& event : events) {
        expected = prd::app_model::reduce(expected, event);
    }

    Store store{initial};
    for (const auto& event : events) {
        store.dispatch(event);
    }

    EXPECT_EQ(store.state(), expected);
}

TEST(StoreTests, AutonomouslyAgesOnlyAtCrossedBoundariesOfItsPolicy) {
    using namespace prd::app_model;
    using std::chrono::milliseconds;
    ReducerPolicy custom;
    custom.freshness = {milliseconds{5}, milliseconds{10}, milliseconds{15}};
    prd::app_store::Store store{custom};
    (void)store.dispatch(Event::printer_snapshot(
        prd::test::at_ms(5), prd::test::printer_snapshot(PrinterState::Idle, 1)));
    const auto dispatches = store.statistics().dispatched;
    store.age_observation_if_due(prd::test::at_ms(10));
    EXPECT_EQ(store.statistics().dispatched, dispatches);
    store.age_observation_if_due(prd::test::at_ms(11));
    EXPECT_EQ(store.state().printer_connection.freshness, DataFreshness::Aging);
    EXPECT_EQ(store.statistics().dispatched, dispatches + 1U);
    store.age_observation_if_due(prd::test::at_ms(11));
    store.age_observation_if_due(prd::test::at_ms(9));
    EXPECT_EQ(store.statistics().dispatched, dispatches + 1U);
    store.age_observation_if_due(prd::test::at_ms(16));
    EXPECT_EQ(store.state().confidence, Confidence::Degraded);
    store.age_observation_if_due(prd::test::at_ms(21));
    EXPECT_EQ(store.state().confidence, Confidence::Unavailable);
}

TEST(StoreTests, RetainedCompletionAgesAfterWifiLossOrBodylessPrinterReport) {
    using namespace prd::app_model;
    const auto last_body = at_ms(1240000);
    for (const auto phase :
         {ConnectionPhase::Connected, ConnectionPhase::Probing, ConnectionPhase::Unconfigured}) {
        SCOPED_TRACE(log_label(phase));
        Store store{printing_device(last_body)};
        NetworkObservation network;
        network.phase = NetworkPhase::Connected;
        store.dispatch(Event::network_observed(last_body, network));
        store.dispatch(Event::display_preferences(
            last_body, {DisplayMode::PrinterStatus, Backlight::Standard}));
        store.dispatch(
            Event::printer_snapshot(last_body, printer_snapshot(PrinterState::Finished, 2)));
        store.dispatch(Event::display_idle_timer_fired(last_body));
        ASSERT_EQ(store.state().subject, SubjectState::Completed);
        ASSERT_EQ(store.state().display.power, DisplayPower::Dimmed);

        if (phase == ConnectionPhase::Connected) {
            network.phase = NetworkPhase::Unavailable;
            store.dispatch(Event::network_observed(last_body + at_ms(9000), network));
            EXPECT_EQ(store.state().printer_connection.last_attempt_at, last_body);
        } else {
            ConnectionObservation report;
            report.phase = phase;
            store.dispatch(Event::connection_observed(last_body + at_ms(9000), report));
        }
        ASSERT_TRUE(store.state().display.observation_interrupted);
        EXPECT_EQ(store.state().printer_connection.phase, phase);

        store.age_observation_after_ordinary_sleep(last_body + at_ms(15000));
        EXPECT_EQ(store.state().confidence, Confidence::Current);
        store.age_observation_after_ordinary_sleep(last_body + at_ms(15100));
        EXPECT_TRUE(project(store.state()).stale_indicator);
        EXPECT_EQ(store.state().confidence, Confidence::Degraded);
        store.age_observation_after_ordinary_sleep(last_body + at_ms(60000));
        EXPECT_EQ(store.state().confidence, Confidence::Degraded);
        store.age_observation_after_ordinary_sleep(last_body + at_ms(60100));
        const auto expired = project(store.state());
        EXPECT_EQ(expired.confidence, Confidence::Unavailable);
        EXPECT_TRUE(expired.observation_lost_acknowledgement);
        EXPECT_FALSE(expired.progress.known);
        EXPECT_FALSE(expired.nozzle.current.known);
        store.age_observation_after_ordinary_sleep(last_body + at_ms(68000));
        EXPECT_EQ(project(store.state()).observation_lost_generation,
                  expired.observation_lost_generation);
        EXPECT_EQ(store.state().printer.observed_at, last_body);
    }
}

TEST(StoreTests, RetainedCompletionUsesBodyAgeDespiteFailedAndBodylessReports) {
    using namespace prd::app_model;
    const auto last_body = at_ms(1240000);
    for (const auto phase : {ConnectionPhase::InvalidResponse, ConnectionPhase::PrinterUnavailable,
                             ConnectionPhase::Degraded, ConnectionPhase::Connected}) {
        for (const bool repeats : {false, true}) {
            SCOPED_TRACE(log_label(phase));
            SCOPED_TRACE(repeats);
            Store store{printing_device(last_body)};
            store.dispatch(Event::display_preferences(
                last_body, {DisplayMode::PrinterStatus, Backlight::Standard}));
            store.dispatch(
                Event::printer_snapshot(last_body, printer_snapshot(PrinterState::Finished, 2)));
            store.dispatch(Event::display_idle_timer_fired(last_body));
            ASSERT_EQ(store.state().subject, SubjectState::Completed);
            ASSERT_EQ(store.state().display.power, DisplayPower::Dimmed);

            ConnectionObservation report;
            report.phase = phase;
            for (int time = 9000; time <= 64000; time += 100) {
                if (time == 9000 || (repeats && time % 9000 == 0)) {
                    store.dispatch(Event::connection_observed(last_body + at_ms(time), report));
                    EXPECT_EQ(store.state().printer_connection.last_attempt_at,
                              last_body + at_ms(time));
                }
                store.age_observation_after_ordinary_sleep(last_body + at_ms(time));
                const auto view = project(store.state());
                EXPECT_EQ(view.confidence, time <= 15000   ? Confidence::Current
                                           : time <= 60000 ? Confidence::Degraded
                                                           : Confidence::Unavailable);
                EXPECT_EQ(view.stale_indicator, time > 15000 && time <= 60000);
                EXPECT_EQ(store.state().printer.observed_at, last_body);
            }
            report.phase = ConnectionPhase::Connected;
            store.dispatch(Event::connection_observed(last_body + at_ms(65000), report));
            EXPECT_EQ(store.state().confidence, Confidence::Unavailable);
            store.dispatch(Event::printer_snapshot(last_body + at_ms(66000),
                                                   printer_snapshot(PrinterState::Idle, 3)));
            EXPECT_EQ(store.state().confidence, Confidence::Current);
            EXPECT_EQ(store.state().subject, SubjectState::Ready);
            EXPECT_FALSE(project(store.state()).stale_indicator);
        }
    }
}

TEST(StoreTests, TimerStillUsesOrdinaryTransportRetryGrace) {
    using namespace prd::app_model;
    Store store{printing_device(at_ms(0))};
    store.dispatch(Event::printer_snapshot(at_ms(0), printer_snapshot(PrinterState::Finished, 2)));
    ConnectionObservation report;
    report.phase = ConnectionPhase::PrinterUnavailable;
    for (std::int64_t time = 9000; time <= 63000; time += 9000) {
        store.age_observation_after_ordinary_sleep(at_ms(time - 100));
        EXPECT_EQ(store.state().confidence,
                  time <= 18000 ? Confidence::Current : Confidence::Degraded);
        store.dispatch(Event::connection_observed(at_ms(time), report));
        EXPECT_EQ(store.state().confidence, time == 9000    ? Confidence::Current
                                            : time == 63000 ? Confidence::Unavailable
                                                            : Confidence::Degraded);
    }
}
