// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "backoff.hpp"

namespace prd::network {

std::chrono::milliseconds delay_for(const std::uint32_t attempt, const Jitter jitter) {
    // Attempt 0 is not a retry — it is the first connect, which happens immediately.
    if (attempt == 0) {
        return std::chrono::milliseconds{0};
    }

    const std::size_t index = attempt - 1 < kSchedule.size() ? attempt - 1 : kSchedule.size() - 1;
    const std::int64_t base = kSchedule[index].count();

    // The full band is 2/8 of the step, applied as base - band/2 + offset, so the result
    // sits in [base * 7/8, base * 9/8]. Integer arithmetic throughout: this runs on a
    // microcontroller and a float here would buy nothing.
    const std::int64_t band = (base * 2) / static_cast<std::int64_t>(kJitterFractionDenominator);
    const std::int64_t offset = (band * jitter.value) / 255;

    return std::chrono::milliseconds{base - (band / 2) + offset};
}

bool schedule_saturated(const std::uint32_t attempt) {
    return attempt >= kSchedule.size();
}

} // namespace prd::network
