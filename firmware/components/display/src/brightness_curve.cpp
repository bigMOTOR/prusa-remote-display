// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "brightness_curve.hpp"

#include <cmath>

namespace prd::display {

void build_brightness_table(std::uint32_t* const table, const std::uint32_t max_duty) {
    if (table == nullptr) {
        return;
    }

    table[0] = 0;

    for (std::size_t percent = 1; percent < kBrightnessTableSize; ++percent) {
        const float fraction = static_cast<float>(percent) / 100.0F;
        const float scaled = std::pow(fraction, kBrightnessGamma) * static_cast<float>(max_duty);

        // Round half up. clang-tidy suggests std::lround and auto here; both are
        // declined deliberately, not overlooked. `scaled` is never negative, which
        // is the case the rounding check exists for, and this table's values were
        // validated on the physical unit in Phase D — a rounding change could move
        // an entry by one duty step, which is a change to signed-off behaviour for
        // no functional gain. Named `std::uint32_t` also states the table's element
        // type at the point of computation.
        // NOLINTNEXTLINE(bugprone-incorrect-roundings,modernize-use-auto)
        std::uint32_t duty = static_cast<std::uint32_t>(scaled + 0.5F);

        // A non-zero request must never round down into darkness. At coarse duty
        // resolutions the bottom few percent all land here, which is a
        // resolution limit rather than an error — the physically determined
        // minimum useful brightness is a separate, higher figure.
        if (duty == 0) {
            duty = 1;
        }
        // clang-tidy suggests std::min. Declined: this is hardware-validated Phase D
        // code, and rewriting a clamp there to gain an <algorithm> include is churn
        // in a file whose output was signed off on the bench.
        // NOLINTNEXTLINE(readability-use-std-min-max)
        if (duty > max_duty) {
            duty = max_duty;
        }

        table[percent] = duty;
    }
}

std::uint32_t duty_for_percent_q8(const std::uint32_t* const table, std::uint32_t percent_q8) {
    if (table == nullptr) {
        return 0;
    }

    // std::min declined for the same reason as the clamp above: validated Phase D
    // code, no functional gain.
    // NOLINTNEXTLINE(readability-use-std-min-max)
    if (percent_q8 > kMaxPercentQ8) {
        percent_q8 = kMaxPercentQ8;
    }

    const std::uint32_t whole = percent_q8 / kPercentQ8One;
    if (whole >= 100) {
        return table[100];
    }

    // Safe against underflow because the table never decreases.
    const std::uint32_t low = table[whole];
    const std::uint32_t high = table[whole + 1];
    const std::uint32_t fraction = percent_q8 % kPercentQ8One;

    return low + (((high - low) * fraction) / kPercentQ8One);
}

std::uint8_t percent_for_light_fraction(const std::uint8_t ceiling_percent,
                                        const LightFraction light_fraction) {
    const std::uint32_t ceiling = ceiling_percent > 100U ? 100U : ceiling_percent;
    const std::uint32_t light = light_fraction.percent > 100U ? 100U : light_fraction.percent;

    // Nothing to be a fraction of, or no fraction wanted. Either way the answer is
    // true off rather than the floor below — floor 1 exists to keep a REQUESTED dim
    // level visible, not to light a panel nobody asked to light.
    if (ceiling == 0U || light == 0U) {
        return 0;
    }

    // ⬅️ THE INVERSE OF build_brightness_table()'s EXPONENT, AND DELIBERATELY THE SAME
    // CONSTANT. The table maps control -> light as (percent/100)^gamma; this maps the
    // light back to the control that produces it. Two constants would drift, and the
    // drift would be silent: the panel would simply be the wrong brightness.
    const float fraction = static_cast<float>(light) / 100.0F;
    const float scaled = static_cast<float>(ceiling) * std::pow(fraction, 1.0F / kBrightnessGamma);

    // Round half up, matching the table's own rounding. std::lround and auto declined
    // for the reason recorded at build_brightness_table(): `scaled` is never negative,
    // and this file's arithmetic was validated on the physical unit.
    // NOLINTNEXTLINE(bugprone-incorrect-roundings,modernize-use-auto)
    std::uint32_t percent = static_cast<std::uint32_t>(scaled + 0.5F);

    // A non-zero request must never round down into darkness — the same rule the duty
    // table applies one layer down, applied here for the same reason. Below a ceiling
    // of about 3 % the product lands under one whole percent, and without this the
    // recession would appear to skip Dimmed entirely: a state the log claims the
    // device is in while the panel shows nothing.
    if (percent == 0U) {
        percent = 1U;
    }

    // Cannot trigger for light <= 100 because pow(f, 1/gamma) <= 1 there, but asserted
    // in code rather than in a comment: if the clamp above is ever loosened, a Dimmed
    // panel brighter than its own ceiling is the failure it would produce.
    // NOLINTNEXTLINE(readability-use-std-min-max)
    if (percent > ceiling) {
        percent = ceiling;
    }

    return static_cast<std::uint8_t>(percent);
}

} // namespace prd::display
