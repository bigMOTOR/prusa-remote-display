// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace prd::ota {

#ifndef PRD_OTA_RELEASE_OWNER
#define PRD_OTA_RELEASE_OWNER "bigMOTOR"
#endif
#ifndef PRD_OTA_RELEASE_NAME
#define PRD_OTA_RELEASE_NAME "prusa-remote-display"
#endif

inline constexpr std::string_view kReleaseOwner = PRD_OTA_RELEASE_OWNER;
inline constexpr std::string_view kReleaseRepository = PRD_OTA_RELEASE_NAME;
// Resolved inside the OTA component, so a consumer cannot see a different URL
// when hardware-validation compile flags are private to this component.
[[nodiscard]] std::string_view release_discovery_url() noexcept;
inline constexpr std::string_view kReleaseManifestAsset = "manifest.json";
inline constexpr std::string_view kReleaseImageAsset = "prusa_remote_display.bin";
inline constexpr std::size_t kReleaseBodyLimit = 32768;
inline constexpr std::size_t kReleaseUrlCapacity = 768;
inline constexpr std::size_t kReleaseTagCapacity = 128;
inline constexpr std::size_t kMaximumReleaseAssets = 32;

struct ReleaseAsset {
    std::uint64_t id{0};
    std::uint64_t size{0};
    std::array<char, kReleaseUrlCapacity> url{};

    [[nodiscard]] std::string_view url_view() const;
};

// Both assets originate in the same immutable GitHub release object. IDs are
// retained so later worker/session binding can name the exact discovery result,
// rather than treating two matching filenames as identity.
struct ReleaseCandidate {
    std::uint64_t release_id{0};
    std::array<char, kReleaseTagCapacity> tag{};
    ReleaseAsset manifest;
    ReleaseAsset image;

    [[nodiscard]] std::string_view tag_view() const;
};

enum class ReleaseStatus : std::uint8_t {
    Decoded,
    TooLarge,
    Malformed,
    NotStable,
    NotImmutable,
    WrongRepository,
    InvalidField,
    MissingAsset,
    DuplicateAsset,
    TooManyAssets,
};

[[nodiscard]] std::string_view log_label(ReleaseStatus status);

// Decodes the body returned by release_discovery_url(). The release must be
// non-draft, non-prerelease and claim immutability; its selected endpoint identity
// and both exact assets are checked. The local fixture claim is not proof of
// GitHub release immutability. Unknown assets/fields are ignored within hard
// body, depth and asset-count limits. Failure clears output.
[[nodiscard]] ReleaseStatus decode_release(std::string_view body, ReleaseCandidate& output);

} // namespace prd::ota
