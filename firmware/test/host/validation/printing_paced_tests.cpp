// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "app_model/display_policy.hpp"
#include "app_model/presentation.hpp"
#include "scenes/operational_content.hpp"
#include "validation/validation.hpp"
#include <chrono>
#include <gtest/gtest.h>

namespace prd::validation {
namespace {
using app_model::ButtonGesture;
app_model::Timestamp at(const std::int64_t ms) {
    return std::chrono::duration_cast<app_model::Timestamp>(std::chrono::milliseconds{ms});
}
TEST(PrintingPacedFlow, ScheduledDispatchUsesEighteenSecondClockNotSixSecondFixtureOffsets) {
    app_store::Store store;
    constexpr std::int64_t start = 1000000;
    printing_timeline_begin(store, at(start));
    for (std::int64_t step = 0; step < 17; ++step) {
        const auto now = at(start + step * 18000);
        if (step > 0) {
            const auto prior = store.state().printer.observed_at;
            printing_timeline_tick(store, now - at(1));
            EXPECT_EQ(store.state().printer.observed_at, prior);
        }
        printing_timeline_tick(store, now);
        if (step > 0) {
            EXPECT_EQ(store.state().printer.observed_at, now) << step;
        }
        if (step == 15) {
            // Exercise the product's real ageing event during the18s retired-job dwell.
            // The former backdated status could become Unavailable after only6s on glass.
            (void)store.dispatch(app_model::Event::confidence_aged(now + at(6000)));
            EXPECT_EQ(app_model::project(store.state()).subject,
                      app_model::SubjectState::Completed);
        }
    }
    EXPECT_EQ(app_model::project(store.state()).subject, app_model::SubjectState::Ready);
}
TEST(PrintingPacedFlow, PauseManualStepResumeAndRestartUseDispatchTime) {
    app_store::Store store;
    printing_timeline_begin(store, at(1000000));
    printing_timeline_tick(store, at(1000000));
    const auto initial = store.state().printer.observed_at;
    printing_timeline_on_gesture(ButtonGesture::LongPress);
    printing_timeline_tick(store, at(1001000));
    printing_timeline_tick(store, at(1060000));
    EXPECT_EQ(store.state().printer.observed_at, initial);
    printing_timeline_on_gesture(ButtonGesture::ShortPress);
    printing_timeline_tick(store, at(1060000));
    EXPECT_EQ(store.state().printer.observed_at, at(1060000));
    printing_timeline_tick(store, at(1090000));
    EXPECT_EQ(store.state().printer.observed_at, at(1060000));
    printing_timeline_on_gesture(ButtonGesture::LongPress);
    printing_timeline_tick(store, at(1090000));
    EXPECT_EQ(store.state().printer.observed_at, at(1090000));
    printing_timeline_on_gesture(ButtonGesture::VeryLongPress);
    printing_timeline_tick(store, at(1120000));
    printing_timeline_tick(store, at(1120000));
    printing_timeline_tick(store, at(1138000));
    EXPECT_EQ(store.state().printer.observed_at, at(1138000));
}
TEST(PrintingPacedFlow, RapidManualStepsDoNotInventFutureFixtureTime) {
    app_store::Store store;
    printing_timeline_begin(store, at(1000000));
    printing_timeline_tick(store, at(1000000));
    for (int press = 0; press < 4; ++press) {
        printing_timeline_on_gesture(ButtonGesture::ShortPress);
    }
    printing_timeline_tick(store, at(1005000));
    EXPECT_EQ(store.state().printer.observed_at, at(1005000));
    EXPECT_EQ(app_model::project(store.state()).subject, app_model::SubjectState::Printing);
}

void run_through(app_store::Store& store, const std::int64_t last_step) {
    constexpr std::int64_t start = 1000000;
    printing_timeline_begin(store, at(start));
    for (std::int64_t step = 0; step <= last_step; ++step) {
        printing_timeline_tick(store, at(start + step * 18000));
    }
}

TEST(PrintingPacedFlow, NoDispatchBetweenFifteenAndEighteenSecondsDoesNotReclassifyConfidence) {
    app_store::Store store;
    run_through(store, 15);
    const auto before = store.state();
    ASSERT_EQ(before.confidence, app_model::Confidence::Current);
    ASSERT_EQ(before.subject, app_model::SubjectState::Completed);
    for (const std::int64_t elapsed : {15000, 15001, 17900, 17999}) {
        printing_timeline_tick(store, before.printer.observed_at + at(elapsed));
        EXPECT_EQ(store.state(), before) << elapsed;
    }
    printing_timeline_tick(store, before.printer.observed_at + at(18000));
    EXPECT_EQ(store.state().printer.observed_at, before.printer.observed_at + at(18000));
    EXPECT_EQ(store.state().confidence, app_model::Confidence::Current);
    EXPECT_EQ(app_model::project(store.state()).subject, app_model::SubjectState::Ready);
}

TEST(PrintingPacedFlow, FinalReadyRemainsFreshThroughTenMinutesOfRealTimerEvents) {
    app_store::Store store;
    run_through(store, 16);
    const auto final = store.state().printer.observed_at;
    const auto interaction = store.state().display.last_interaction_at;
    for (std::int64_t elapsed = 1000; elapsed <= 600000; elapsed += 1000) {
        const auto now = final + at(elapsed);
        (void)store.dispatch(app_model::Event::display_idle_timer_fired(now));
        printing_timeline_tick(store, now);
        EXPECT_EQ(store.state().subject, app_model::SubjectState::Ready);
        EXPECT_EQ(store.state().confidence, app_model::Confidence::Current);
        EXPECT_FALSE(app_model::project(store.state()).stale_indicator);
        EXPECT_FALSE(ui::scene::resolve(app_model::project(store.state())).show_progress);
        EXPECT_EQ(store.state().display.last_interaction_at, interaction);
    }
}

TEST(PrintingPacedFlow, ExplicitAgeingEventReclassifiesTheSameFifteenToEighteenSecondWindow) {
    app_store::Store store;
    run_through(store, 15);
    const auto observed = store.state().printer.observed_at;
    (void)store.dispatch(app_model::Event::confidence_aged(observed + at(15000)));
    EXPECT_EQ(store.state().confidence, app_model::Confidence::Current);
    for (const std::int64_t elapsed : {15001, 17900}) {
        (void)store.dispatch(app_model::Event::confidence_aged(observed + at(elapsed)));
        EXPECT_EQ(store.state().last_event_at, observed + at(elapsed));
        EXPECT_EQ(store.state().printer.observed_at, observed);
        EXPECT_EQ(store.state().confidence, app_model::Confidence::Degraded);
        EXPECT_TRUE(app_model::project(store.state()).stale_indicator);
        EXPECT_EQ(app_model::project(store.state()).subject, app_model::SubjectState::Completed);
    }
    printing_timeline_tick(store, observed + at(18000));
    EXPECT_EQ(store.state().confidence, app_model::Confidence::Current);
    EXPECT_FALSE(app_model::project(store.state()).stale_indicator);
}

TEST(PrintingPacedFlow, PausedReadyAndCompletedAgeAtTheRealDisplayTimerEvent) {
    namespace policy = app_model::display_policy;
    for (const std::int64_t last_step : {0, 15}) {
        SCOPED_TRACE(last_step);
        app_store::Store store;
        run_through(store, last_step);
        const auto observed = store.state().printer.observed_at;
        const auto subject = store.state().subject;
        const auto interaction = store.state().display.last_interaction_at;
        ASSERT_FALSE(policy::holds_awake(store.state()));
        printing_timeline_on_gesture(ButtonGesture::LongPress);
        printing_timeline_tick(store, observed + at(1000));
        printing_timeline_tick(store, observed + at(17900));
        EXPECT_EQ(store.state().confidence, app_model::Confidence::Current);
        EXPECT_EQ(store.state().display.last_interaction_at, interaction);
        EXPECT_EQ(policy::next_step(store.state().display.power, 19999, false, policy::Timings{}),
                  policy::Step::Hold);
        ASSERT_EQ(policy::next_step(store.state().display.power, 20000, false, policy::Timings{}),
                  policy::Step::Dim);

        // app_core processes display_tick before the timeline tick. The real timer event,
        // unlike a paused no-dispatch tick, advances reducer time and derives confidence.
        (void)store.dispatch(app_model::Event::display_idle_timer_fired(observed + at(20000)));
        printing_timeline_tick(store, observed + at(20000));
        const auto degraded = app_model::project(store.state());
        EXPECT_EQ(store.state().last_event_at, observed + at(20000));
        EXPECT_EQ(store.state().printer.observed_at, observed);
        EXPECT_EQ(store.state().display.power, app_model::DisplayPower::Dimmed);
        EXPECT_EQ(degraded.confidence, app_model::Confidence::Degraded);
        EXPECT_EQ(degraded.subject, subject);
        EXPECT_TRUE(degraded.stale_indicator);
        EXPECT_FALSE(degraded.nozzle.current.known);
        EXPECT_FALSE(degraded.bed.current.known);

        // There is no autonomous confidence clock in the simulator. An explicit later event
        // can expire the observation; merely waiting past sixty seconds does not dispatch one.
        printing_timeline_tick(store, observed + at(60001));
        EXPECT_EQ(store.state().last_event_at, observed + at(20000));
        (void)store.dispatch(app_model::Event::confidence_aged(observed + at(60001)));
        EXPECT_EQ(store.state().confidence, app_model::Confidence::Unavailable);
        EXPECT_TRUE(app_model::project(store.state()).observation_lost);
        EXPECT_EQ(store.state().printer.observed_at, observed);
    }
}

TEST(PrintingPacedFlow, GestureRestartRetainsQueuedControlsAndEmittedTimeFloor) {
    app_store::Store store;
    run_through(store, 4);
    const auto observed = store.state().printer.observed_at;
    const auto generation = store.state().last_printer_generation;
    printing_timeline_on_gesture(ButtonGesture::ShortPress);
    printing_timeline_on_gesture(ButtonGesture::ShortPress);
    printing_timeline_on_gesture(ButtonGesture::LongPress);
    printing_timeline_on_gesture(ButtonGesture::VeryLongPress);
    printing_timeline_tick(store, observed);
    EXPECT_GT(store.state().last_event_at, observed);
    printing_timeline_tick(store, observed);
    EXPECT_EQ(store.state().last_printer_generation, generation + 1);
    EXPECT_EQ(store.state().last_printer_sequence, 1U);
    EXPECT_EQ(store.state().subject, app_model::SubjectState::Preparing);
    const auto after = store.state();
    printing_timeline_tick(store, observed + at(18000));
    EXPECT_EQ(store.state(), after); // The queued toggle paused the restarted run.
}

TEST(PrintingPacedFlow, BeginClearsQueuedControlsAndTimeFloorForANewStore) {
    app_store::Store old_store;
    run_through(old_store, 4);
    printing_timeline_on_gesture(ButtonGesture::ShortPress);
    printing_timeline_on_gesture(ButtonGesture::LongPress);
    printing_timeline_on_gesture(ButtonGesture::VeryLongPress);
    app_store::Store store;
    printing_timeline_begin(store, at(1000));
    printing_timeline_tick(store, at(1000));
    EXPECT_LT(store.state().printer.observed_at, old_store.state().printer.observed_at);
    EXPECT_EQ(store.state().last_printer_generation, 1U);
    EXPECT_EQ(store.state().last_printer_sequence, 0U);
    EXPECT_EQ(store.state().subject, app_model::SubjectState::Ready);
    printing_timeline_tick(store, at(19000));
    EXPECT_EQ(store.state().printer.observed_at, at(19000));
    EXPECT_EQ(store.state().subject, app_model::SubjectState::Preparing);
}
} // namespace
} // namespace prd::validation
