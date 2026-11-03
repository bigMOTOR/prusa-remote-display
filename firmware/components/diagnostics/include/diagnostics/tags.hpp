// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

// Canonical project log tags (Bootstrap Spec §6.5). All project log lines use
// one of these tags so serial output stays filterable and stable.

namespace prd::diagnostics::tag {

inline constexpr std::string_view kBoot = "prd.boot";
inline constexpr std::string_view kBoard = "prd.board";
inline constexpr std::string_view kApp = "prd.app";
inline constexpr std::string_view kReview = "prd.review";
inline constexpr std::string_view kDisplay = "prd.display";
inline constexpr std::string_view kStorage = "prd.storage";
inline constexpr std::string_view kConfig = "prd.config";
inline constexpr std::string_view kSecrets = "prd.secrets";
inline constexpr std::string_view kNetwork = "prd.network";

// The printer integration (Architecture Spec §5.8). Separate from kNetwork because a
// reader filtering for printer traffic should not have to also see radio and mDNS lines.
inline constexpr std::string_view kPrinter = "prd.printer";
inline constexpr std::string_view kButton = "prd.button";
inline constexpr std::string_view kUi = "prd.ui";
inline constexpr std::string_view kProvisioning = "prd.provisioning";

// One tag per component that logs, declared here and nowhere else. A component
// that hard-codes its own literal defeats the point of the registry: the set of
// tags stops being knowable from one place, and a typo produces a tag that
// filters differently while looking correct.

} // namespace prd::diagnostics::tag
