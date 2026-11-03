// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "../../support/display_timer.hpp"
#include "app_model/presentation.hpp"
#include "fake_source/scenarios.hpp"
#include "loss_review.hpp"
#include <array>
#include <gtest/gtest.h>

namespace prd::validation {
namespace {
using namespace std::chrono_literals;
using Phase = LossReview::Phase;
using app_model::Confidence;
using app_model::DisplayPower;
using app_model::Timestamp;

struct Rig {
    app_store::Store store;
    test::DisplayTimer display;
    LossReview run;
    Timestamp now{100s};
    explicit Rig(std::uint32_t manual = 0) {
        constexpr auto boot = fake_source::Scenario::ColdBootToReady;
        for (std::size_t i = 0; i < fake_source::step_count(boot); ++i)
            (void)store.dispatch(fake_source::step_at(boot, i, Timestamp{}, 0).event);
        run.begin(store, now, {1, 0}, manual);
    }
    void tick(bool with_display = true) {
        now += 100ms;
        if (with_display)
            display.tick(store, now);
        run.tick(store, now);
    }
    void until(Phase target, std::size_t limit = 5000, bool with_display = true) {
        for (std::size_t i = 0; i < limit && run.phase() != target && run.phase() != Phase::Failed;
             ++i)
            tick(with_display);
    }
};

TEST(LossReview, FourRealTimeChaptersReachEveryWindowAndRefreshFinalReadyForTenMinutes) {
    Rig r;
    std::array<int, 4> loss{}, recovered{}, stale{}, off{};
    std::array<Timestamp, 4> loss_at{}, seed_at{};
    constexpr std::array<Timestamp, 4> delay{3500ms, 400ms, 20600ms, 26000ms};
    seed_at[0] = r.now;
    bool was_off = false;
    for (int tick = 0; tick < 4000 && r.run.phase() != Phase::FinalReady; ++tick) {
        const auto prior = r.run.phase();
        r.tick();
        ASSERT_NE(r.run.phase(), Phase::Failed);
        ASSERT_LE(r.store.state().last_event_at, r.now);
        const auto chapter = r.run.chapter();
        if (chapter >= 4)
            continue;
        const auto p = app_model::project(r.store.state());
        if (r.run.phase() == Phase::AwaitLoss && prior == Phase::Seed)
            seed_at[chapter] = r.now;
        if (p.stale_indicator)
            ++stale[chapter]; // Rejected Degraded treatment is exercised, NOT hidden or accepted.
        if (r.run.phase() == Phase::Loss && prior != Phase::Loss) {
            ++loss[chapter];
            loss_at[chapter] = r.now;
            EXPECT_EQ(r.now - seed_at[chapter], 60100ms);
            EXPECT_TRUE(p.observation_lost_acknowledgement);
        }
        if (r.run.phase() == Phase::OffFade) {
            ++off[chapter];
            EXPECT_EQ(r.store.state().display.power, DisplayPower::Off);
            was_off = true;
        }
        if (r.run.phase() == Phase::Recovered && prior != Phase::Recovered) {
            ++recovered[chapter];
            EXPECT_EQ(r.now - loss_at[chapter], delay[chapter]);
            EXPECT_FALSE(p.observation_lost);
            EXPECT_EQ(r.store.state().printer.observed_at, r.now);
        }
        if (r.run.phase() == Phase::Recovered && prior == Phase::Recovered && was_off) {
            EXPECT_EQ(r.store.state().display.power, DisplayPower::On);
            was_off = false;
        }
    }
    ASSERT_EQ(r.run.phase(), Phase::FinalReady);
    EXPECT_EQ(r.run.recoveries(), 4U);
    EXPECT_TRUE(r.run.unattended());
    EXPECT_EQ(loss, (std::array<int, 4>{1, 1, 1, 1}));
    EXPECT_EQ(recovered, loss);
    for (int count : stale)
        EXPECT_GT(count, 400);
    EXPECT_EQ(off[0] + off[1] + off[2], 0);
    EXPECT_EQ(off[3], 30);
    for (int i = 0; i < 6000; ++i) {
        r.tick();
        const auto p = app_model::project(r.store.state());
        ASSERT_EQ(p.subject, app_model::SubjectState::Ready);
        ASSERT_EQ(p.confidence, Confidence::Current);
        ASSERT_FALSE(p.stale_indicator || p.observation_lost);
        ASSERT_LE(r.store.state().last_event_at, r.now);
    }
    EXPECT_EQ(r.store.state().display.power, DisplayPower::Off);
}

TEST(LossReview, NoExpiryAtSixtySecondsAndNoBackdatedObservation) {
    Rig r;
    const auto seed = r.store.state().printer.observed_at;
    for (int i = 0; i < 600; ++i)
        r.tick();
    EXPECT_EQ(r.run.phase(), Phase::AwaitLoss);
    EXPECT_EQ(r.store.state().confidence, Confidence::Degraded);
    EXPECT_EQ(r.store.state().printer.observed_at, seed);
    r.tick();
    EXPECT_EQ(r.run.phase(), Phase::Loss);
    EXPECT_EQ(r.store.state().printer.observed_at, seed);
}

TEST(LossReview, MissingDisplayRetirementFailsInsteadOfCertifyingATailRecovery) {
    Rig r;
    r.run.skip(r.store, r.now);
    r.run.skip(r.store, r.now); // Enter tail chapter, deliberately omit app_core display timer.
    r.until(Phase::Failed, 1000, false);
    EXPECT_EQ(r.run.phase(), Phase::Failed);
    EXPECT_EQ(r.run.recoveries(), 0U);
    EXPECT_FALSE(r.run.unattended());
}

TEST(LossReview, SkippedOrPreviouslyManuallySteppedRunCannotClaimUnattended) {
    Rig skipped;
    for (int i = 0; i < 4; ++i)
        skipped.run.skip(skipped.store, skipped.now);
    EXPECT_EQ(skipped.run.phase(), Phase::FinalReady);
    EXPECT_EQ(skipped.run.recoveries(), 0U);
    EXPECT_FALSE(skipped.run.unattended());
    Rig manual{1};
    manual.until(Phase::FinalReady);
    EXPECT_EQ(manual.run.phase(), Phase::FinalReady);
    EXPECT_EQ(manual.run.recoveries(), 4U);
    EXPECT_FALSE(manual.run.unattended());
}

TEST(LossReview, WaitsForRealClockToReachPriorEventFloor) {
    Rig r;
    const auto future = r.now + 2s;
    (void)r.store.dispatch(app_model::Event::confidence_aged(future));
    const auto prior = r.store.state();
    r.run.begin(r.store, r.now, {2, 0});
    EXPECT_EQ(r.run.phase(), Phase::Seed);
    EXPECT_EQ(r.store.state(), prior);
    r.run.tick(r.store, future);
    EXPECT_EQ(r.run.phase(), Phase::AwaitLoss);
    EXPECT_EQ(r.store.state().printer.observed_at, future);
}

TEST(LossReview, RapidSkipsWhileWaitingForThePreludeClockCannotWalkPastTheFinalChapter) {
    Rig r;
    const auto future = r.now + 2s;
    (void)r.store.dispatch(app_model::Event::confidence_aged(future));
    r.run.begin(r.store, r.now, {2, 0});
    ASSERT_EQ(r.run.phase(), Phase::Seed);
    for (int i = 0; i < 100; ++i)
        r.run.skip(r.store, r.now);
    EXPECT_EQ(r.run.chapter(), LossReview::kChapterCount);
    EXPECT_EQ(r.run.phase(), Phase::Seed);
    EXPECT_EQ(r.run.recoveries(), 0U);
    EXPECT_FALSE(r.run.unattended());
    r.run.tick(r.store, future);
    EXPECT_EQ(r.run.phase(), Phase::FinalReady);
    EXPECT_EQ(r.store.state().printer.observed_at, future);
}

TEST(LossReview, InterruptedOffFailsAndADuplicateSequencedObservationCannotPass) {
    Rig r;
    for (int i = 0; i < 3; ++i)
        r.run.skip(r.store, r.now);
    r.until(Phase::OffFade);
    ASSERT_EQ(r.run.phase(), Phase::OffFade);
    (void)r.store.dispatch(app_model::Event::display_wake(r.now));
    r.until(Phase::Failed);
    EXPECT_EQ(r.run.recoveries(), 0U);
    Rig lease;
    lease.until(Phase::Recovered);
    ASSERT_EQ(lease.run.phase(), Phase::Recovered);
    ASSERT_EQ(lease.store.state().last_printer_sequence, 1U);
    lease.run.begin(lease.store, lease.now + 1s, {1, 1});
    EXPECT_EQ(lease.run.phase(), Phase::Failed);
}
} // namespace
} // namespace prd::validation
