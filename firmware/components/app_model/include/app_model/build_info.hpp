// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace prd::app_model {

struct SemanticVersion {
    unsigned int major{0};
    unsigned int minor{0};
    unsigned int patch{0};
    std::string prerelease;

    [[nodiscard]] static std::optional<SemanticVersion> parse(std::string_view value);
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const SemanticVersion& other) const;
};

struct BuildInfo {
    SemanticVersion version;
    std::string git_commit;
    std::string build_profile;
    std::string idf_version;
    std::string board_target;
    bool git_dirty{false};

    [[nodiscard]] std::string summary() const;
};

// Returns the BuildInfo for the running firmware image, populated from
// build-time generated metadata. Only available in the ESP-IDF build;
// host tests compile app_model without this translation unit.
[[nodiscard]] BuildInfo current_build_info();

} // namespace prd::app_model
