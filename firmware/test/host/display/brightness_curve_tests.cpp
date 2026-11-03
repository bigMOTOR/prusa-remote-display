// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "brightness_curve.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

namespace {

using prd::display::build_brightness_table;
using prd::display::duty_for_percent_q8;
using prd::display::kBrightnessTableSize;
using prd::display::kMaxPercentQ8;
using prd::display::kPercentQ8One;

// The duty resolutions the LEDC timer actually lands on for the Phase D
// candidate frequencies with a 40 MHz XTAL source: 14 bits at 2 kHz and below,
// 12 at 5 kHz, 11 at 10 kHz, 10 at 20 kHz, 9 at 40 kHz.
constexpr std::uint32_t kResolutionBits[] = {9, 10, 11, 12, 14};

using Table = std::array<std::uint32_t, kBrightnessTableSize>;

Table table_for(const std::uint32_t bits) {
    Table table{};
    build_brightness_table(table.data(), 1U << bits);
    return table;
}

TEST(BrightnessCurve, ZeroPercentIsTrueOff) {
    for (const std::uint32_t bits : kResolutionBits) {
        const Table table = table_for(bits);
        EXPECT_EQ(table[0], 0U) << "bits=" << bits;
    }
}

TEST(BrightnessCurve, HundredPercentIsFullScale) {
    for (const std::uint32_t bits : kResolutionBits) {
        const Table table = table_for(bits);
        EXPECT_EQ(table[100], 1U << bits) << "bits=" << bits;
    }
}

TEST(BrightnessCurve, NeverDecreases) {
    for (const std::uint32_t bits : kResolutionBits) {
        const Table table = table_for(bits);
        for (std::size_t percent = 1; percent < kBrightnessTableSize; ++percent) {
            EXPECT_GE(table[percent], table[percent - 1])
                << "bits=" << bits << " percent=" << percent;
        }
    }
}

TEST(BrightnessCurve, NonZeroRequestIsNeverDark) {
    // The whole point of the floor: however coarse the resolution, asking for
    // 1 % must still drive the LED string.
    for (const std::uint32_t bits : kResolutionBits) {
        const Table table = table_for(bits);
        for (std::size_t percent = 1; percent < kBrightnessTableSize; ++percent) {
            EXPECT_GE(table[percent], 1U) << "bits=" << bits << " percent=" << percent;
        }
    }
}

TEST(BrightnessCurve, IsPerceptualNotLinear) {
    // A gamma curve puts the midpoint well below half of full scale. If this ever
    // becomes linear, equal percentage steps stop looking like equal brightness
    // steps and the fade regains the "rush then crawl" behaviour the curve exists
    // to remove.
    const Table table = table_for(12);
    const std::uint32_t full_scale = 1U << 12;

    EXPECT_LT(table[50], full_scale / 3);
    EXPECT_GT(table[50], full_scale / 8);
}

TEST(BrightnessCurve, InterpolationMatchesTableAtWholePercent) {
    const Table table = table_for(12);

    for (std::uint32_t percent = 0; percent <= 100; ++percent) {
        EXPECT_EQ(duty_for_percent_q8(table.data(), percent * kPercentQ8One), table[percent])
            << "percent=" << percent;
    }
}

TEST(BrightnessCurve, InterpolationStaysBetweenNeighbours) {
    const Table table = table_for(14);

    for (std::uint32_t percent = 0; percent < 100; ++percent) {
        const std::uint32_t low = table[percent];
        const std::uint32_t high = table[percent + 1];

        for (std::uint32_t fraction = 0; fraction < kPercentQ8One; fraction += 17) {
            const std::uint32_t duty =
                duty_for_percent_q8(table.data(), percent * kPercentQ8One + fraction);
            EXPECT_GE(duty, low) << "percent=" << percent << " fraction=" << fraction;
            EXPECT_LE(duty, high) << "percent=" << percent << " fraction=" << fraction;
        }
    }
}

TEST(BrightnessCurve, InterpolationClampsAboveFullScale) {
    const Table table = table_for(12);

    EXPECT_EQ(duty_for_percent_q8(table.data(), kMaxPercentQ8), table[100]);
    EXPECT_EQ(duty_for_percent_q8(table.data(), kMaxPercentQ8 * 2), table[100]);
}

TEST(BrightnessCurve, HandlesNullTable) {
    // Defensive: the LEDC path bails out before building a table if the timer
    // could not be configured, so neither entry point may dereference nothing.
    build_brightness_table(nullptr, 4096);
    EXPECT_EQ(duty_for_percent_q8(nullptr, 50 * kPercentQ8One), 0U);
}

// --- Light fractions --------------------------------------------------------
//
// ⛔ THESE PROTECT A CONFIRMED PHYSICAL DEFECT, NOT A HYPOTHETICAL. The display policy specified
// Dimmed as 15 % of the user's brightness and implemented it as 15 % of the CONTROL value. This
// file's own table then raised that value to kBrightnessGamma, so the panel emitted 1.5 % — and
// the owner, looking at the board, judged Dimmed unreadable. The measurement is in the capture:
// at a ceiling of 50 the old rule asked for 7 %, which is duty 12 against the ceiling's 891.
//
// What is asserted below is the EMITTED LIGHT, read out of the duty table rather than assumed,
// because light is the thing the rule was always about and the units are where it went wrong.

using prd::display::LightFraction;
using prd::display::percent_for_light_fraction;

// The production duty resolution: 12 bits at the validated 5 kHz PWM frequency.
constexpr std::uint32_t kProductionBits = 12;

// The light a control percentage actually emits, as a fraction of the light at `ceiling`.
double emitted_ratio(const Table& table, const std::uint8_t percent, const std::uint8_t ceiling) {
    return static_cast<double>(table[percent]) / static_cast<double>(table[ceiling]);
}

// What the defective rule computed: a fraction of the control value.
std::uint8_t control_space_scaling(const std::uint8_t ceiling, const std::uint8_t light) {
    const std::uint32_t scaled = (static_cast<std::uint32_t>(ceiling) * light) / 100U;
    return static_cast<std::uint8_t>(scaled == 0U ? 1U : scaled);
}

constexpr std::uint8_t kCeilingCandidates[] = {20, 25, 40, 50, 65, 80, 100};
constexpr std::uint8_t kLightFractions[] = {6, 12, 15, 20, 30};

TEST(BrightnessCurve, AControlPercentageEmitsTheLightFractionItWasAskedFor) {
    const Table table = table_for(kProductionBits);

    for (const std::uint8_t ceiling : kCeilingCandidates) {
        for (const std::uint8_t light : kLightFractions) {
            const std::uint8_t percent = percent_for_light_fraction(ceiling, LightFraction{light});
            const double ratio = emitted_ratio(table, percent, ceiling);
            const double wanted = static_cast<double>(light) / 100.0;

            // Integer percentages cannot hit a fraction exactly, and the table's own rounding
            // moves the answer again. The band is wide enough to absorb both and far too narrow
            // to admit the gamma defect, which lands an order of magnitude low.
            EXPECT_GT(ratio, wanted * 0.75)
                << "ceiling=" << static_cast<int>(ceiling) << " light=" << static_cast<int>(light)
                << " percent=" << static_cast<int>(percent);
            EXPECT_LT(ratio, wanted * 1.30)
                << "ceiling=" << static_cast<int>(ceiling) << " light=" << static_cast<int>(light)
                << " percent=" << static_cast<int>(percent);
        }
    }
}

// ⛔ THE FIRING NEGATIVE CONTROL. It asserts what the OLD rule really did, at the exact ceiling the
// board was running, so the two behaviours are separated by measurement rather than by argument.
// If the correction is ever reverted, percent_for_light_fraction() starts returning the
// control-space answer and the final expectation here fails — which is the whole reason this test
// computes the defect instead of merely describing it.
TEST(BrightnessCurve, ControlSpaceScalingEmitsAboutATenthOfTheLightItNames) {
    const Table table = table_for(kProductionBits);

    constexpr std::uint8_t kCeiling = 50; // the driver's validated startup level
    constexpr std::uint8_t kLight = 15;   // Timings::dimmed_light_percent_of_user

    const std::uint8_t defective = control_space_scaling(kCeiling, kLight);
    const std::uint8_t corrected = percent_for_light_fraction(kCeiling, LightFraction{kLight});

    // The two answers, pinned. These are the numbers the panel showed and the numbers it will.
    EXPECT_EQ(defective, 7) << "the rule that shipped: 50 * 15 / 100";
    EXPECT_EQ(corrected, 21) << "the control level whose light is 15 % of the ceiling's";

    const double defective_ratio = emitted_ratio(table, defective, kCeiling);
    const double corrected_ratio = emitted_ratio(table, corrected, kCeiling);

    // duty 12 against duty 891 — the measurement from the bench capture.
    EXPECT_EQ(table[defective], 12U);
    EXPECT_EQ(table[kCeiling], 891U);

    EXPECT_LT(defective_ratio, 0.02) << "the defect emitted under 2 % of the ceiling's light";
    EXPECT_LT(defective_ratio * 5.0, 0.15)
        << "the shipped Dimmed state was more than five times darker than it claimed";

    EXPECT_GT(corrected_ratio, 0.13);
    EXPECT_LT(corrected_ratio, 0.17);

    // ⛔ THE CONTROL ITSELF: the corrected function must not agree with the defective rule.
    EXPECT_NE(corrected, defective)
        << "a fraction of the control value is not a fraction of the light; if these are equal "
           "the gamma-domain correction has been reverted";
}

TEST(BrightnessCurve, FullLightIsTheCeilingItselfAndZeroIsTrueOff) {
    for (std::uint8_t ceiling = 0; ceiling <= 100; ++ceiling) {
        EXPECT_EQ(percent_for_light_fraction(ceiling, LightFraction{100}), ceiling)
            << "ceiling=" << static_cast<int>(ceiling);
        EXPECT_EQ(percent_for_light_fraction(ceiling, LightFraction{0}), 0)
            << "no fraction wanted is true off, not the floor";
    }

    EXPECT_EQ(percent_for_light_fraction(0, LightFraction{15}), 0)
        << "nothing to take a fraction of";

    // Out-of-range inputs clamp rather than overflow the control range.
    EXPECT_EQ(percent_for_light_fraction(255, LightFraction{100}), 100);
    EXPECT_EQ(percent_for_light_fraction(80, LightFraction{255}), 80);
}

// ⛔ NEGATIVE CONTROL: a dim panel may never be brighter than an awake one, and may never collapse
// into an off one. Both invariants moved here from the display policy with the arithmetic, and
// they are asserted exhaustively because that is now cheap.
TEST(BrightnessCurve, ADimLevelIsNeverBrighterThanItsCeilingAndNeverRoundsIntoDarkness) {
    for (std::uint16_t ceiling = 1; ceiling <= 100; ++ceiling) {
        for (std::uint16_t light = 1; light <= 100; ++light) {
            const std::uint8_t percent =
                percent_for_light_fraction(static_cast<std::uint8_t>(ceiling),
                                           LightFraction{static_cast<std::uint8_t>(light)});

            EXPECT_LE(percent, ceiling) << "ceiling=" << ceiling << " light=" << light;
            EXPECT_GE(percent, 1) << "ceiling=" << ceiling << " light=" << light;
        }
    }
}

// ⛔ NEGATIVE CONTROL FOR THE FLOOR, FROM THE OTHER SIDE. The old control-space rule collapsed to
// its floor of 1 for every ceiling at or below 6 %, so three different ceilings produced one
// indistinguishable Dimmed level. The corrected mapping separates them, because 15 % of a
// brighter ceiling's light really is brighter.
TEST(BrightnessCurve, LowCeilingsNoLongerCollapseOntoOneDimLevel) {
    EXPECT_EQ(control_space_scaling(4, 15), 1);
    EXPECT_EQ(control_space_scaling(6, 15), 1);
    EXPECT_EQ(control_space_scaling(10, 15), 1);

    EXPECT_EQ(percent_for_light_fraction(4, LightFraction{15}), 2);
    EXPECT_EQ(percent_for_light_fraction(6, LightFraction{15}), 3);
    EXPECT_EQ(percent_for_light_fraction(10, LightFraction{15}), 4);
}

TEST(BrightnessCurve, TheResultNeverDecreasesAsEitherArgumentRises) {
    for (std::uint16_t light = 1; light <= 100; ++light) {
        std::uint8_t previous = 0;
        for (std::uint16_t ceiling = 1; ceiling <= 100; ++ceiling) {
            const std::uint8_t percent =
                percent_for_light_fraction(static_cast<std::uint8_t>(ceiling),
                                           LightFraction{static_cast<std::uint8_t>(light)});
            EXPECT_GE(percent, previous) << "light=" << light << " ceiling=" << ceiling;
            previous = percent;
        }
    }

    for (std::uint16_t ceiling = 1; ceiling <= 100; ++ceiling) {
        std::uint8_t previous = 0;
        for (std::uint16_t light = 1; light <= 100; ++light) {
            const std::uint8_t percent =
                percent_for_light_fraction(static_cast<std::uint8_t>(ceiling),
                                           LightFraction{static_cast<std::uint8_t>(light)});
            EXPECT_GE(percent, previous) << "ceiling=" << ceiling << " light=" << light;
            previous = percent;
        }
    }
}

} // namespace
