// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>

#include "scenes/operational_energy.hpp"
#include "scenes/operational_luminance.hpp"
#include "theme/tokens.hpp"

// The composition's luminance ramp — UI Design Decision 004 §4, divergence OF-7.
//
// §4 is the one rule in the family whose correctness cannot be settled here: "luminance is
// floored by legibility of primary operational information at approximately one to two metres"
// is a measurement against a physical panel, and §15 reserves it for exactly that. So nothing
// below asserts that the floor is the RIGHT floor. What is asserted is everything that must
// hold whatever the floor turns out to be — that it exists, that the ramp reaches it, that it
// never goes under it, and that the top of the ramp leaves Printing untouched.

using namespace prd::ui::scene;

namespace {

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

} // namespace

// --- The endpoints ----------------------------------------------------------

TEST(OperationalLuminanceTests, MaximumEnergyLeavesTheCompositionUnscaled) {
    // THE CONTROL. At the top of the ramp the level is the full scale, so every element keeps
    // exactly the §5 opacity it was built with and Printing renders as it did before Phase F.
    // Anything seen to change there on the panel is a defect, not a design consequence.
    EXPECT_EQ(body_level(kEnergyMax), prd::ui::theme::kBodyLevelFull);
}

TEST(OperationalLuminanceTests, MinimumEnergySitsAtTheFloorAndNotBelowIt) {
    EXPECT_EQ(body_level(kEnergyMin), prd::ui::theme::kBodyLevelRest);

    // §4: "Low energy shall never mean difficult to read." The floor is a working baseline and
    // this does not assert its value — only that the bottom of the axis is a floor rather than
    // an extinction, which is the property §4 states and §2.2 repeats for energy itself.
    EXPECT_GT(body_level(kEnergyMin), 0);
}

TEST(OperationalLuminanceTests, TheCompositionActuallyRecedes) {
    // OF-7 in one assertion. Before Phase F no luminance varied by state at all, so Ready and
    // Printing drew their text at identical weight; §13.2 lists "the luminance of the
    // composition as one body" among the things that VARY across the six states.
    EXPECT_LT(body_level(kEnergyMin), body_level(kEnergyMax));
}

TEST(OperationalLuminanceTests, EnergiesOutsideTheAxisClampToItsEnds) {
    EXPECT_EQ(body_level(kEnergyMin - 1), body_level(kEnergyMin));
    EXPECT_EQ(body_level(kEnergyMax + 1), body_level(kEnergyMax));
    EXPECT_EQ(body_level(kEnergyMin - 10000), body_level(kEnergyMin));
    EXPECT_EQ(body_level(kEnergyMax + 10000), body_level(kEnergyMax));
}

// --- Continuity across the whole axis ---------------------------------------

TEST(OperationalLuminanceTests, TheLevelRisesMonotonicallyAndContinuouslyAcrossTheWholeAxis) {
    // The whole range rather than the six magnitudes: those are a working baseline Phase H will
    // move, and §11's motion traverses everything between them. A discontinuity here would be a
    // visible step in an animation that §11 requires to be continuous.
    std::int32_t previous = body_level(kEnergyMin);

    for (Energy energy = kEnergyMin; energy <= kEnergyMax; ++energy) {
        const std::int32_t level = body_level(energy);

        EXPECT_GE(level, previous) << "fell at energy " << energy;
        EXPECT_LE(level - previous, 1) << "stepped at energy " << energy;

        previous = level;
    }
}

TEST(OperationalLuminanceTests, NoEnergyEscapesTheFloorOrTheCeiling) {
    // The invariant §4 states, checked where it must hold rather than only at the ends. A ramp
    // that overshot the top would brighten an element past the §5 step its role was given.
    for (Energy energy = kEnergyMin; energy <= kEnergyMax; ++energy) {
        EXPECT_GE(body_level(energy), prd::ui::theme::kBodyLevelRest) << "under at " << energy;
        EXPECT_LE(body_level(energy), prd::ui::theme::kBodyLevelFull) << "over at " << energy;
    }
}

// --- The family -------------------------------------------------------------

TEST(OperationalLuminanceTests, TheSixEnergiesAllReceedeToDifferentLevels) {
    std::set<std::int32_t> seen;

    for (const auto& member : kFamily) {
        EXPECT_TRUE(seen.insert(body_level(member.energy)).second)
            << member.name << " repeats an earlier level";
    }

    EXPECT_EQ(seen.size(), kFamily.size());
}

TEST(OperationalLuminanceTests, TheSixEnergiesAscendInTheOrderSection22Gives) {
    // §2.2 gives the ordering and nothing else; the ramp must not invert it. kFamily is read
    // upward, from Ready carrying nothing to Printing carrying a job.
    for (std::size_t index = 1; index < kFamily.size(); ++index) {
        EXPECT_GT(body_level(kFamily[index].energy), body_level(kFamily[index - 1].energy))
            << kFamily[index].name << " does not exceed " << kFamily[index - 1].name;
    }
}

// --- One body ---------------------------------------------------------------

TEST(OperationalLuminanceTests, ScalingPreservesTheHierarchyBetweenRoles) {
    // "AS ONE BODY" IS THE POINT, and this is what it means arithmetically: one factor applied
    // to every element, so §5's steps keep their order and their proportion at every energy
    // instead of converging toward a single value. The scene and the telemetry column both
    // multiply their own §5 token by this level; the ordering below is what that must preserve.
    //
    // Modelled here rather than measured in the scene, which is LVGL and cannot be compiled in
    // this build — the same reason the content table exists as a pure decision.
    const auto scaled = [](const std::uint8_t token_percent, const std::int32_t level) {
        return (token_percent * level) / prd::ui::theme::kBodyLevelFull;
    };

    for (Energy energy = kEnergyMin; energy <= kEnergyMax; ++energy) {
        const std::int32_t level = body_level(energy);

        const std::int32_t full = scaled(prd::ui::theme::kOpacityFullPercent, level);
        const std::int32_t high = scaled(prd::ui::theme::kOpacityHighPercent, level);
        const std::int32_t medium = scaled(prd::ui::theme::kOpacityMediumPercent, level);

        EXPECT_GT(full, high) << "hierarchy collapsed at energy " << energy;
        EXPECT_GT(high, medium) << "hierarchy collapsed at energy " << energy;
    }
}
