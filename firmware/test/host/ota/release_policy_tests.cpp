// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_discovery.hpp"
#include "ota/release_policy.hpp"

#include <gtest/gtest.h>

#include <string>

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

// Public GitHub CDN parameter shape; signature/JWT are deliberately fake, not
// a captured temporary URL. This is a policy fixture, never a download address.
constexpr std::string_view kSignedCdn =
    "https://release-assets.githubusercontent.com/github-production-release-asset/123/abc?"
    "sp=r&sv=2018-11-09&sr=b&spr=https&se=2026-11-04T15%3A00%3A00Z&"
    "rscd=attachment%3B+filename%3Dmanifest.json&rsct=application%2Foctet-stream&"
    "skt=2026-11-04T14%3A00%3A00Z&ske=2026-11-04T15%3A00%3A00Z&"
    "sks=b&skv=2018-11-09&sig=FAKE%2Fsignature%2fonly%3D&jwt=FAKE.JWT.TOKEN&"
    "response-content-disposition=attachment%3B%20filename%3Dmanifest.json&"
    "response-content-type=application%2Foctet-stream";

TEST(ReleasePolicyTests, SignedCdnQueryAllowsEncodedSlashesForBothAssetRoutesOnly) {
    ReleaseRoute manifest{ReleaseResource::Manifest};
    ReleaseRoute image{ReleaseResource::Image};
    EXPECT_TRUE(release_url_policy(&manifest, kManifest, kSignedCdn, 1));
    EXPECT_TRUE(release_url_policy(&image, kImage, kSignedCdn, 1));
    EXPECT_FALSE(release_url_policy(&manifest, {}, kSignedCdn, 0));
    EXPECT_FALSE(release_url_policy(&manifest, kManifest, kSignedCdn, 2));
    EXPECT_FALSE(release_url_policy(&manifest, kSignedCdn, kSignedCdn, 1));
    EXPECT_FALSE(release_url_allowed(ReleaseResource::Manifest, kSignedCdn));
}

TEST(ReleasePolicyTests, QueryExceptionDoesNotRelaxPathAuthorityOrRawStructure) {
    ReleaseRoute route{ReleaseResource::Manifest};
    const std::string prefix =
        "https://release-assets.githubusercontent.com/github-production-release-asset/";
    for (const auto* path : {"123%2Fabc", "123%2fabc", "123%5Cabc", "123%5cabc", "123/../abc",
                             "123/./abc", "123\\abc"}) {
        EXPECT_FALSE(release_url_policy(&route, kManifest, prefix + path + "?sig=fake%2F", 1))
            << path;
    }
    for (const auto* authority : {"http://release-assets.githubusercontent.com/",
                                  "https://release-assets.githubusercontent.com.evil.test/",
                                  "https://user@release-assets.githubusercontent.com/",
                                  "https://release-assets.githubusercontent.com@evil.test/",
                                  "https://release-assets.githubusercontent.com:443/"}) {
        EXPECT_FALSE(release_url_policy(
            &route, kManifest,
            std::string{authority} + "github-production-release-asset/123/abc?sig=fake%2F", 1))
            << authority;
    }
    for (const auto* query : {"", "sig=fake#fragment", "sig=user@host", "sig=fake%5C",
                              "sig=fake%5c", "sig=fake\\value", "sig=/../", "sig=/./"}) {
        EXPECT_FALSE(release_url_policy(&route, kManifest, prefix + "123/abc?" + query, 1))
            << query;
    }
    for (unsigned value = 0; value <= 0x7fU; ++value) {
        if (value > 0x20U && value != 0x7fU) {
            continue;
        }
        std::string url{kSignedCdn};
        url.push_back(static_cast<char>(value));
        EXPECT_FALSE(release_url_policy(&route, kManifest, url, 1)) << value;
    }
}

TEST(ReleasePolicyTests, MetadataNeverRedirects) {
    ReleaseRoute metadata{ReleaseResource::Metadata};
    EXPECT_TRUE(release_url_policy(&metadata, {}, release_discovery_url(), 0));
    EXPECT_FALSE(release_url_policy(&metadata, release_discovery_url(), kCdn, 1));
}

} // namespace
} // namespace prd::ota
