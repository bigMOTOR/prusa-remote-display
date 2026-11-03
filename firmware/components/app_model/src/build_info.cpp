// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/build_info.hpp"

#include <charconv>
#include <sstream>
#include <system_error>

namespace prd::app_model {
namespace {

[[nodiscard]] std::optional<unsigned int> parse_number(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }

    unsigned int result = 0;
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();

    const auto conversion = std::from_chars(begin, end, result);
    if (conversion.ec != std::errc{} || conversion.ptr != end) {
        return std::nullopt;
    }

    return result;
}

} // namespace

std::optional<SemanticVersion> SemanticVersion::parse(const std::string_view value) {
    const auto prerelease_separator = value.find('-');
    const auto core = value.substr(0, prerelease_separator);

    const auto first_dot = core.find('.');
    if (first_dot == std::string_view::npos) {
        return std::nullopt;
    }

    const auto second_dot = core.find('.', first_dot + 1);
    if (second_dot == std::string_view::npos ||
        core.find('.', second_dot + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    const auto major = parse_number(core.substr(0, first_dot));
    const auto minor = parse_number(core.substr(first_dot + 1, second_dot - first_dot - 1));
    const auto patch = parse_number(core.substr(second_dot + 1));

    if (!major.has_value() || !minor.has_value() || !patch.has_value()) {
        return std::nullopt;
    }

    SemanticVersion version;
    version.major = *major;
    version.minor = *minor;
    version.patch = *patch;

    if (prerelease_separator != std::string_view::npos) {
        const auto prerelease = value.substr(prerelease_separator + 1);
        if (prerelease.empty()) {
            return std::nullopt;
        }

        version.prerelease = std::string{prerelease};
    }

    return version;
}

std::string SemanticVersion::to_string() const {
    std::ostringstream stream;
    stream << major << '.' << minor << '.' << patch;

    if (!prerelease.empty()) {
        stream << '-' << prerelease;
    }

    return stream.str();
}

bool SemanticVersion::operator==(const SemanticVersion& other) const {
    return major == other.major && minor == other.minor && patch == other.patch &&
           prerelease == other.prerelease;
}

std::string BuildInfo::summary() const {
    std::ostringstream stream;
    stream << "version=" << version.to_string() << " git=" << git_commit
           << " dirty=" << (git_dirty ? "true" : "false") << " profile=" << build_profile
           << " idf=" << idf_version << " board=" << board_target;

    return stream.str();
}

} // namespace prd::app_model
