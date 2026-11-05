// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "app_model/bounded_string.hpp"
#include "app_model/observation.hpp"
#include "app_model/presentation.hpp"
#include "app_model/values.hpp"

// Presentation formatting — Architecture Spec §5.12 `ui_formatters` and §17.7.
//
// §17.7: "Formatting functions must be deterministic and testable." So this file
// contains no LVGL header, no ESP-IDF header and no clock read, and it is compiled
// directly into the host test build — the same arrangement app_core's button
// translation already uses.
//
// WHERE THE LINE IS. app_model's projection decides what is presentable and emits
// values with an explicit `known` flag; this decides how a known value reads. The
// projection deliberately emits no text at all, so nothing here is a second opinion
// about visibility: every function takes a value that the projection has already
// said may be shown.
//
// ABSENCE IS NEVER FORMATTED. Every function returns an empty result for an
// unknown value, and none of them can emit a placeholder — there is no dash
// anywhere in this file. UI Design Decision 003 forbids placeholders for
// information that does not yet exist, and Production UI §9 forbids them twice
// more. A caller that receives an empty string must draw nothing, which is what the
// components do.

namespace prd::ui::format {

// Sized for the longest output each function can produce, with the reasoning at
// each declaration. Fixed capacity rather than std::string: these run on the render
// task every time a value changes, and §21's budget rules out heap traffic in a
// path that repeats.
using ValueText = app_model::BoundedString<12>;
// Full uint32 seconds, including the estimate marker: "~1193046h28m left".
using RemainingText = app_model::BoundedString<17>;
using NameText = app_model::BoundedString<32>;
using UpdateVersionText = app_model::BoundedString<48>;

// A printer condition qualifies the retained subject explicitly; it does not
// establish a job, progress, or a new operational energy.
[[nodiscard]] NameText error_context(const app_model::Presentation& presentation);

// Whole elapsed seconds from the last successful status, not from scene entry.
// uint32 milliseconds matches LVGL's monotonic boot clock, including its wrap.
[[nodiscard]] NameText observation_age(std::optional<app_model::Timestamp> observed,
                                       std::uint32_t now_ms);

// Whole degrees and the degree sign, per §17.7 ("whole-degree display for
// temperatures"). Rounds to nearest rather than truncating: a bed reported at
// 59.8 °C reads as 60, which is what the number means.
//
// Longest output: "-273°" — five characters. Negative values are formatted rather
// than rejected because a printer reporting a below-zero chamber is reporting a
// fact, and silently blanking it would lose it.
[[nodiscard]] ValueText temperature(const app_model::TemperatureValue& value);

// Percent without a unit — the unit is a separate typographic element in the
// accepted composition (UIDD 001 shows "64" large and "%" small), so joining them
// here would force one font on both.
//
// Longest output: "100".
[[nodiscard]] ValueText percent(const app_model::ProgressValue& value);

// Firmware version as the accepted update composition states it, e.g. "Firmware 0.2.0".
// The model accepts at most 31 bytes, so the 48-byte result always holds the catalogue label,
// one separator and the full supplied version. Empty input remains absent.
[[nodiscard]] UpdateVersionText firmware_version(const app_model::BoundedString<32>& version);

// Remaining or elapsed duration as H:MM (owner decision D9).
//
// NO SECONDS, EVER. §17.7 requires "no rapid text jitter from small estimate
// changes", and a seconds field on a value the printer revises every two seconds is
// exactly that jitter. Minutes truncate toward zero, so a job with 30 seconds left
// reads "0:00" — which is true, and the state change to Completed is what resolves
// it.
//
// Hours are not clamped: a 100-hour print reads "100:00" and stays legible.
// Longest realistic output: "999:59".
[[nodiscard]] ValueText duration(const app_model::DurationValue& value);

// The `~` marker for an estimate §17.7 calls inaccurate, prefixed to a duration.
// Separate from duration() rather than a flag on it, because the caller already
// holds `remaining_is_approximate` and a bool parameter at a call site reads as
// neither one thing nor the other.
//
// Longest output: "~999:59".
[[nodiscard]] ValueText approximate_duration(const app_model::DurationValue& value,
                                             bool is_approximate);

// Printing estimate with explicit units and direction, e.g. "2h42m left".
// Below one hour: "42m left". Total hours never roll over at 24; minutes
// truncate toward zero, including "0m left" in the final minute. Unknown stays absent.
[[nodiscard]] RemainingText remaining_duration(const app_model::DurationValue& value,
                                               bool is_approximate);

// Layer as current/total, e.g. "87/240".
//
// Emits nothing when the total is unknown or zero: "87/0" would be false and "87"
// alone would read as a total. The projection already withholds layer data before a
// job is established, so this guards the residual case rather than duplicating that
// decision.
//
// Longest output: "99999/99999".
[[nodiscard]] ValueText layers(const app_model::LayerProgress& value);

// Filename for display: extension removed, then truncated at the middle if it does
// not fit.
//
// §7.8 makes extension removal presentation-only, which is why it happens here and
// not in the reducer. §17.7 requires "stable filename truncation" — stable meaning
// the same name always truncates the same way, so middle truncation with a fixed
// ellipsis is used rather than a width-dependent tail. The head is kept longer than
// the tail because a print name's distinguishing part is usually at the front.
//
// Not called by the Phase C Printing scene, which does not show the filename (owner
// decision D9). Implemented and tested now because Completed and Stopped need it in
// Phase D and the rule belongs with the other formatters.
[[nodiscard]] NameText filename(const app_model::BoundedString<64>& name);

} // namespace prd::ui::format
