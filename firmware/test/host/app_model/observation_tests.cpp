// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <chrono>

#include "app_model/observation.hpp"

// The freshness classification is the input to confidence, and confidence is what
// decides whether the product may present printer state as current. §11.5's
// boundaries are therefore pinned exactly, including on the boundary values.

using prd::app_model::classify_freshness;
using prd::app_model::DataFreshness;
using prd::app_model::FreshnessPolicy;
using prd::app_model::Timestamp;

namespace {

constexpr Timestamp age_ms(const std::int64_t milliseconds) {
    return std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{milliseconds});
}

} // namespace

TEST(FreshnessTests, ZeroAgeIsFresh) {
    EXPECT_EQ(classify_freshness(Timestamp::zero(), FreshnessPolicy{}), DataFreshness::Fresh);
}

TEST(FreshnessTests, ClassifiesEachBand) {
    const FreshnessPolicy policy;

    EXPECT_EQ(classify_freshness(age_ms(1), policy), DataFreshness::Fresh);
    EXPECT_EQ(classify_freshness(age_ms(5000), policy), DataFreshness::Aging);
    EXPECT_EQ(classify_freshness(age_ms(30000), policy), DataFreshness::Stale);
    EXPECT_EQ(classify_freshness(age_ms(120000), policy), DataFreshness::Expired);
}

TEST(FreshnessTests, BoundariesAreInclusiveOfTheYoungerBand) {
    const FreshnessPolicy policy;

    EXPECT_EQ(classify_freshness(policy.fresh_within, policy), DataFreshness::Fresh);
    EXPECT_EQ(classify_freshness(policy.fresh_within + age_ms(1), policy), DataFreshness::Aging);

    EXPECT_EQ(classify_freshness(policy.aging_within, policy), DataFreshness::Aging);
    EXPECT_EQ(classify_freshness(policy.aging_within + age_ms(1), policy), DataFreshness::Stale);

    EXPECT_EQ(classify_freshness(policy.stale_within, policy), DataFreshness::Stale);
    EXPECT_EQ(classify_freshness(policy.stale_within + age_ms(1), policy), DataFreshness::Expired);
}

TEST(FreshnessTests, NegativeAgeIsTreatedAsFreshRatherThanAncient) {
    // A timestamp that moved backwards must not blank the screen. The reducer
    // counts the disorder separately; the safe classification here is the youngest.
    EXPECT_EQ(classify_freshness(age_ms(-5000), FreshnessPolicy{}), DataFreshness::Fresh);
}

TEST(FreshnessTests, HonoursACallerSuppliedPolicy) {
    // The thresholds are provisional until polling is validated against a real
    // printer, so they must be substitutable rather than compiled in.
    FreshnessPolicy fast;
    fast.fresh_within = std::chrono::milliseconds{100};
    fast.aging_within = std::chrono::milliseconds{200};
    fast.stale_within = std::chrono::milliseconds{300};

    EXPECT_EQ(classify_freshness(age_ms(50), fast), DataFreshness::Fresh);
    EXPECT_EQ(classify_freshness(age_ms(150), fast), DataFreshness::Aging);
    EXPECT_EQ(classify_freshness(age_ms(250), fast), DataFreshness::Stale);
    EXPECT_EQ(classify_freshness(age_ms(350), fast), DataFreshness::Expired);
}
