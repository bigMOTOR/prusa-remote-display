// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>

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
