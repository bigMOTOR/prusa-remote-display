// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>

#include "scenes/operational_accent.hpp"
#include "scenes/operational_energy.hpp"
#include "theme/tokens.hpp"

// The operational accent — UI Design Decision 004 §5.1, as ruled by the owner on 2026-09-01.
//
// WHAT THIS SUITE NOW PROVES, AND WHAT IT USED TO. It used to prove a RAMP: that the accent rose
// continuously from a working-baseline resting level to the full token, that one scalar drove all
// three channels, and that the six energies drew six different colours. The owner rejected both
// the endpoint and the mechanism. So the property under test is inverted — the accent is one
// colour, `color.accent`, and nothing varies it.
//
// ⬅️ THE STRONGEST GUARD IS NOT IN THIS FILE. scene::accent() takes no Energy, so reinstating the
// ramp cannot be done without changing its signature, and every call site stops compiling. What is
// asserted below is the value and the rejected endpoint; what the type system asserts is that
// energy cannot reach the accent at all.

using namespace prd::ui::scene;

namespace {

constexpr std::uint8_t red_of(const std::uint32_t colour) {
    return static_cast<std::uint8_t>((colour >> 16U) & 0xFFU);
}

constexpr std::uint8_t green_of(const std::uint32_t colour) {
    return static_cast<std::uint8_t>((colour >> 8U) & 0xFFU);
}

constexpr std::uint8_t blue_of(const std::uint32_t colour) {
    return static_cast<std::uint8_t>(colour & 0xFFU);
}

// The six magnitudes, named so a failure says which state is wrong rather than which number.
struct FamilyEnergy {
    Energy energy;
    const char* name;
};

constexpr std::array<FamilyEnergy, 6> kFamily{{
    {kEnergyReady, "Ready"},
    {kEnergyStopped, "Stopped"},
    {kEnergyPreparing, "Preparing"},
    {kEnergyCompleted, "Completed"},
    {kEnergyPaused, "Paused"},
    {kEnergyPrinting, "Printing"},
}};

// The REJECTED ramp, reproduced here and nowhere else, so the negative control below is aimed at a
// value this test derives rather than at a literal someone typed.
//
// This is exactly the arithmetic that was deleted from theme/tokens.hpp and
// scenes/operational_accent.cpp on 2026-09-01: a level of `opacity.low` of the full 0...255 scale
// at kEnergyMin rising to the full scale at kEnergyMax, applied to each channel of `color.accent`.
constexpr std::int32_t kRejectedLevelFull = 255;
constexpr std::int32_t kRejectedLevelRest =
    kRejectedLevelFull * prd::ui::theme::kOpacityLowPercent / prd::ui::theme::kOpacityFullPercent;

constexpr std::uint32_t rejected_ramp(const Energy energy) {
    const std::int32_t level =
        kRejectedLevelRest + (((kRejectedLevelFull - kRejectedLevelRest) * energy) / kEnergyMax);

    const auto scaled = [level](const std::uint32_t channel) {
        return (channel * static_cast<std::uint32_t>(level)) /
               static_cast<std::uint32_t>(kRejectedLevelFull);
    };

    return (scaled((prd::ui::theme::kColorAccent >> 16U) & 0xFFU) << 16U) |
           (scaled((prd::ui::theme::kColorAccent >> 8U) & 0xFFU) << 8U) |
           scaled(prd::ui::theme::kColorAccent & 0xFFU);
}

} // namespace

// --- The rule ---------------------------------------------------------------

TEST(OperationalAccentTests, TheAccentIsExactlyTheToken) {
    // §5.1: "There shall be one accent colour, `color.accent`." Exact equality, not approximate —
    // the accent IS the token, not a value near it.
    EXPECT_EQ(accent(), prd::ui::theme::kColorAccent);
    EXPECT_EQ(accent(), 0xF77F2FU);
}

TEST(OperationalAccentTests, TheAccentIsTheSameColourAtEveryEnergyInTheFamily) {
    // The rule stated over the axis, through the only surface that still relates the two: the six
    // family magnitudes exist, and not one of them changes the answer. Before the ruling this
    // suite asserted the exact opposite — that the six drew six different colours.
    std::set<std::uint32_t> seen;

    for (const auto& member : kFamily) {
        seen.insert(accent());
        EXPECT_EQ(accent(), prd::ui::theme::kColorAccent) << member.name;
    }

    EXPECT_EQ(seen.size(), 1U) << "the family draws more than one accent";
}

