// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>
#include <string_view>

// How old an observation is, and what that means.
//
// Every timestamp in the application model is monotonic time since boot, in the
// same units and with the same guarantee as platform::monotonic_time() and the
// host tests' FakeClock. Wall-clock time is deliberately absent: printer
// monitoring must not depend on internet time (Architecture Spec §5.15) and the
// reducer must not read a clock at all (it receives time as an argument).

namespace prd::app_model {

using Timestamp = std::chrono::microseconds;

// Architecture Spec §7.5 freshness, plus §11.5's fourth behaviour.
//
// §7.5 lists fresh / aging / stale / unknown. §11.5 then describes a distinct
// expired behaviour — "sensitive dynamic values such as remaining time are
// removed" — which is not the same as stale, where previous values are still
// shown behind an explicit indicator. Expired is therefore a value here rather
// than a special case hidden inside the projection.
enum class DataFreshness {
    // No observation has been made, so age is undefined. Not the same as old.
    Unknown,

    // Within the expected update window: may be presented as current.
    Fresh,

    // Missed updates, but recent enough to retain visible state (§11.5).
    Aging,

    // Presentable only behind an explicit stale indicator (§11.5).
    Stale,

    // Dynamic values must be removed rather than shown (§11.5).
    Expired,
};

[[nodiscard]] std::string_view log_label(DataFreshness freshness);

// PROVISIONAL thresholds. §11.5 states them as proposed behaviour, and §11.1's
// 2 s active polling interval — the input to "within 2 expected polling
// intervals" — is itself "subject to physical load validation". These become
// settled values when polling is validated against a real printer, which is
// Milestone 4. They are grouped in a struct rather than written as constants so
// a test can state its own policy and so tuning later touches one place.
struct FreshnessPolicy {
    // §11.5 fresh: "last successful status within 2 expected polling intervals",
    // against §11.1's 2 s active interval.
    std::chrono::milliseconds fresh_within{4000};

    std::chrono::milliseconds aging_within{15000};

    std::chrono::milliseconds stale_within{60000};
};

// Classifies an observation by its age.
//
// Takes the age rather than a pair of timestamps: two adjacent parameters of the
// same type are trivially swappable at a call site, and "now minus observed_at"
// is the caller's business anyway. A negative age means time appeared to move
// backwards, which the reducer treats as out-of-order rather than as a very
// fresh observation; it is clamped to Fresh here and counted there.
[[nodiscard]] DataFreshness classify_freshness(Timestamp age, const FreshnessPolicy& policy);

} // namespace prd::app_model
