// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>

#include "access_review.hpp"
#include "app_model/presentation.hpp"
#include "gtest/gtest.h"

namespace {
using namespace std::chrono_literals;
using namespace prd::app_model;

TEST(AccessReviewTests, ClassificationsCorrectionsAndRetriesUseTheRealReducer) {
    prd::app_store::Store store;
    prd::validation::AccessReview flow;
    Timestamp now{1s};
    flow.begin(store, now);
    const auto revision = store.revision();
    flow.begin(store, now);
    EXPECT_EQ(store.revision(), revision);
    std::uint32_t loss = 0;
    for (std::size_t step = 0; step < flow.kStepCount; ++step) {
        const auto view = project(store.state());
        ASSERT_EQ(flow.step(), step);
        if (step == 0 || step == 2 || step == 4 || step == 7) {
            EXPECT_TRUE(view.acquiring_connection);
        }
        if (step == 1) {
            EXPECT_EQ(view.access_condition, AccessCondition::AuthenticationRequired);
        }
        if (step == 3) {
            EXPECT_EQ(view.access_condition, AccessCondition::UnsupportedInterface);
        }
        if (step == 5) {
            ASSERT_TRUE(view.observation_lost);
            loss = view.observation_lost_generation;
        }
        if (step == 6) {
            EXPECT_FALSE(view.acquiring_connection);
            EXPECT_EQ(view.observation_lost_generation, loss);
        }
        if (step == 8) {
            EXPECT_TRUE(view.observation_lost);
            EXPECT_NE(view.observation_lost_generation, loss);
        }
        now += 18s;
        flow.tick(store, now);
    }
}

TEST(AccessReviewTests, FinalReadyNeverAgesWhileItsSourceKeepsRefreshing) {
    prd::app_store::Store store;
    prd::validation::AccessReview flow;
    flow.begin(store, 1s);
    for (int ms = 1100; ms <= 763000; ms += 100) {
        const Timestamp now{std::chrono::milliseconds{ms}};
        flow.tick(store, now);
        EXPECT_LE(store.state().last_event_at, now);
        if (flow.step() == flow.kStepCount - 1) {
            const auto view = project(store.state());
            ASSERT_EQ(view.subject, SubjectState::Ready);
            ASSERT_EQ(view.confidence, Confidence::Current);
            ASSERT_FALSE(view.stale_indicator);
            ASSERT_FALSE(view.observation_lost);
        }
    }
}
} // namespace
