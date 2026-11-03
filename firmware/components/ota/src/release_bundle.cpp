// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#include "ota/release_bundle.hpp"

namespace prd::ota {

ReleaseBundleStatus validate_release_bundle(const ReleaseCandidate& release,
                                            const std::string_view manifest_body,
                                            const DeviceCompatibility& device,
                                            DecodedManifest& manifest) {
    manifest = {};
    if (release.manifest.size != manifest_body.size()) {
        return ReleaseBundleStatus::ManifestLengthMismatch;
    }
    if (decode_manifest(manifest_body, manifest) != ManifestStatus::Decoded) {
        return ReleaseBundleStatus::ManifestInvalid;
    }

    const Candidate candidate = manifest.candidate();
    if (candidate.image_size != release.image.size) {
        manifest = {};
        return ReleaseBundleStatus::AssetLengthMismatch;
    }

    std::string_view tag = release.tag_view();
    if (!tag.empty() && tag.front() == 'v') {
        tag.remove_prefix(1);
    }
    if (tag != candidate.version) {
        manifest = {};
        return ReleaseBundleStatus::TagMismatch;
    }
    if (candidate.version == device.version) {
        manifest = {};
        return ReleaseBundleStatus::NotNewer;
    }
    if (check_compatibility(candidate, device, DowngradePolicy::Reject) !=
        Compatibility::MetadataCompatible) {
        manifest = {};
        return ReleaseBundleStatus::Incompatible;
    }
    return ReleaseBundleStatus::Accepted;
}

} // namespace prd::ota
