// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_store/store.hpp"

namespace prd::app_store {

Store::Store(const app_model::AppState& initial) : state_{initial} {}

Store::Store(const app_model::ReducerPolicy& policy) : policy_{policy} {}

app_model::DataFreshness Store::observation_freshness_at(const app_model::Timestamp now) const {
    if (!state_.printer.ever_observed) {
        return app_model::DataFreshness::Unknown;
    }
    return app_model::classify_freshness(now - state_.printer.observed_at, policy_.freshness);
}

void Store::age_observation_if_due(const app_model::Timestamp now) {
    if (state_.printer.ever_observed && now > state_.last_event_at &&
        observation_freshness_at(now) != state_.printer_connection.freshness) {
        (void)dispatch(app_model::Event::confidence_aged(now));
    }
}

void Store::age_observation_after_ordinary_sleep(const app_model::Timestamp now) {
    using app_model::AccessCondition;
    using app_model::ConnectionPhase;
    using app_model::DataFreshness;
    const auto freshness = observation_freshness_at(now);
    if (freshness != DataFreshness::Stale && freshness != DataFreshness::Expired) {
        return;
    }

    if (state_.display.preferences.mode == app_model::DisplayMode::PrinterStatus &&
        state_.subject == app_model::SubjectState::Completed &&
        state_.access_condition == AccessCondition::None) {
        // Once Timer would be Off, the retained completion follows the last usable body.
        // Transport retries and version-only replies cannot extend either age boundary.
        age_observation_if_due(now);
        return;
    }

    if (state_.display.observation_interrupted) {
        const auto phase = state_.printer_connection.phase;
        if (phase != ConnectionPhase::PrinterUnavailable &&
            phase != ConnectionPhase::InvalidResponse && phase != ConnectionPhase::Degraded) {
            return;
        }
        // Preserve the ordinary retry grace outside the retained completion.
        const auto attempt_age = app_model::classify_freshness(
            now - state_.printer_connection.last_attempt_at, policy_.freshness);
        if (app_model::confidence_from(attempt_age, true) == app_model::Confidence::Current) {
            return;
        }
    }

    age_observation_if_due(now);
}

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
