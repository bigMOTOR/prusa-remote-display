// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "vendor_log_policy.hpp"

#include <string>

#include "diagnostics/tags.hpp"

namespace prd::network::vendor_log_policy {

void apply() {
    for (const std::string_view tag : kTags) {
        diagnostics::set_external_tag_level(tag, kApplied);
    }

    // The tags are ESP-IDF's own and carry no secret; naming them is what makes the suppression
    // auditable in a capture rather than invisible.
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                     "vendor http log level held at warning for " + std::to_string(kTags.size()) +
                         " tags (§13.5: these components print header values at debug)");
}

} // namespace prd::network::vendor_log_policy
