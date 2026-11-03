// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "app_model/build_info.hpp"

// Bootstrap Spec §11.6, final item: "no credentials or runtime timestamps in
// reproducible build metadata". §14.5 states the rule the metadata must satisfy —
// it shall contain no usernames, absolute source paths, machine names, Wi-Fi
// information or credentials — and §14.3 forbids compiling local wall-clock time
// into a release binary.
//
// The subject under test is the metadata the firmware actually emits:
// BuildInfo::summary(), which is the single structured build line §14.5 requires
// at boot and the line that appears in every committed Milestone 1 evidence log.
//
// The detector lives here rather than in production code on purpose. Nothing in
// the firmware needs to inspect its own metadata at run time, so a production API
// for it would be dead code — the audit that closed Milestone 1 flagged exactly
// that pattern. What matters is that the invariant is checked, and that the check
// is proved capable of failing: every rule below has a paired negative case, so a
// detector that silently stopped matching would fail this suite rather than pass
// it quietly.

namespace {

struct Rule {
    std::string_view name;
    std::regex pattern;
};

// One rule per forbidden class in §14.5, plus §14.3's wall-clock rule.
std::vector<Rule> forbidden_rules() {
    std::vector<Rule> rules;

    // Absolute source paths, which are also how a username leaks in practice.
    rules.push_back({"macos-home-path", std::regex{R"(/Users/[^/ ]+)"}});
    rules.push_back({"linux-home-path", std::regex{R"(/home/[^/ ]+)"}});
    rules.push_back({"windows-abs-path", std::regex{R"([A-Za-z]:\\)"}});

    // Machine names as they appear on a local network.
    rules.push_back({"mdns-machine-name", std::regex{R"([A-Za-z0-9-]+\.local\b)"}});

    // Credentials and Wi-Fi identifiers.
    rules.push_back(
        {"credential-keyword",
         std::regex{R"((?:password|passwd|api[_-]?key|secret|token))", std::regex::icase}});
    rules.push_back({"wifi-ssid", std::regex{R"(\bssid\b)", std::regex::icase}});

    // §14.3: an ISO-8601 wall-clock stamp, which is what a build that compiled in
    // the local time of day would carry. A reproducible build carries a Unix epoch
    // taken from the Git commit instead, and never renders it as a date.
    rules.push_back({"wall-clock-timestamp", std::regex{R"(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2})"}});

    return rules;
}

// Returns the names of every rule the text violates.
std::vector<std::string> findings(const std::string& text) {
    std::vector<std::string> hits;

    for (const auto& rule : forbidden_rules()) {
        if (std::regex_search(text, rule.pattern)) {
            hits.emplace_back(rule.name);
        }
    }

    return hits;
}

// A BuildInfo shaped like the real one: the values from the committed
// Milestone 1 hardware-validation image.
prd::app_model::BuildInfo representative_build_info() {
    prd::app_model::BuildInfo info;
    info.version = prd::app_model::SemanticVersion{0, 1, 0, "dev"};
    info.git_commit = "c6b574efadfa";
    info.build_profile = "hardware-validation";
    info.idf_version = "v5.5.4";
    info.board_target = "waveshare_esp32_s3_lcd_2_8";
    info.git_dirty = false;
    return info;
}

} // namespace

// --- The invariant --------------------------------------------------------

TEST(BuildMetadataPurityTests, RepresentativeSummaryIsClean) {
    const auto summary = representative_build_info().summary();
    const auto hits = findings(summary);

    EXPECT_TRUE(hits.empty()) << "summary: " << summary;
    for (const auto& hit : hits) {
        ADD_FAILURE() << "violated rule: " << hit;
    }
}

TEST(BuildMetadataPurityTests, SummaryCarriesOnlyTheDeclaredFields) {
    // §14.5 is a rule about absence, so it is satisfied trivially by a summary
    // that says nothing. This pins what must be present, so the purity test above
    // cannot pass because the metadata went missing.
    const auto summary = representative_build_info().summary();

    EXPECT_NE(summary.find("version=0.1.0-dev"), std::string::npos) << summary;
    EXPECT_NE(summary.find("git=c6b574efadfa"), std::string::npos) << summary;
    EXPECT_NE(summary.find("dirty=false"), std::string::npos) << summary;
    EXPECT_NE(summary.find("profile=hardware-validation"), std::string::npos) << summary;
    EXPECT_NE(summary.find("idf=v5.5.4"), std::string::npos) << summary;
    EXPECT_NE(summary.find("board=waveshare_esp32_s3_lcd_2_8"), std::string::npos) << summary;
}

TEST(BuildMetadataPurityTests, DirtyTreeIsReportedRatherThanHidden) {
    // §14.4: debug and hardware-validation builds may use a dirty tree and must
    // report dirty=true. Absence of the flag would be a metadata defect, not
    // cleanliness.
    auto info = representative_build_info();
    info.git_dirty = true;

    const auto summary = info.summary();
    EXPECT_NE(summary.find("dirty=true"), std::string::npos) << summary;
    EXPECT_TRUE(findings(summary).empty()) << summary;
}

// --- Expected failure cases ----------------------------------------------
//
// Each of these injects one forbidden class into a metadata field and asserts the
// detector catches it. Without them the invariant test above could pass because
// the rules had quietly stopped matching anything.

TEST(BuildMetadataPurityTests, DetectsMacOsHomePathInAField) {
    auto info = representative_build_info();
    info.board_target = "/Users/someone/Developer/board";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "macos-home-path"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsLinuxHomePathInAField) {
    auto info = representative_build_info();
    info.build_profile = "/home/builder/ci/release";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "linux-home-path"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsWindowsAbsolutePathInAField) {
    auto info = representative_build_info();
    info.idf_version = R"(C:\esp\v5.5.4)";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "windows-abs-path"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsMachineNameInAField) {
    auto info = representative_build_info();
    info.git_commit = "built-on-some-macbook.local";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "mdns-machine-name"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsCredentialKeywordInAField) {
    auto info = representative_build_info();
    info.board_target = "board_with_api_key_embedded";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "credential-keyword"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsWifiIdentifierInAField) {
    auto info = representative_build_info();
    info.build_profile = "debug-ssid-workshop";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "wifi-ssid"), hits.end());
}

TEST(BuildMetadataPurityTests, DetectsWallClockTimestampInAField) {
    // The shape a non-reproducible build would take: local time of day compiled
    // into the metadata instead of the commit's Unix epoch.
    auto info = representative_build_info();
    info.build_profile = "release built 2026-08-05 11:24";

    const auto hits = findings(info.summary());
    EXPECT_NE(std::find(hits.begin(), hits.end(), "wall-clock-timestamp"), hits.end());
}

TEST(BuildMetadataPurityTests, ReproducibleEpochIsNotMistakenForWallClock) {
    // The converse, so the wall-clock rule cannot be satisfied by rejecting the
    // legitimate representation too. A Unix epoch from the Git commit is a bare
    // integer and must pass.
    auto info = representative_build_info();
    info.git_commit = "1785914392";

    const auto hits = findings(info.summary());
    EXPECT_TRUE(hits.empty()) << info.summary();
}
