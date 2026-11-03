// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdint>

#include <gtest/gtest.h>

#include "wifi_log_policy.hpp"

// The Wi-Fi stack's log level — Milestone 3.11 T19.
//
// ⛔ WHAT THESE PROTECT. Architecture Spec §13.2 places `wifiSsid` inside SecretConfigV1 and §13.5
// forbids credentials in logs. At Info the ESP-IDF Wi-Fi stack prints the associated SSID and the
// router's BSSID on every connection, so the level this firmware leaves it at IS the contract. T19
// found the line in 8 of 19 bench captures before this policy existed.
//
// ⬅️ WHY THE LEVEL AND NOT THE CALL IS TESTED HERE. The call is one ESP-IDF line in radio.cpp,
// which cannot compile in this build; what can go wrong without anyone noticing is the VALUE — a
// developer raising it to Info to debug a radio problem and leaving it there. That is the regression
// these tests exist for. The numeric mapping to ESP-IDF's own enum is pinned by static_asserts in
// radio.cpp, where the real header is in scope, and the placement of the call relative to
// esp_wifi_init() is verified against the pinned v5.5.4 source rather than asserted here.

namespace prd::network::wifi_log_policy {
namespace {

constexpr Level kAllLevels[] = {Level::None,    Level::Error, Level::Warning,
                                Level::Verbose, Level::Info,  Level::Debug};

} // namespace

TEST(WifiLogPolicyTests, TheAppliedLevelCannotLogCredentials) {
    // ⛔ THE ONE THAT MATTERS. §13.5, stated as an assertion about the value the firmware ships.
    EXPECT_FALSE(emits_credentials(kApplied));
}

TEST(WifiLogPolicyTests, TheAppliedLevelStillReportsFaults) {
    // Owner requirement 2: the correction must not buy silence. A stack that reports nothing at all
    // would satisfy the rule above and be a worse product.
    EXPECT_TRUE(reports_faults(kApplied));
}

TEST(WifiLogPolicyTests, InfoAndEverythingAboveItEmitsCredentials) {
    // Where the association line starts, and that it is inherited by the more verbose levels.
    EXPECT_TRUE(emits_credentials(Level::Info));
    EXPECT_TRUE(emits_credentials(Level::Debug));
    EXPECT_TRUE(emits_credentials(Level::Verbose));
}

TEST(WifiLogPolicyTests, NothingBelowInfoEmitsCredentials) {
    EXPECT_FALSE(emits_credentials(Level::None));
    EXPECT_FALSE(emits_credentials(Level::Error));
    EXPECT_FALSE(emits_credentials(Level::Warning));
}

TEST(WifiLogPolicyTests, OnlyWarningAndAboveReportsFaults) {
    EXPECT_FALSE(reports_faults(Level::None));
    EXPECT_FALSE(reports_faults(Level::Error));
    EXPECT_TRUE(reports_faults(Level::Warning));
    EXPECT_TRUE(reports_faults(Level::Info));
}

TEST(WifiLogPolicyTests, ExactlyOneLevelSatisfiesBothObligations) {
    // ⬅️ STATED AS A PROPERTY, WHICH IS WHY IT IS WORTH A TEST. "Keeps faults, drops credentials"
    // has exactly one solution in this enum, so the applied value is not a preference that could be
    // adjusted without breaking one half or the other — and this is what would fail if a future
    // change tried to satisfy §13.5 by silencing the radio entirely.
    int satisfying = 0;
    Level found = Level::None;

    for (const Level level : kAllLevels) {
        if (!emits_credentials(level) && reports_faults(level)) {
            ++satisfying;
            found = level;
        }
    }

    EXPECT_EQ(satisfying, 1);
    EXPECT_EQ(found, kApplied);
}

TEST(WifiLogPolicyTests, TheEnumMirrorsTheEspIdfNumbering) {
    // The adapter static_asserts these against ESP-IDF's wifi_log_level_t. Restating them here
    // means a renumbering shows up as a named failure in this suite too, rather than only as a
    // firmware build error on a machine with the toolchain installed.
    EXPECT_EQ(static_cast<std::uint8_t>(Level::None), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Level::Error), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(Level::Warning), 2);
    EXPECT_EQ(static_cast<std::uint8_t>(Level::Info), 3);
    EXPECT_EQ(static_cast<std::uint8_t>(Level::Debug), 4);
    EXPECT_EQ(static_cast<std::uint8_t>(Level::Verbose), 5);
}

} // namespace prd::network::wifi_log_policy
