// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "diagnostics/boot_summary.hpp"

#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

namespace prd::diagnostics {

void log_boot_build_summary(const app_model::BuildInfo& build_info) {
    log(LogLevel::Info, tag::kBoot, build_info.summary());
}

void log_boot_phase(const std::string_view phase_label) {
    log(LogLevel::Info, tag::kBoot, "phase=" + std::string{phase_label});
}

} // namespace prd::diagnostics
