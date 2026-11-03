// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "app_model/display_policy.hpp"
#include "app_store/store.hpp"

namespace prd::test {
// Test clock adapter for app_core::display_tick's event ordering. Calls the real policy and
// reducer, but is NOT the ESP task, backlight driver or hardware. Call before the instrument,
// as app_core does. Never shortens a product timer or advances the Store beyond supplied now.
class DisplayTimer {
  public:
    void tick(app_store::Store& store, app_model::Timestamp now) {
        namespace policy = app_model::display_policy;
        const auto& state = store.state();
        const auto situation = policy::situation_of(state);
        const bool changed = !seen_ || situation != situation_;
        if (changed || interaction_ != state.display.last_interaction_at) {
            activity_ = now;
            interaction_ = state.display.last_interaction_at;
            situation_ = situation;
            seen_ = true;
        }
        if (changed && state.display.power != app_model::DisplayPower::On) {
            (void)store.dispatch(app_model::Event::display_wake(now));
            return;
        }
        const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - activity_);
        switch (policy::next_step(state.display.power, static_cast<std::uint32_t>(idle.count()),
                                  policy::holds_awake(state), policy::Timings{})) {
        case policy::Step::Dim:
            (void)store.dispatch(app_model::Event::display_idle_timer_fired(now));
            break;
        case policy::Step::Sleep:
            (void)store.dispatch(app_model::Event::display_sleep_timer_fired(now));
            break;
        case policy::Step::Hold:
            break;
        }
    }

  private:
    app_model::display_policy::Situation situation_{};
    app_model::Timestamp activity_{}, interaction_{};
    bool seen_{false};
};
} // namespace prd::test
