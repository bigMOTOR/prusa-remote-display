// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_policy.hpp"

#include "ota/release_discovery.hpp"

namespace prd::ota {
namespace {

#ifdef PRD_OTA_LAN_MODE
constexpr std::string_view kAssetPrefix = PRD_OTA_LAN_ORIGIN "/releases/download/";
#else
constexpr std::string_view kAssetPrefix =
    "https://github.com/" PRD_OTA_RELEASE_OWNER "/" PRD_OTA_RELEASE_NAME "/releases/download/";
#endif
#ifndef PRD_OTA_LAN_MODE
constexpr std::string_view kCdnPrefix =
    "https://release-assets.githubusercontent.com/github-production-release-asset/";
#endif

[[nodiscard]] bool contains_forbidden_structure(const std::string_view value) {
    if (value.find('@') != std::string_view::npos || value.find('#') != std::string_view::npos ||
        value.find('\\') != std::string_view::npos ||
        value.find("/../") != std::string_view::npos ||
        value.find("/./") != std::string_view::npos) {
        return true;
    }
    // Only the path gives an encoded slash routing meaning. GitHub's signed CDN
    // query legitimately includes %2F in MIME types and signatures. Keep every
    // other structural rejection over the complete URL, including the query.
    const auto path_end = value.find('?');
    for (const char raw : value) {
        const auto character = static_cast<unsigned char>(raw);
        if (character <= 0x20U || character == 0x7fU) {
            return true;
        }
    }
    for (std::size_t index = 0; index + 2 < value.size(); ++index) {
        if (value[index] != '%') {
            continue;
        }
        const char first = value[index + 1];
        const char second = value[index + 2];
        if ((index < path_end && first == '2' && (second == 'f' || second == 'F')) ||
            (first == '5' && (second == 'c' || second == 'C'))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool asset_url(const std::string_view url, const std::string_view asset) {
    if (url.size() <= kAssetPrefix.size() + asset.size() + 1 ||
        url.compare(0, kAssetPrefix.size(), kAssetPrefix) != 0 ||
        contains_forbidden_structure(url) || url.find('?') != std::string_view::npos) {
        return false;
    }
    const auto suffix = url.substr(url.size() - asset.size());
    const auto tag =
        url.substr(kAssetPrefix.size(), url.size() - kAssetPrefix.size() - asset.size());
    return suffix == asset && tag.size() > 1 && tag.back() == '/';
}

#ifndef PRD_OTA_LAN_MODE
[[nodiscard]] bool cdn_url(const std::string_view url) {
    if (url.size() <= kCdnPrefix.size() || url.compare(0, kCdnPrefix.size(), kCdnPrefix) != 0 ||
        contains_forbidden_structure(url)) {
        return false;
    }
    const auto query = url.find('?');
    return query != std::string_view::npos && query > kCdnPrefix.size() && query + 1 < url.size();
}
#endif

} // namespace

bool release_url_allowed(const ReleaseResource resource, const std::string_view url) {
    switch (resource) {
    case ReleaseResource::Metadata:
        return url == release_discovery_url();
    case ReleaseResource::Manifest:
        return asset_url(url, kReleaseManifestAsset);
    case ReleaseResource::Image:
        return asset_url(url, kReleaseImageAsset);
    }
    return false;
}

bool release_url_policy(void* context, const std::string_view previous,
                        const std::string_view candidate, const std::uint8_t redirect_count) {
    if (context == nullptr) {
        return false;
    }
    const auto resource = static_cast<ReleaseRoute*>(context)->resource;
    if (redirect_count == 0) {
        return previous.empty() && release_url_allowed(resource, candidate);
    }
    if (redirect_count != 1 || resource == ReleaseResource::Metadata ||
        !release_url_allowed(resource, previous)) {
        return false;
    }
#ifdef PRD_OTA_LAN_MODE
    return false;
#else
    return cdn_url(candidate);
#endif
}

} // namespace prd::ota
