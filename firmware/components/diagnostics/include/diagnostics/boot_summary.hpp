// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

#include "app_model/build_info.hpp"

namespace prd::diagnostics {

// Logs the single structured build-identity line required at boot
// (Bootstrap Spec §14.5, §15) under the prd.boot tag.
void log_boot_build_summary(const app_model::BuildInfo& build_info);

// Logs a structured boot phase transition, e.g. "phase=ready"
// (Bootstrap Spec §15) under the prd.boot tag.
void log_boot_phase(std::string_view phase_label);

} // namespace prd::diagnostics
