// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/hardware_check.hpp"

#include <sstream>
#include <utility>

namespace prd::app_model {

CapacityCheck check_capacity(std::string label, const std::uint64_t expected_bytes,
                             const std::optional<std::uint64_t> detected_bytes) {
    CapacityCheck check;
    check.label = std::move(label);
    check.expected_bytes = expected_bytes;
    check.detected_bytes = detected_bytes;

    if (detected_bytes.has_value()) {
        check.result = (*detected_bytes == expected_bytes) ? ValidationState::Validated
                                                           : ValidationState::Failed;
    }

    return check;
}

std::string_view validation_result_label(const ValidationState state) {
    switch (state) {
    case ValidationState::Unknown:
        return "unvalidated";

    case ValidationState::Validated:
        return "pass";

    case ValidationState::Failed:
        return "fail";
    }

    return "unvalidated";
}

std::string to_log_line(const CapacityCheck& check) {
    std::ostringstream stream;
    stream << check.label << "_detected=";

    if (check.detected_bytes.has_value()) {
        stream << *check.detected_bytes;
    } else {
        stream << "unavailable";
    }

    stream << " expected=" << check.expected_bytes
           << " result=" << validation_result_label(check.result);

    return stream.str();
}

} // namespace prd::app_model
