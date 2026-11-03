// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "app_model/board_identity.hpp"

namespace prd::app_model {

// One expected-vs-detected hardware capacity comparison (Bootstrap Spec §15
// items 8-9). detected_bytes is nullopt when detection itself was
// unavailable, which classifies as Unknown ("unvalidated") rather than Failed.
struct CapacityCheck {
    std::string label;
    std::uint64_t expected_bytes{0};
    std::optional<std::uint64_t> detected_bytes;
    ValidationState result{ValidationState::Unknown};
};

[[nodiscard]] CapacityCheck check_capacity(std::string label, std::uint64_t expected_bytes,
                                           std::optional<std::uint64_t> detected_bytes);

// Log-facing result labels required by Bootstrap Spec §15:
// pass / fail / unvalidated.
[[nodiscard]] std::string_view validation_result_label(ValidationState state);

// "<label>_detected=<bytes|unavailable> expected=<bytes> result=<label>"
[[nodiscard]] std::string to_log_line(const CapacityCheck& check);

} // namespace prd::app_model
