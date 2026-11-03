// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <string_view>

#include "diagnostics/log.hpp"

// The log level ESP-IDF's HTTP components are held at, and why.
//
// ⛔ THE DEFECT THIS CLOSES. Two vendor components print credential material at Debug, and both
// `debug` and `hardware-validation` set CONFIG_LOG_MAXIMUM_LEVEL_DEBUG:
//
//   • `HTTP_CLIENT` — esp_http_client.c:1724 logs the whole outgoing request header buffer, which
//     contains `X-Api-Key: <the printer's key>`. Also :245 (`HEADER=%s:%s`, response header name
//     and value) and :1923 (the WWW-Authenticate header on a 401).
//   • `httpd_parse` — logs every setup-request header name and value, which on this device
//     includes the setup session's CSRF token (§14.4: the setup server must never expose secrets).
//
// §13.5 is unqualified — "credentials must never be written to logs" — and the precedent from
// Milestone 3.11's F1 is that the shipped image's behaviour is the contract regardless of whose
// code emits the line. `release` compiles ESP_LOGD out and was never affected; the two profiles
// that produce every bench capture were.
//
// ⬅️ WHY THIS SHAPE. It is the same shape T19 settled on for the Wi-Fi log level, and for the same
// reason: the value IS the contract, and what goes wrong is somebody raising it to chase a bug and
// leaving it raised. Pinning it here makes it host-testable.
//
// ⬅️ AND UNLIKE T19, esp_log_level_set REACHES THESE. The Wi-Fi blobs bypass esp_log entirely and
// needed esp_wifi_internal_set_log_level(); these components call ESP_LOGD, so the ordinary tag
// mechanism works — which is why the fix is a level and not a patched vendor call.
//
// ⛔ SCOPED TO TWO VENDOR TAGS. Project diagnostics are untouched: nothing here raises the global
// level or `*`, so `prd.*` output — including everything the bench evidence depends on — keeps its
// configured verbosity. Silencing broadly would have hidden the next finding.

namespace prd::network::vendor_log_policy {

// Warning, not Error: a genuine transport complaint from these components is worth seeing, and
// Warning is the highest level at which none of the three credential-printing lines is emitted
// (all three are ESP_LOGD, and :1095's redirect-count line is ESP_LOGE and carries no secret).
inline constexpr diagnostics::LogLevel kApplied = diagnostics::LogLevel::Warning;

// The ESP-IDF tag strings, exactly as those components declare them:
// `static const char *TAG = "HTTP_CLIENT"` and `"httpd_parse"`.
inline constexpr std::string_view kHttpClientTag = "HTTP_CLIENT";
inline constexpr std::string_view kHttpServerParseTag = "httpd_parse";

inline constexpr std::array<std::string_view, 2> kTags{kHttpClientTag, kHttpServerParseTag};

static_assert(kApplied == diagnostics::LogLevel::Warning,
              "Debug or Info would re-admit the header lines this policy exists to suppress");
static_assert(kTags.size() == 2, "both credential-printing vendor tags must be covered");
static_assert(!kHttpClientTag.empty() && !kHttpServerParseTag.empty(),
              "an empty tag would silently apply to nothing");

// Applies the policy. Idempotent, and safe to call before the components are first used —
// esp_log_level_set records a level for a tag whether or not anything has logged with it yet.
void apply();

} // namespace prd::network::vendor_log_policy
