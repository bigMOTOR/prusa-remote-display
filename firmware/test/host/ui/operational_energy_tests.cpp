// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <set>

#include "app_model/presentation.hpp"
#include "fake_source/fixtures.hpp"
#include "scenes/operational_energy.hpp"

// The operational energy axis — UI Design Decision 004 §2.
//
// The axis is the whole of UIDD 004's mechanism: §1 says only energy is drawn, so every
// downstream decision about shape (§3) and composition luminance (§4) reads this and nothing else.
// The accent does NOT — §5.1, as ruled on 2026-09-01, gives the form one colour at every energy. A
// wrong ordering here would be invisible in the code that consumes it and would appear on the panel
// as six states that look almost right.
//
// §2.2's ordering is the one property here that is NOT provisional. The magnitudes are a
// working baseline settled in Phase H; the order they sit in is an accepted Project Source.
// The suite is written to that distinction: it asserts the order exhaustively and asserts
// nothing about the spacing.
//
// The second property it protects is the family boundary. §Scope confines operational energy
// to six states, so membership is a fact the type carries — an energy either exists for a
// state or does not — and "outside the family" must never be expressible as a low energy.

using namespace prd::ui::scene;
using prd::app_model::project;
using prd::app_model::SubjectState;
using prd::fake_source::Fixture;

namespace {

struct FamilyMember {
    Fixture fixture;
    SubjectState subject;
    const char* name;
};

// §2.2's table, highest energy first. The order of this array IS the assertion in
// TheSixEnergiesDescendInTheOrderTheDecisionGives, so it is written in the decision's order
// rather than the artwork's.
constexpr std::array<FamilyMember, 6> kFamilyByEnergy{{
    {Fixture::Printing, SubjectState::Printing, "Printing"},
    {Fixture::Paused, SubjectState::Paused, "Paused"},
    {Fixture::Completed, SubjectState::Completed, "Completed"},
    {Fixture::Preparing, SubjectState::Preparing, "Preparing"},
    {Fixture::Stopped, SubjectState::Stopped, "Stopped"},
    {Fixture::Ready, SubjectState::Ready, "Ready"},
}};

// The axis, for a state that must be on it.
//
// Fails the calling test and returns an OFF-SCALE value rather than a plausible one, so a
// missing energy cannot be silently read as a low energy — which is the exact confusion the
// optional exists to prevent, and it would be perverse for the suite guarding that property to
// reintroduce it internally.
Energy energy_of(const SubjectState state) {
    const std::optional<Energy> energy = energy_for(state);
    EXPECT_TRUE(energy.has_value()) << "state has no operational energy";

    return energy.value_or(kEnergyMax + 1);
}

} // namespace

// --- §2.2, the ordering -----------------------------------------------------

TEST(OperationalEnergyTests, TheSixEnergiesDescendInTheOrderTheDecisionGives) {
    // §2.2, ordered from highest to lowest: Printing, Paused, Completed, Preparing, Stopped,
    // Ready. Asserted pairwise over the table rather than as six literals, so the test states
    // the property — strictly descending — instead of restating the numbers it is guarding.
    for (std::size_t index = 1; index < kFamilyByEnergy.size(); ++index) {
        const Energy higher = energy_of(kFamilyByEnergy[index - 1].subject);
        const Energy lower = energy_of(kFamilyByEnergy[index].subject);

        EXPECT_GT(higher, lower) << kFamilyByEnergy[index - 1].name
                                 << " must carry more energy than " << kFamilyByEnergy[index].name;
    }
}

TEST(OperationalEnergyTests, PrintingIsTheMaximumAndReadyTheMinimum) {
    // The two endpoints. They are what anchors the axis to the two states with accepted
    // artwork — UIDD 001's Printing composition and UIDD 002's resting expression — so the
    // geometry derived from them in E.2 reproduces the current baseline exactly and those two
    // states act as controls on the panel.
    EXPECT_EQ(energy_of(SubjectState::Printing), kEnergyMax);
    EXPECT_EQ(energy_of(SubjectState::Ready), kEnergyMin);
}

