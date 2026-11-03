// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_store/store.hpp"

namespace prd::app_store {

Store::Store(const app_model::AppState& initial) : state_{initial} {}

Store::Store(const app_model::ReducerPolicy& policy) : policy_{policy} {}

DispatchOutcome Store::dispatch(const app_model::Event& event) {
    DispatchOutcome outcome;

    // An observer dispatching from inside its notification would reduce against a
    // state that is still being published, and consumers would then be notified
    // in an order that does not match the revisions they receive. Refused, and
    // counted so it cannot be mistaken for a lost event.
    if (dispatching_) {
        ++statistics_.reentrant_rejections;
        outcome.revision = state_.revision;
        return outcome;
    }

    ++statistics_.dispatched;

    const std::uint32_t previous_revision = state_.revision;

    dispatching_ = true;
    state_ = app_model::reduce(state_, event, policy_);

    outcome.accepted = true;
    outcome.mutated = state_.revision != previous_revision;
    outcome.revision = state_.revision;

    if (outcome.mutated) {
        ++statistics_.mutations;

        // Only an accepted mutation notifies. §7.1 ties publication to a revision
        // increase, and UI Behaviour Decision 001 §4 requires routine polling and
        // retries not to become visible — so an event that changed nothing wakes
        // nobody.
        for (std::size_t index = 0; index < observer_count_; ++index) {
            const Observer& observer = observers_[index];

            if (observer.function != nullptr) {
                ++statistics_.notifications;
                observer.function(state_, observer.context);
            }
        }
    }

    dispatching_ = false;

    return outcome;
}

bool Store::add_observer(const ObserverFn observer, void* const context) {
    if (observer == nullptr) {
        ++statistics_.observers_rejected;
        return false;
    }

    // Registering from inside a notification would mutate the table being walked.
    if (dispatching_) {
        ++statistics_.observers_rejected;
        return false;
    }

    if (observer_count_ >= kMaxObservers) {
        ++statistics_.observers_rejected;
        return false;
    }

    observers_[observer_count_].function = observer;
    observers_[observer_count_].context = context;
    ++observer_count_;

    return true;
}

} // namespace prd::app_store
