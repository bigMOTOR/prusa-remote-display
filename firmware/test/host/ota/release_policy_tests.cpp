// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_discovery.hpp"
#include "ota/release_policy.hpp"

#include <gtest/gtest.h>

namespace prd::ota {
namespace {

constexpr std::string_view kManifest =
    "https://github.com/bigMOTOR/prusa-remote-display/releases/download/v1.2.3/manifest.json";
constexpr std::string_view kImage =
    "https://github.com/bigMOTOR/prusa-remote-display/releases/download/v1.2.3/"
    "prusa_remote_display.bin";
constexpr std::string_view kCdn =
    "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?sp=r";

TEST(ReleasePolicyTests, InitialRoutesAreExactAndHttpsOnly) {
    EXPECT_TRUE(release_url_allowed(ReleaseResource::Metadata, release_discovery_url()));
    EXPECT_TRUE(release_url_allowed(ReleaseResource::Manifest, kManifest));
    EXPECT_TRUE(release_url_allowed(ReleaseResource::Image, kImage));
    EXPECT_FALSE(release_url_allowed(ReleaseResource::Metadata,
                                     "http://api.github.com/repos/bigMOTOR/"
                                     "prusa-remote-display/releases/latest"));
    EXPECT_FALSE(release_url_allowed(ReleaseResource::Manifest, kImage));
    EXPECT_FALSE(release_url_allowed(ReleaseResource::Image, kManifest));
}

TEST(ReleasePolicyTests, AuthorityUserinfoPortQueryAndEncodedSeparatorsAreRejected) {
    EXPECT_FALSE(release_url_allowed(
        ReleaseResource::Manifest,
        "https://github.com.evil.test/bigMOTOR/prusa-remote-display/releases/download/v1/"
        "manifest.json"));
    EXPECT_FALSE(release_url_allowed(
        ReleaseResource::Manifest,
        "https://user@github.com/bigMOTOR/prusa-remote-display/releases/download/v1/"
        "manifest.json"));
    EXPECT_FALSE(release_url_allowed(
        ReleaseResource::Manifest,
        "https://github.com:443/bigMOTOR/prusa-remote-display/releases/download/v1/"
        "manifest.json"));
    EXPECT_FALSE(
        release_url_allowed(ReleaseResource::Manifest, std::string{kManifest} + "?download=1"));
    EXPECT_FALSE(release_url_allowed(
        ReleaseResource::Manifest,
        "https://github.com/bigMOTOR/prusa-remote-display/releases/download/v1%2Fother/"
        "manifest.json"));
}

TEST(ReleasePolicyTests, OnlyOneAssetToReleaseCdnRedirectIsAllowed) {
    ReleaseRoute manifest{ReleaseResource::Manifest};
    EXPECT_TRUE(release_url_policy(&manifest, {}, kManifest, 0));
    EXPECT_TRUE(release_url_policy(&manifest, kManifest, kCdn, 1));
    EXPECT_FALSE(release_url_policy(&manifest, kManifest, kCdn, 2));
    EXPECT_FALSE(release_url_policy(&manifest, {}, kCdn, 0));
    EXPECT_FALSE(release_url_policy(&manifest, kManifest,
                                    "https://objects.githubusercontent.com/file?token=x", 1));
    EXPECT_FALSE(release_url_policy(nullptr, {}, kManifest, 0));
}

TEST(ReleasePolicyTests, MetadataNeverRedirects) {
    ReleaseRoute metadata{ReleaseResource::Metadata};
    EXPECT_TRUE(release_url_policy(&metadata, {}, release_discovery_url(), 0));
    EXPECT_FALSE(release_url_policy(&metadata, release_discovery_url(), kCdn, 1));
}

} // namespace
} // namespace prd::ota
