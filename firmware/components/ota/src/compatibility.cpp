// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/compatibility.hpp"

#include "app_model/build_info.hpp"

#include <algorithm>
#include <optional>
#include <tuple>

namespace prd::ota {
namespace {

bool digit(const char value) {
    return value >= '0' && value <= '9';
}

bool numeric(const std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), digit);
}

std::string_view take_identifier(std::string_view& value) {
    const auto dot = value.find('.');
    const auto result = value.substr(0, dot);
    value = dot == std::string_view::npos ? std::string_view{} : value.substr(dot + 1);
    return result;
}

bool valid_identifiers(std::string_view value, const bool prerelease) {
    if (value.empty() || value.back() == '.') {
        return false;
    }
    while (!value.empty()) {
        const auto part = take_identifier(value);
        if (part.empty() || !std::all_of(part.begin(), part.end(), [](const char character) {
                return digit(character) || (character >= 'A' && character <= 'Z') ||
                       (character >= 'a' && character <= 'z') || character == '-';
            })) {
            return false;
        }
        if (prerelease && numeric(part) && part.size() > 1 && part.front() == '0') {
            return false;
        }
    }
    return true;
}

// Reuse the project's numeric representation, with strict SemVer validation at
// this untrusted metadata boundary. Build metadata does not affect precedence.
std::optional<app_model::SemanticVersion> parse_version(std::string_view value) {
    const auto plus = value.find('+');
    if (plus != std::string_view::npos) {
        if (!valid_identifiers(value.substr(plus + 1), false)) {
            return std::nullopt;
        }
        value = value.substr(0, plus);
    }
    auto core = value.substr(0, value.find('-'));
    while (!core.empty()) {
        const auto part = take_identifier(core);
        if (!numeric(part) || (part.size() > 1 && part.front() == '0')) {
            return std::nullopt;
        }
    }
    auto parsed = app_model::SemanticVersion::parse(value);
    if (parsed && !parsed->prerelease.empty() && !valid_identifiers(parsed->prerelease, true)) {
        return std::nullopt;
    }
    return parsed;
}

bool prerelease_less(std::string_view left, std::string_view right) {
    if (left.empty() || right.empty()) {
        return !left.empty() && right.empty();
    }
    while (!left.empty() && !right.empty()) {
        const auto lhs = take_identifier(left);
        const auto rhs = take_identifier(right);
        if (lhs == rhs) {
            continue;
        }
        const bool lhs_numeric = numeric(lhs);
        const bool rhs_numeric = numeric(rhs);
        if (lhs_numeric != rhs_numeric) {
            return lhs_numeric;
        }
        // Numeric identifiers have no leading zeroes. Length then lexical order
        // compares arbitrary-size identifiers without integer overflow.
        if (lhs_numeric && lhs.size() != rhs.size()) {
            return lhs.size() < rhs.size();
        }
        return lhs < rhs;
    }
    return left.empty() && !right.empty();
}

bool version_less(const app_model::SemanticVersion& left, const app_model::SemanticVersion& right) {
    const auto lhs = std::tie(left.major, left.minor, left.patch);
    const auto rhs = std::tie(right.major, right.minor, right.patch);
    return lhs != rhs ? lhs < rhs : prerelease_less(left.prerelease, right.prerelease);
}

bool valid_digest(const std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](const char character) {
               return digit(character) || (character >= 'a' && character <= 'f') ||
                      (character >= 'A' && character <= 'F');
           });
}

} // namespace

Compatibility check_compatibility(const Candidate& candidate, const DeviceCompatibility& device,
                                  const DowngradePolicy policy) {
    const auto current = parse_version(device.version);
    if (device.board.empty() || device.chip.empty() || !current || device.bootloader_schema == 0 ||
        device.inactive_slot_size == 0) {
        return Compatibility::InvalidLocalIdentity;
    }
    if (candidate.manifest_schema != 1) {
        return Compatibility::UnsupportedManifestSchema;
    }
    if (candidate.board != device.board) {
        return Compatibility::WrongBoard;
    }
    if (candidate.chip != device.chip) {
        return Compatibility::WrongChip;
    }
    if (candidate.min_bootloader_schema == 0 ||
        candidate.min_bootloader_schema > device.bootloader_schema) {
        return Compatibility::UnsupportedBootloaderSchema;
    }
    if (candidate.image_size == 0 || candidate.image_size > device.inactive_slot_size) {
        return Compatibility::InvalidImageSize;
    }
    if (!valid_digest(candidate.sha256)) {
        return Compatibility::InvalidDigestDeclaration;
    }
    const auto proposed = parse_version(candidate.version);
    if (!proposed) {
        return Compatibility::InvalidCandidateVersion;
    }
    if (version_less(*proposed, *current) && policy != DowngradePolicy::ExplicitRecovery) {
        return Compatibility::Downgrade;
    }
    return Compatibility::MetadataCompatible;
}

} // namespace prd::ota
