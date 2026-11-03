// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "backoff.hpp"

#include <cstdint>

#include <gtest/gtest.h>

// §11.3's reconnect ladder.
//
// THE NUMBERS ARE THE SOURCE'S. "Bounded exponential backoff with jitter ... 2 s → 4 s →
// 8 s → 15 s → 30 s maximum" is quoted from §11.3, so these tests pin the specification
// rather than a preference — a change that breaks one is a divergence from an accepted
// source, not a tuning decision.

namespace {

using prd::network::delay_for;
using prd::network::Jitter;
using prd::network::kSchedule;
using prd::network::schedule_saturated;

// The midpoint jitter byte. It lands the delay ON its step to within a fraction of a
// percent — not exactly, because 128/255 is a hair above one half and the band is divided
// with integer arithmetic. The tolerance below is that rounding, not slack.
constexpr prd::network::Jitter kNoJitter{128};

// Comfortably inside the gap between any two steps, so these tests still pin WHICH step a
// delay came from.
[[nodiscard]] double rounding_tolerance(const std::int64_t base) {
    return static_cast<double>(base) / 100.0 + 2.0;
}

TEST(NetworkBackoffTests, TheLadderIsTheOneTheSpecificationGives) {
    EXPECT_EQ(kSchedule[0].count(), 2000);
    EXPECT_EQ(kSchedule[1].count(), 4000);
    EXPECT_EQ(kSchedule[2].count(), 8000);
    EXPECT_EQ(kSchedule[3].count(), 15000);
    EXPECT_EQ(kSchedule[4].count(), 30000);
}

// Attempt 0 is the first connect, not a retry.
TEST(NetworkBackoffTests, TheFirstAttemptWaitsNotAtAll) {
    EXPECT_EQ(delay_for(0, kNoJitter).count(), 0);
    EXPECT_EQ(delay_for(0, Jitter{0}).count(), 0);
    EXPECT_EQ(delay_for(0, Jitter{255}).count(), 0);
}

TEST(NetworkBackoffTests, EachAttemptClimbsItsStep) {
    for (std::uint32_t attempt = 1; attempt <= kSchedule.size(); ++attempt) {
        const auto expected = kSchedule[attempt - 1].count();

        EXPECT_NEAR(static_cast<double>(delay_for(attempt, kNoJitter).count()),
                    static_cast<double>(expected), rounding_tolerance(expected))
            << "attempt " << attempt;
    }
}

TEST(NetworkBackoffTests, TheLadderStrictlyClimbsUntilItSaturates) {
    for (std::uint32_t attempt = 2; attempt <= kSchedule.size(); ++attempt) {
        EXPECT_GT(delay_for(attempt, kNoJitter).count(), delay_for(attempt - 1, kNoJitter).count())
            << "attempt " << attempt;
    }
}

// "30 s maximum" — the ladder is bounded, so a device failing for hours still retries every
// half minute rather than drifting toward never.
TEST(NetworkBackoffTests, TheLadderHoldsAtItsMaximum) {
    const auto maximum = kSchedule.back().count();

    for (const std::uint32_t attempt : {5U, 6U, 20U, 1000U, 0xFFFFFFFFU}) {
        EXPECT_NEAR(static_cast<double>(delay_for(attempt, kNoJitter).count()),
                    static_cast<double>(maximum), rounding_tolerance(maximum))
            << "attempt " << attempt;
    }
}

// ±12.5 % — an implementation value, recorded as such. It exists so several devices on one
// network do not retry in lockstep after a router restart.
TEST(NetworkBackoffTests, JitterStaysWithinAnEighthEitherWay) {
    for (std::uint32_t attempt = 1; attempt <= kSchedule.size() + 2; ++attempt) {
        const std::size_t index =
            attempt - 1 < kSchedule.size() ? attempt - 1 : kSchedule.size() - 1;
        const auto base = kSchedule[index].count();
        const auto bound = base / 8;

        for (const Jitter jitter : {Jitter{0}, Jitter{64}, kNoJitter, Jitter{192}, Jitter{255}}) {
            const auto delay = delay_for(attempt, jitter).count();

            EXPECT_GE(delay, base - bound - 1) << "attempt " << attempt;
            EXPECT_LE(delay, base + bound + 1) << "attempt " << attempt;
        }
    }
}

TEST(NetworkBackoffTests, JitterActuallyVaries) {
    EXPECT_NE(delay_for(1, Jitter{0}).count(), delay_for(1, Jitter{255}).count());
    EXPECT_LT(delay_for(1, Jitter{0}).count(), delay_for(1, Jitter{255}).count());
}

// A delay is never zero or negative for a real retry — a zero would turn backoff into a
// spin, which is exactly what §16.2 forbids.
TEST(NetworkBackoffTests, NoRetryDelayIsEverZero) {
    for (std::uint32_t attempt = 1; attempt <= 10; ++attempt) {
        for (const Jitter jitter : {Jitter{0}, kNoJitter, Jitter{255}}) {
            EXPECT_GT(delay_for(attempt, jitter).count(), 0) << "attempt " << attempt;
        }
    }
}

TEST(NetworkBackoffTests, SaturationIsReachedAtTheLastStep) {
    EXPECT_FALSE(schedule_saturated(0));
    EXPECT_FALSE(schedule_saturated(1));
    EXPECT_FALSE(schedule_saturated(kSchedule.size() - 1));
    EXPECT_TRUE(schedule_saturated(kSchedule.size()));
    EXPECT_TRUE(schedule_saturated(kSchedule.size() + 1));
}

} // namespace
