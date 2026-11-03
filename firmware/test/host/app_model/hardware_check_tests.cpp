// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "app_model/hardware_check.hpp"

using namespace prd::app_model;

TEST(CapacityCheckTests, MatchingDetectionPasses) {
    const auto check = check_capacity("flash", 16777216U, 16777216U);

    EXPECT_EQ(check.result, ValidationState::Validated);
    EXPECT_EQ(validation_result_label(check.result), "pass");
}

TEST(CapacityCheckTests, MismatchedDetectionFails) {
    const auto check = check_capacity("psram", 8388608U, 2097152U);

    EXPECT_EQ(check.result, ValidationState::Failed);
    EXPECT_EQ(validation_result_label(check.result), "fail");
}

TEST(CapacityCheckTests, AbsentDetectionRemainsUnvalidated) {
    const auto check = check_capacity("flash", 16777216U, std::nullopt);

    EXPECT_EQ(check.result, ValidationState::Unknown);
    EXPECT_EQ(validation_result_label(check.result), "unvalidated");
}

TEST(CapacityCheckTests, FormatsLogLineForDetectedValue) {
    const auto check = check_capacity("flash", 16777216U, 16777216U);

    EXPECT_EQ(to_log_line(check), "flash_detected=16777216 expected=16777216 result=pass");
}

TEST(CapacityCheckTests, FormatsLogLineForUnavailableDetection) {
    const auto check = check_capacity("psram", 8388608U, std::nullopt);

    EXPECT_EQ(to_log_line(check), "psram_detected=unavailable expected=8388608 result=unvalidated");
}