TEST(OperationalAccentTests, TheAccentIsStableAcrossRepeatedReads) {
    // Cheap, and it is the property the renderer depends on: progress_ring.cpp RETAINS the accent
    // for its report rather than recomputing it, and unavailable_motion.cpp writes it into every
    // frame. Both are only sound if the answer cannot move between calls.
    const std::uint32_t first = accent();

    for (int index = 0; index < 1000; ++index) {
        EXPECT_EQ(accent(), first) << "changed on read " << index;
    }
}

TEST(OperationalAccentTests, NoValueLeavesTheGamut) {
    // Nothing above the blue channel's 24 bits: a colour that had overflowed into a fourth byte
    // would still compare unequal to the token and might still draw something.
    EXPECT_EQ(accent() >> 24U, 0U);
}

TEST(OperationalAccentTests, TheAccentIsNeitherAbsentNorTheField) {
    // §2.2: "Minimum energy is not zero. Ready shall read as awake, connected and settled." At
    // rest the form is a drawn colour, not an absence and not the background.
    EXPECT_NE(accent(), 0U);
    EXPECT_NE(accent(), prd::ui::theme::kColorBackground);
}

// --- The firing negative control --------------------------------------------

TEST(OperationalAccentTests, TheRejectedRestingEndpointIsReproducibleAndIsNotWhatProductionDraws) {
    // ⛔ THE CONTROL, AND IT IS AIMED AT A DERIVED VALUE. The rejected ramp is recomputed above
    // from the tokens it used, and this asserts FIRST that it really does land on #623212 — the
    // colour the owner rejected on the panel. A control that only said "not #623212" would pass
    // even if the literal were wrong, and would then be guarding nothing.
    EXPECT_EQ(rejected_ramp(kEnergyReady), 0x623212U)
        << "the control has drifted off the value that was actually rejected";

    // ⬅️ AND NOW THE ASSERTION THAT FIRES. Production draws the token, not the rejected endpoint.
    // If the ramp were reinstated with the same endpoint, accent() would return #623212 and this
    // fails. If it were reinstated with a DIFFERENT endpoint, accent() would need an Energy
    // parameter and this file would not compile — which is the stronger half of the guard.
    EXPECT_NE(accent(), rejected_ramp(kEnergyReady));
    EXPECT_NE(accent(), 0x623212U);
}

TEST(OperationalAccentTests, ProductionDivergesFromTheRejectedRampAtEveryEnergyBelowMaximum) {
    // The ramp and the rule agree at exactly one point — kEnergyMax, where the old ramp reached
    // the full token. Everywhere below it they differ, and the gap widens toward rest. Sweeping
    // the whole axis rather than the six magnitudes, because §3.1 makes every intermediate energy
    // reachable and a partial reinstatement could hide between two family members.
    for (Energy energy = kEnergyMin; energy < kEnergyMax; ++energy) {
        ASSERT_NE(accent(), rejected_ramp(energy)) << "matches the rejected ramp at " << energy;
    }

    EXPECT_EQ(rejected_ramp(kEnergyMax), prd::ui::theme::kColorAccent)
        << "the two rules must still agree at maximum energy";
}

TEST(OperationalAccentTests, EveryChannelIsTheTokensOwnAndIsNotAttenuated) {
    // Channel by channel, because a partial attenuation — one channel scaled, two left alone —
    // would change the hue while leaving the composite close enough to pass a coarse check.
    EXPECT_EQ(red_of(accent()), red_of(prd::ui::theme::kColorAccent));
    EXPECT_EQ(green_of(accent()), green_of(prd::ui::theme::kColorAccent));
    EXPECT_EQ(blue_of(accent()), blue_of(prd::ui::theme::kColorAccent));

    // And strictly brighter than the rejected endpoint in every channel, which is the direction
    // the ruling moved: no second attenuation on top of the backlight.
    EXPECT_GT(red_of(accent()), red_of(rejected_ramp(kEnergyReady)));
    EXPECT_GT(green_of(accent()), green_of(rejected_ramp(kEnergyReady)));
    EXPECT_GT(blue_of(accent()), blue_of(rejected_ramp(kEnergyReady)));
}
