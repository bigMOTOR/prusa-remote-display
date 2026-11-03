// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_bundle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace prd::ota {
namespace {

std::string manifest(const std::string_view version = "1.2.3", const std::uint64_t size = 1280000) {
    return "{\"schemaVersion\":1,\"boardTarget\":\"board\",\"chipTarget\":\"esp32s3\","
           "\"productVersion\":\"" +
           std::string{version} +
           "\",\"compatibility\":{\"minBootloaderSchema\":1},\"image\":{\"sizeBytes\":" +
           std::to_string(size) +
           ",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}}";
}

ReleaseCandidate release(const std::string_view tag, const std::size_t manifest_size,
                         const std::uint64_t image_size = 1280000) {
    ReleaseCandidate candidate;
    std::copy(tag.begin(), tag.end(), candidate.tag.begin());
    candidate.manifest.size = manifest_size;
    candidate.image.size = image_size;
    return candidate;
}

TEST(ReleaseBundleTests, AcceptsOnlyOneCoherentNewerCompatibleReleaseBundle) {
    const std::string body = manifest();
    const ReleaseCandidate candidate = release("v1.2.3", body.size());
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 2000000};
    DecodedManifest decoded;

    EXPECT_EQ(validate_release_bundle(candidate, body, device, decoded),
              ReleaseBundleStatus::Accepted);
    EXPECT_EQ(decoded.candidate().version, "1.2.3");
    EXPECT_EQ(decoded.candidate().image_size, candidate.image.size);
}

TEST(ReleaseBundleTests, RefusesLengthTagImageCurrentAndCompatibilityMismatches) {
    const std::string body = manifest();
    const DeviceCompatibility device{"board", "esp32s3", "1.2.2", 1, 2000000};
    DecodedManifest decoded;

    EXPECT_EQ(validate_release_bundle(release("v1.2.3", body.size() + 1), body, device, decoded),
              ReleaseBundleStatus::ManifestLengthMismatch);
    EXPECT_EQ(validate_release_bundle(release("v9.9.9", body.size()), body, device, decoded),
              ReleaseBundleStatus::TagMismatch);
    EXPECT_EQ(validate_release_bundle(release("v1.2.3", body.size(), 7), body, device, decoded),
              ReleaseBundleStatus::AssetLengthMismatch);

    const DeviceCompatibility current{"board", "esp32s3", "1.2.3", 1, 2000000};
    EXPECT_EQ(validate_release_bundle(release("v1.2.3", body.size()), body, current, decoded),
              ReleaseBundleStatus::NotNewer);

    const DeviceCompatibility wrong{"other", "esp32s3", "1.2.2", 1, 2000000};
    EXPECT_EQ(validate_release_bundle(release("v1.2.3", body.size()), body, wrong, decoded),
              ReleaseBundleStatus::Incompatible);
}

} // namespace
} // namespace prd::ota
