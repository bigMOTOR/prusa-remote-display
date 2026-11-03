// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

namespace prd::diagnostics {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

void log(LogLevel level, std::string_view tag, std::string_view message);

// One provisioning-timeline record, in a shape a script can parse.
//
// WHY THIS EXISTS AS ITS OWN CALL. Several physical validation cycles were spent asking where
// thirty seconds went, and the answer was never in the log because each component reported its
// own step in its own words while the numbers that mattered — a reason code, an attempt
// number, the moment a dwell began — were absent or spread across four tags. Every record
// here carries the same prefix and the same key=value grammar, so one grep is the whole
// timeline and the arithmetic is a script's job rather than a reader's.
//
// ⛔ NON-SECRET BY CONSTRUCTION. The signature admits an event name and up to two integer
// measurements. There is deliberately no way to pass a string value, so no SSID, passphrase,
// API key or address can reach a timeline record even by mistake (§13.5, §24.4).
//
// The ESP-IDF log timestamp supplies the time base; it is milliseconds since boot and is
// already on every line.
void timeline(std::string_view tag, std::string_view event);
void timeline(std::string_view tag, std::string_view event, std::string_view name,
              std::int64_t value);
void timeline(std::string_view tag, std::string_view event, std::string_view first_name,
              std::int64_t first_value, std::string_view second_name, std::int64_t second_value);

// Raises the minimum level for a THIRD-PARTY log tag at run time.
//
// Project tags are not the target: they come from the canonical registry and their
// volume is ours to control at the call site. This exists for ESP-IDF's own
// drivers, whose verbose output is per-transaction and can bury — or perturb — the
// measurement it sits inside. `tag` is an ESP-IDF tag string, not a
// diagnostics::tag value.
void set_external_tag_level(std::string_view tag, LogLevel level);

} // namespace prd::diagnostics
