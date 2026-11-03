// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include "app_model/build_info.hpp"

using namespace prd::app_model;

TEST(SemanticVersionTests, ParsesReleaseVersion) {
    const auto version = SemanticVersion::parse("1.2.3");

    ASSERT_TRUE(version.has_value());

    EXPECT_EQ(version->major, 1U);
    EXPECT_EQ(version->minor, 2U);
    EXPECT_EQ(version->patch, 3U);
    EXPECT_TRUE(version->prerelease.empty());
}

TEST(SemanticVersionTests, ParsesPrereleaseVersion) {
    const auto version = SemanticVersion::parse("2.5.0-dev");

    ASSERT_TRUE(version.has_value());

    EXPECT_EQ(version->major, 2U);
    EXPECT_EQ(version->minor, 5U);
    EXPECT_EQ(version->patch, 0U);
    EXPECT_EQ(version->prerelease, "dev");
}

TEST(SemanticVersionTests, RejectsInvalidVersion) {
    EXPECT_FALSE(SemanticVersion::parse("1.2").has_value());
    EXPECT_FALSE(SemanticVersion::parse("1").has_value());
    EXPECT_FALSE(SemanticVersion::parse("abc").has_value());
    EXPECT_FALSE(SemanticVersion::parse("1.2.3.4").has_value());
}

TEST(BuildInfoTests, ProducesSummary) {
    BuildInfo info;

    info.version = *SemanticVersion::parse("1.0.0-dev");
    info.git_commit = "abcdef1";
    info.git_dirty = true;
    info.build_profile = "debug";
    info.idf_version = "v5.5.4";
    info.board_target = "esp32s3";

    const auto summary = info.summary();

    EXPECT_NE(summary.find("1.0.0-dev"), std::string::npos);
    EXPECT_NE(summary.find("abcdef1"), std::string::npos);
    EXPECT_NE(summary.find("debug"), std::string::npos);
    EXPECT_NE(summary.find("v5.5.4"), std::string::npos);
    EXPECT_NE(summary.find("esp32s3"), std::string::npos);
}
