// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <string_view>

#include "ota/compatibility.hpp"
#include "ota/manifest.hpp"
#include "ota/release_discovery.hpp"

namespace prd::ota {

enum class ReleaseBundleStatus : std::uint8_t {
    Accepted,
    ManifestLengthMismatch,
    ManifestInvalid,
    AssetLengthMismatch,
    TagMismatch,
    NotNewer,
    Incompatible,
};

[[nodiscard]] ReleaseBundleStatus validate_release_bundle(const ReleaseCandidate& release,
                                                          std::string_view manifest_body,
                                                          const DeviceCompatibility& device,
                                                          DecodedManifest& manifest);

} // namespace prd::ota
