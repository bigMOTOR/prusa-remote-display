// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/board_identity.hpp"

namespace prd::app_model {

std::string_view to_string(const ValidationState state) {
    switch (state) {
    case ValidationState::Unknown:
        return "Unknown";

    case ValidationState::Validated:
        return "Validated";

    case ValidationState::Failed:
        return "Failed";
    }

    return "Unknown";
}

bool BoardIdentity::is_fully_validated() const {
    return flash_validation == ValidationState::Validated &&
           psram_validation == ValidationState::Validated;
}

} // namespace prd::app_model
