// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_model/observation.hpp"
#include "app_store/store.hpp"
#include "fake_source/scenarios.hpp"

// The fake application data source.
//
// It drives the real store, through real typed events, and it can do nothing else.
// There is no method here that writes state: the only thing this class does to the
// store is call dispatch(). That is deliberate and is the property the phase is
// for — when the PrusaLink adapter replaces it in Milestone 4, the store, the
// reducer and every scene behind them cannot tell the difference, because there was
// never a second way in.
//
// Deterministic, and free of any clock. `advance()` steps by index. `advance_to()`
// takes the current time as an argument, so the caller owns the clock: on the target
// that is platform::monotonic_time(), and in a host test it is a number.
//
// Controls for bench work: pause, resume, single-step, and restart. Restart is a
// genuine re-run rather than a rewind — see the generation note on reset().

namespace prd::fake_source {

class FakeSource {
  public:
    struct Advance {
        // Whether a step was dispatched. False when paused, finished, or when the
        // next step is not due yet.
        bool applied{false};

        // Whether the store's revision changed. An applied step may legitimately
        // change nothing a consumer can see.
        bool mutated{false};

        std::uint32_t revision{0};

        // The index of the step that was applied.
        std::size_t step_index{0};

        StepLabel label;
    };

    FakeSource(app_store::Store& store, Scenario scenario, app_model::Timestamp base);

    // Switches scenario and restarts from its first step. Advances the generation,
    // so observation sequences stay ahead of anything already dispatched.
    void load(Scenario scenario, app_model::Timestamp base);

    // Restarts the current scenario from its first step at a new base time.
    //
    // The generation advances, which offsets every observation sequence in the
    // replay. Without that, §8.1's stale-response rule would reject the whole second
    // run: its sequences would be no greater than the ones already accepted. A
    // restart is therefore a re-run at a later time with fresh generations, which is
    // also the only kind of restart a real device could have — its history only
    // moves forward.
    void reset(app_model::Timestamp base);

    void pause();
    void resume();

    [[nodiscard]] Scenario scenario() const {
        return scenario_;
    }

    [[nodiscard]] bool paused() const {
        return paused_;
    }

    [[nodiscard]] bool finished() const {
        return next_index_ >= step_count();
    }

    [[nodiscard]] std::size_t step_index() const {
        return next_index_;
    }

    [[nodiscard]] std::size_t step_count() const {
        return fake_source::step_count(scenario_);
    }

    [[nodiscard]] app_model::Timestamp base() const {
        return base_;
    }

    // How many times this source has been loaded or restarted. Drives the sequence
    // offset; also useful in a log, so a bench observer can tell one pass from the
    // next.
    [[nodiscard]] std::uint32_t generation() const {
        return generation_;
    }

    [[nodiscard]] std::uint32_t steps_applied() const {
        return steps_applied_;
    }

    // The sequence offset the current generation is running under. Public because a
    // driver that wants to re-read a step it has already dispatched — to label it in a
    // log, say — needs the same offset step_at() was called with.
    [[nodiscard]] std::uint32_t sequence_base() const;

    // The next step without dispatching it. An out-of-range index yields a step
    // with an empty label, which is how `finished()` reads from the outside.
    [[nodiscard]] Step peek() const;

    // Dispatches the next step regardless of its scheduled time. This is the
    // single-step control, and what a button press drives on the bench.
    Advance advance();

    // Dispatches every step whose scheduled time has arrived. Returns how many.
    // Bounded by the remaining step count, so it always terminates.
    std::size_t advance_to(app_model::Timestamp now);

    struct Sweep {
        std::size_t steps_applied{0};

        // The scenario reached its end on this call. Reported once, not on every
        // subsequent call, so a driver can log a completion line exactly once.
        bool just_finished{false};

        // The next scenario in the catalogue was loaded on this call.
        bool loaded_next{false};
    };

    // The unattended sweep policy: apply what is due, hold the terminal presentation
    // for `dwell`, then load the next scenario in the catalogue and keep going.
    //
    // PAUSE HOLDS EVERYTHING, INCLUDING THE BOUNDARY. That rule lives here rather
    // than in the caller because putting it in the caller is exactly what went wrong:
    // the validation flow checked paused() before advancing but not before loading
    // the next scenario, and load() deliberately clears the pause flag — so a press
    // that should have resumed instead paused a freshly loaded scenario, and from the
    // bench the toggle silently did not toggle. It took a finger on the button at the
    // exact moment a scenario ended to find it.
    //
    // Owning the policy here makes it host-testable against the real implementation
    // rather than against a copy of it in a test.
    Sweep sweep(app_model::Timestamp now, std::chrono::milliseconds dwell);

  private:
    app_store::Store& store_;
    Scenario scenario_;
    app_model::Timestamp base_;

    std::size_t next_index_{0};
    std::uint32_t generation_{0};
    std::uint32_t steps_applied_{0};
    bool paused_{false};

    // Sweep bookkeeping: whether the terminal presentation is currently being held,
    // and since when. Cleared by load(), because a new scenario is not dwelling.
    bool dwelling_{false};
    app_model::Timestamp finished_at_{};
};

} // namespace prd::fake_source
