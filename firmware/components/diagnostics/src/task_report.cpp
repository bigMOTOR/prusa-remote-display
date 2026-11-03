// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "diagnostics/task_report.hpp"

#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "platform/task_stack.hpp"

namespace prd::diagnostics {

void log_task_stack(const std::string_view task_name, const std::uint32_t configured_bytes) {
    const std::uint32_t free_low = platform::task_stack_free_low_water_bytes();

    // Guard the subtraction: a caller that passes a configured size smaller than
    // the measured free stack has mismatched the constant, and reporting a
    // wrapped unsigned figure would hide that behind an absurd number.
    const bool consistent = configured_bytes >= free_low;
    const std::uint32_t used_peak = consistent ? configured_bytes - free_low : 0;
    const std::uint32_t headroom_percent =
        configured_bytes != 0 ? (free_low * 100U) / configured_bytes : 0;

    std::string line = "task=" + std::string{task_name} +
                       " stack_bytes=" + std::to_string(configured_bytes) +
                       " free_low_bytes=" + std::to_string(free_low);

    if (consistent) {
        line += " used_peak_bytes=" + std::to_string(used_peak) +
                " headroom=" + std::to_string(headroom_percent) + "%";
    } else {
        line += " INCONSISTENT: configured size is below the measured free stack,"
                " so the reported stack_bytes does not match this task";
    }

    log(consistent ? LogLevel::Info : LogLevel::Error, tag::kApp, line);
}

} // namespace prd::diagnostics
