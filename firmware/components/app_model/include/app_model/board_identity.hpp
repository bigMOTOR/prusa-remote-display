// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>

namespace prd::app_model {

enum class ValidationState {
    Unknown,
    Validated,
    Failed,
};

[[nodiscard]] std::string_view to_string(ValidationState state);

struct BoardIdentity {
    std::string target;
    std::string model;
    ValidationState flash_validation{ValidationState::Unknown};
    ValidationState psram_validation{ValidationState::Unknown};

    [[nodiscard]] bool is_fully_validated() const;
};

} // namespace prd::app_model
