// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_schedule.hpp"

#include <limits>

namespace prd::ota {
namespace {

[[nodiscard]] std::uint64_t jitter(const std::uint32_t entropy) {
    return (kReleaseCheckJitterWindowMs * entropy) /
           static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] std::uint64_t saturated_add(const std::uint64_t left, const std::uint64_t right) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

} // namespace

void ReleaseCheckSchedule::start(const ReleaseCheckTiming timing) {
    due_ms_ = saturated_add(timing.now_ms, jitter(timing.entropy));
    armed_ = true;
}

void ReleaseCheckSchedule::checked(const ReleaseCheckTiming timing) {
    const auto interval = saturated_add(kReleaseCheckIntervalMs, jitter(timing.entropy));
    due_ms_ = saturated_add(timing.now_ms, interval);
    armed_ = true;
}

void ReleaseCheckSchedule::reset() {
    due_ms_ = 0;
    armed_ = false;
}

} // namespace prd::ota