TEST(OperationalEnergyTests, EveryEnergyIsDistinct) {
    // Six states, six energies. Two states sharing a magnitude would be two states drawn
    // identically, which is §1's law failing in the direction nobody notices: the composition
    // would be consistent and the family would have lost a distinction.
    std::set<Energy> seen;
    for (const auto& member : kFamilyByEnergy) {
        EXPECT_TRUE(seen.insert(energy_of(member.subject)).second)
            << member.name << " shares an energy with another state";
    }

    EXPECT_EQ(seen.size(), kFamilyByEnergy.size());
}

TEST(OperationalEnergyTests, EveryEnergyLiesOnTheScale) {
    for (const auto& member : kFamilyByEnergy) {
        const Energy energy = energy_of(member.subject);

        EXPECT_GE(energy, kEnergyMin) << member.name;
        EXPECT_LE(energy, kEnergyMax) << member.name;
    }
}

// --- The family boundary ----------------------------------------------------

TEST(OperationalEnergyTests, EveryFamilyMemberHasAnOperationalEnergy) {
    // The positive half of the boundary. §Scope names six states, and all six must be on the
    // axis — a member without an energy would be a state the composition cannot draw.
    for (const auto& member : kFamilyByEnergy) {
        EXPECT_TRUE(energy_for(member.subject).has_value()) << member.name;
    }
}

TEST(OperationalEnergyTests, AcquiringStateIsOutsideTheOperationalFamily) {
    // The negative half. UIDD 004's Scope confines operational energy to the six operational
    // states and says it "shall not be used to express anything outside this family".
    // AcquiringState belongs to the acquisition family (UIDD 003) and Production UI §9 gives
    // it its own Connecting screen.
    //
    // It therefore has NO energy — not a low one. Asserting emptiness rather than a magnitude
    // is the point: if this ever returned kEnergyMin, a non-member would be indistinguishable
    // from Ready, which is a real point on the axis with an accepted resting form.
    EXPECT_FALSE(energy_for(SubjectState::AcquiringState).has_value());
}

TEST(OperationalEnergyTests, TheAxisHoldsExactlyTheSixOperationalStates) {
    // Membership stated as a count rather than as six separate facts, so a seventh state
    // quietly acquiring an energy fails here even if every other assertion still passes.
    std::size_t on_the_axis = 0;
    for (const SubjectState state :
         {SubjectState::AcquiringState, SubjectState::Ready, SubjectState::Preparing,
          SubjectState::Printing, SubjectState::Paused, SubjectState::Completed,
          SubjectState::Stopped}) {
        if (energy_for(state).has_value()) {
            ++on_the_axis;
        }
    }

    EXPECT_EQ(on_the_axis, kFamilyByEnergy.size());
}

TEST(OperationalEnergyTests, EveryFamilyMemberIsReachedByItsOwnFixture) {
    // Guards the rest of the suite. If a fixture stopped producing the state it is named for,
    // every assertion above would be testing the wrong state and still passing — the same trap
    // the content suite guards against, and worth repeating because this suite's subject is an
    // ordering rather than a value.
    for (const auto& member : kFamilyByEnergy) {
        EXPECT_EQ(project(prd::fake_source::state_for(member.fixture)).subject, member.subject)
            << member.name;
    }
}

// --- What this suite deliberately does not assert ---------------------------

TEST(OperationalEnergyTests, TheSpacingIsNotAsserted) {
    // §15 makes "the magnitude of each of the six energies" a design act to be performed
    // against the physical panel. The magnitudes are therefore a working baseline settled in
    // Phase H, and a test that pinned the spacing would turn a provisional value into a
    // regression to be argued with.
    //
    // What is pinned instead is the scale's shape — the endpoints and the ordering — which is
    // what the accepted decision actually fixes. This test exists to say so where someone
    // tempted to add EXPECT_EQ(energy_for(Paused), 800) will read it.
    const Energy paused = energy_of(SubjectState::Paused);
    const Energy completed = energy_of(SubjectState::Completed);

    EXPECT_GT(paused, completed);
    EXPECT_LT(paused, kEnergyMax);
    EXPECT_GT(completed, kEnergyMin);
}
