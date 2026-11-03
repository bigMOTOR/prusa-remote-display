// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_model/app_state.hpp"
#include "app_model/events.hpp"
#include "app_model/reducer.hpp"

// The single authoritative application state — Architecture Spec §5.2, §24.11.
//
// The store owns the state, runs the reducer, and publishes revisions. It owns no
// policy: what an event means is the reducer's business, and it is passed in so a
// test can substitute its own thresholds.
//
// SERIALIZATION. §5.2 requires all state mutations to be serialized, and §10.2
// provides that by construction: a single consumer task owns the store and "this
// task provides actor-like serialization similar to a single-threaded store". The
// store therefore takes no lock and contains no FreeRTOS primitive — which is
// also what keeps it host-testable and free of any ESP-IDF header.
//
// What it does enforce is the one way that arrangement can still break: an
// observer that dispatches while being notified. That is a real bug class — it
// would reduce a second event against a state that is still being published, and
// a consumer would see the two revisions out of order. Reentrant dispatch is
// rejected and counted rather than left to produce a state nobody can explain.
//
// NOT PRESENT, deliberately: no LVGL type, no networking type, no GPIO or
// hardware semantics, no std::function (an observer is a function pointer plus a
// context, so registration allocates nothing).

namespace prd::app_store {

struct DispatchOutcome {
    // False when the dispatch was refused — currently only on reentrancy. A
    // refused dispatch leaves the state untouched.
    bool accepted{false};

    // Whether the state actually changed. An event may be accepted and change
    // nothing: a duplicate observation, or telemetry that resolves to the same
    // product state (§7.1's "accepted state mutation").
    bool mutated{false};

    std::uint32_t revision{0};
};

class Store {
  public:
    // Notified after an accepted mutation, with the new state. Called on the
    // dispatching task; an observer must not dispatch (see the reentrancy note
    // above) and must not block.
    using ObserverFn = void (*)(const app_model::AppState& state, void* context);

    // §10.7 requires bounded queues and bounded structures generally. Four is
    // sized for the consumers the architecture names — the UI projection, the
    // diagnostics summary, persistence and one spare — and a fifth registration
    // fails loudly rather than growing an allocation.
    static constexpr std::size_t kMaxObservers = 4;

    struct Statistics {
        std::uint32_t dispatched{0};
        std::uint32_t mutations{0};
        std::uint32_t reentrant_rejections{0};
        std::uint32_t notifications{0};
        std::uint32_t observers_rejected{0};
    };

    Store() = default;

    // Starts from a given state rather than a default one. Used by the replay
    // tests, and by a later milestone restoring a persisted snapshot.
    explicit Store(const app_model::AppState& initial);

    // The policy the reducer runs under. Fixed for the store's lifetime so that
    // two dispatches can never be reduced under different rules.
    explicit Store(const app_model::ReducerPolicy& policy);

    [[nodiscard]] const app_model::AppState& state() const {
        return state_;
    }

    // A copy, for handing across a task boundary. §10.7: "no shared mutable
    // domain state across tasks" — a consumer on another task takes this, not a
    // reference into the store.
    [[nodiscard]] app_model::AppState snapshot() const {
        return state_;
    }

    [[nodiscard]] std::uint32_t revision() const {
        return state_.revision;
    }

    DispatchOutcome dispatch(const app_model::Event& event);

    // False when the table is full, or when called during a notification.
    bool add_observer(ObserverFn observer, void* context);

    [[nodiscard]] std::size_t observer_count() const {
        return observer_count_;
    }

    [[nodiscard]] Statistics statistics() const {
        return statistics_;
    }

  private:
    struct Observer {
        ObserverFn function{nullptr};
        void* context{nullptr};
    };

    app_model::AppState state_{};
    app_model::ReducerPolicy policy_{};

    Observer observers_[kMaxObservers]{};
    std::size_t observer_count_{0};

    bool dispatching_{false};
    Statistics statistics_{};
};

} // namespace prd::app_store
