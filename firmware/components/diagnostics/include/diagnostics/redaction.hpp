// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>

namespace prd::diagnostics {

// Replacement marker for redacted secret material.
inline constexpr std::string_view kRedactedMarker = "<redacted>";

// Returns message with every occurrence of secret replaced by
// kRedactedMarker. Empty secrets redact nothing. Redaction is centralized
// here so it stays host-testable (Bootstrap Spec §6.5; Architecture Spec
// §13.5: credentials must never reach logs).
[[nodiscard]] std::string redact(std::string_view message, std::string_view secret);

} // namespace prd::diagnostics
