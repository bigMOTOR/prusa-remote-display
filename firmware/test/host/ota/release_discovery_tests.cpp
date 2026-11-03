// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_discovery.hpp"

#include <gtest/gtest.h>

#include <string>

namespace prd::ota {
namespace {

std::string asset(const std::uint64_t id, const std::string_view name, const std::uint64_t size,
                  const std::string_view url) {
    return "{\"id\":" + std::to_string(id) + ",\"name\":\"" + std::string{name} +
           "\",\"size\":" + std::to_string(size) + ",\"browser_download_url\":\"" +
           std::string{url} + "\"}";
}

std::string release(const std::string_view assets,
                    const std::string_view flags = "\"draft\":false,\"prerelease\":false,"
                                                   "\"immutable\":true") {
    return "{\"id\":42,\"url\":\"https://api.github.com/repos/bigMOTOR/"
           "prusa-remote-display/releases/42\",\"tag_name\":\"v1.2.3\"," +
           std::string{flags} + ",\"assets\":[" + std::string{assets} + "]}";
}

std::string valid_release() {
    return release(asset(10, kReleaseManifestAsset, 1024,
                         "https://github.com/bigMOTOR/prusa-remote-display/releases/download/"
                         "v1.2.3/manifest.json") +
                   "," +
                   asset(11, kReleaseImageAsset, 1280000,
                         "https://github.com/bigMOTOR/prusa-remote-display/releases/download/"
                         "v1.2.3/prusa_remote_display.bin"));
}

TEST(ReleaseDiscoveryTests, DecodesExactAssetsFromOneImmutableStableRelease) {
    ReleaseCandidate candidate;
    ASSERT_EQ(decode_release(valid_release(), candidate), ReleaseStatus::Decoded);
    EXPECT_EQ(candidate.release_id, 42U);
    EXPECT_EQ(candidate.tag_view(), "v1.2.3");
    EXPECT_EQ(candidate.manifest.id, 10U);
    EXPECT_EQ(candidate.manifest.size, 1024U);
    EXPECT_EQ(candidate.image.id, 11U);
    EXPECT_EQ(candidate.image.size, 1280000U);
}

TEST(ReleaseDiscoveryTests, DraftPrereleaseAndMutableRecordsAreNotCandidates) {
    ReleaseCandidate candidate;
    EXPECT_EQ(decode_release(release("[]", "\"draft\":true,\"prerelease\":false,"
                                           "\"immutable\":true"),
                             candidate),
              ReleaseStatus::NotStable);
    EXPECT_EQ(decode_release(release("[]", "\"draft\":false,\"prerelease\":true,"
                                           "\"immutable\":true"),
                             candidate),
              ReleaseStatus::NotStable);
    EXPECT_EQ(decode_release(release("[]", "\"draft\":false,\"prerelease\":false,"
                                           "\"immutable\":false"),
                             candidate),
              ReleaseStatus::NotImmutable);
}

TEST(ReleaseDiscoveryTests, RepositoryAndReleaseIdentityMustAgree) {
    ReleaseCandidate candidate;
    std::string body = valid_release();
    body.replace(body.find("bigMOTOR"), 8, "attacker");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::WrongRepository);
    body = valid_release();
    body.replace(body.find("releases/42"), 11, "releases/41");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::WrongRepository);
}

TEST(ReleaseDiscoveryTests, BothExactAssetsAreRequiredExactlyOnce) {
    ReleaseCandidate candidate;
    const auto manifest = asset(10, kReleaseManifestAsset, 1024,
                                "https://github.com/bigMOTOR/prusa-remote-display/releases/"
                                "download/v1/manifest.json");
    const auto image = asset(11, kReleaseImageAsset, 1280000,
                             "https://github.com/bigMOTOR/prusa-remote-display/releases/"
                             "download/v1/prusa_remote_display.bin");
    EXPECT_EQ(decode_release(release(manifest), candidate), ReleaseStatus::MissingAsset);
    EXPECT_EQ(decode_release(release(manifest + "," + manifest + "," + image), candidate),
              ReleaseStatus::DuplicateAsset);
    EXPECT_EQ(decode_release(release(manifest + "," + image + "," + image), candidate),
              ReleaseStatus::DuplicateAsset);
}

TEST(ReleaseDiscoveryTests, AssetAuthorityPathAndDeclaredBoundsAreStrict) {
    ReleaseCandidate candidate;
    auto body = valid_release();
    body.replace(body.find("https://github.com"), 18, "https://evil.test");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::InvalidField);
    body = valid_release();
    const auto manifest_url_name = body.find("/manifest.json");
    ASSERT_NE(manifest_url_name, std::string::npos);
    body.replace(manifest_url_name + 1, 13, "manifest.json?q");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::InvalidField);
    body = valid_release();
    body.replace(body.find("1024"), 4, "8193");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::InvalidField);
    body = valid_release();
    body.replace(body.find("1280000"), 7, "6815745");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::InvalidField);
}

TEST(ReleaseDiscoveryTests, UnknownAssetsAreIgnoredButTheArrayCountIsBounded) {
    ReleaseCandidate candidate;
    auto body = valid_release();
    const auto position = body.find("\"assets\":[") + 10;
    body.insert(position, "{\"name\":\"notes.txt\"},");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::Decoded);

    std::string many;
    for (std::size_t index = 0; index < kMaximumReleaseAssets + 1; ++index) {
        if (!many.empty()) {
            many += ',';
        }
        many += "{\"name\":\"notes" + std::to_string(index) + ".txt\"}";
    }
    EXPECT_EQ(decode_release(release(many), candidate), ReleaseStatus::TooManyAssets);
}

TEST(ReleaseDiscoveryTests, MalformedOversizedAndWrongTypesClearOutput) {
    ReleaseCandidate candidate;
    ASSERT_EQ(decode_release(valid_release(), candidate), ReleaseStatus::Decoded);
    EXPECT_EQ(decode_release("{}", candidate), ReleaseStatus::InvalidField);
    EXPECT_EQ(candidate.release_id, 0U);
    EXPECT_EQ(decode_release(std::string(kReleaseBodyLimit + 1, ' '), candidate),
              ReleaseStatus::TooLarge);
    EXPECT_EQ(decode_release(release("1"), candidate), ReleaseStatus::Malformed);
}

TEST(ReleaseDiscoveryTests, RelevantDuplicateAndEscapedAliasMembersAreRejected) {
    ReleaseCandidate candidate;
    std::string body = valid_release();
    body.replace(body.find("\"id\":42"), 7, "\"id\":42,\"i\\u0064\":42");
    EXPECT_EQ(decode_release(body, candidate), ReleaseStatus::InvalidField);
}

} // namespace
} // namespace prd::ota
