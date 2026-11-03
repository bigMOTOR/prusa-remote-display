// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_schedule.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace prd::ota {
namespace {

TEST(ReleaseScheduleTests, StartsSpreadAcrossTheFirstHour) {
    ReleaseCheckSchedule minimum;
    ReleaseCheckSchedule maximum;
    minimum.start({1000, 0});
    maximum.start({1000, std::numeric_limits<std::uint32_t>::max()});
    EXPECT_EQ(minimum.due_ms(), 1000U);
    EXPECT_EQ(maximum.due_ms(), 1000U + kReleaseCheckJitterWindowMs);
}

TEST(ReleaseScheduleTests, RepeatChecksNeverOccurBeforeTwentyFourHours) {
    ReleaseCheckSchedule schedule;
    schedule.checked({5000, 0});
    EXPECT_FALSE(schedule.due(5000 + kReleaseCheckIntervalMs - 1));
    EXPECT_TRUE(schedule.due(5000 + kReleaseCheckIntervalMs));
    schedule.checked({9000, std::numeric_limits<std::uint32_t>::max()});
    EXPECT_EQ(schedule.due_ms(), 9000U + kReleaseCheckIntervalMs + kReleaseCheckJitterWindowMs);
}

TEST(ReleaseScheduleTests, ResetDisarmsAndArithmeticSaturates) {
    ReleaseCheckSchedule schedule;
    schedule.checked({std::numeric_limits<std::uint64_t>::max() - 100, 0});
    EXPECT_EQ(schedule.due_ms(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(schedule.due(std::numeric_limits<std::uint64_t>::max() - 1));
    EXPECT_TRUE(schedule.due(std::numeric_limits<std::uint64_t>::max()));
    schedule.reset();
    EXPECT_FALSE(schedule.armed());
    EXPECT_FALSE(schedule.due(std::numeric_limits<std::uint64_t>::max()));
}

} // namespace
} // namespace prd::ota
