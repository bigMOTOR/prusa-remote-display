// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "walk_time_base.hpp"

#include <chrono>

namespace prd::validation {
namespace {

// The separation between one replayed device lifetime and the next.
//
// Its only requirement is to be positive: a base equal to the previous script's last event would
// not be clamped — the reducer's test is strictly less-than — but it would give two events from
// two different device lifetimes the same instant, and a log where the boundary between loads
// cannot be seen is a log that cannot be read. One second is far larger than the 10 ms steps a
// fixture prelude uses and small enough to stay legible beside them.
//
// Nothing derives from this value. It is not a threshold, and no freshness rule is measured
// against it: every fixture re-establishes its own observations from its own boot prelude.
constexpr app_model::Timestamp kLoadGap =
    std::chrono::duration_cast<app_model::Timestamp>(std::chrono::milliseconds{1000});

} // namespace

app_model::Timestamp WalkTimeBase::base_for(const app_model::Timestamp now) const {
    return now > earliest_ ? now : earliest_;
}

void WalkTimeBase::observe(const app_model::Timestamp observed_at) {
    const app_model::Timestamp after = observed_at + kLoadGap;

    if (after > earliest_) {
        earliest_ = after;
    }
}

} // namespace prd::validation
