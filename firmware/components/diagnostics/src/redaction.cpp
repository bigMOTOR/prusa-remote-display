// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "diagnostics/redaction.hpp"

#include <cstddef>

namespace prd::diagnostics {

std::string redact(const std::string_view message, const std::string_view secret) {
    if (secret.empty()) {
        return std::string{message};
    }

    std::string result;
    result.reserve(message.size());

    std::size_t position = 0;
    while (position < message.size()) {
        const auto found = message.find(secret, position);
        if (found == std::string_view::npos) {
            result.append(message.substr(position));
            break;
        }

        result.append(message.substr(position, found - position));
        result.append(kRedactedMarker);
        position = found + secret.size();
    }

    return result;
}

} // namespace prd::diagnostics
