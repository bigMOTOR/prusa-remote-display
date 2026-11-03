// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "brightness_curve.hpp"
#include "brightness_harness_steps.hpp"

// The owner-paced brightness harness's step sequence.
//
// ⛔ WHAT IS PROTECTED HERE IS AN INVALIDATED BENCH RUN, NOT A HYPOTHETICAL. The first brightness
// instrument advanced itself every 8-15 seconds and named each step in chat. The announcements
// arrived batched, the owner could not tell which level was on screen, and the run was ruled
// INVALID for selection: no Active, Quiet or Dimmed level was recorded from it, and neither an
// upper-end saturation nor a broken brightness path may be concluded from it.
//
// The repair is that the instrument cannot advance itself and the panel carries the label. The
// first of those is a property of this table, and it is what most of the tests below assert.

using namespace prd::validation::brightness_harness;

namespace {

using prd::display::LightFraction;
using prd::display::percent_for_light_fraction;

// The exact labels the owner reads off the panel. Written out here rather than referenced from the
// table, so a table edit that reworded a screen fails instead of quietly agreeing with itself.
constexpr std::string_view kRequiredLabels[] = {
    "C1 • CEILING 50%",
    "C2 • CEILING 65%",
    "C3 • CEILING 80%",
    "C4 • CEILING 100%",
    "D1 • DIM 30% LIGHT",
    "D2 • DIM 20% LIGHT",
    "D3 • DIM 12% LIGHT",
    "D4 • DIM 6% LIGHT",
    "F1 • 80% • PRESS TO FADE",
    "F2 • 15% LIGHT",
    "F3 • PRESSING WILL FADE OFF",
    "OFF",
    "F4 • BACK ON 80%",
};

} // namespace

// --- Manual advancement only ------------------------------------------------

TEST(BrightnessHarnessTests, OnePressAdvancesExactlyOneStep) {
    for (std::size_t index = 0; index + 1 < kStepCount; ++index) {
        EXPECT_EQ(next_index(index), index + 1) << "index=" << index;
    }
}

TEST(BrightnessHarnessTests, TheSequenceWrapsFromTheLastStepBackToC1) {
    EXPECT_EQ(next_index(kStepCount - 1), kFirstStep);
    EXPECT_STREQ(kSteps[kFirstStep].id, "C1");
    EXPECT_STREQ(kSteps[kStepCount - 1].id, "F4");
}

// ⛔ THE NEGATIVE CONTROL FOR THE INVALIDATED INSTRUMENT, AND THE MOST IMPORTANT TEST HERE. The
// previous ladder advanced on a per-step DWELL. If a dwell ever returns to this table, the struct
// grows and this fails — so re-adding automatic advancement has to be argued rather than slipped
// in. `fade_ms` is not a dwell: it is the duration of the transition INTO a step, and it is zero
// for every step that is not a recession.
TEST(BrightnessHarnessTests, NoStepCarriesADwellSoNothingCanSelfAdvance) {
    static_assert(sizeof(Step) == sizeof(const char*) * 2 + sizeof(std::uint32_t) * 2,
                  "the harness step is {id, label, phase, ceiling, light, fade} and nothing else; "
                  "a larger struct means a dwell or an interval was added, which would reinstate "
                  "the self-advancing instrument the owner ruled invalid");

    for (std::size_t index = 0; index < kStepCount; ++index) {
        const std::uint32_t fade = kSteps[index].fade_ms;

        EXPECT_TRUE(fade == 0U || fade == kProductionFadeMs)
            << "a transition duration is either instant or the production fade; anything else is "
               "a dwell wearing the wrong name. index="
            << index << " fade_ms=" << fade;
    }
}

TEST(BrightnessHarnessTests, EveryStepIsReachableAndHeldIndefinitely) {
    // Walking kStepCount times from any start returns to that start, and visits every step once.
    // "Held indefinitely" is the absence of any other mover: next_index is the only function that
    // changes an index, and it takes no time argument at all.
    std::set<std::size_t> visited;
    std::size_t index = kFirstStep;

    for (std::size_t press = 0; press < kStepCount; ++press) {
        visited.insert(index);
        index = next_index(index);
    }

    EXPECT_EQ(index, kFirstStep) << "the walk must close the loop";
    EXPECT_EQ(visited.size(), kStepCount) << "every step must be reachable by pressing";
}

// --- The required screens ---------------------------------------------------

TEST(BrightnessHarnessTests, TheRequiredScreensAppearInTheRequiredOrder) {
    ASSERT_EQ(kStepCount, sizeof(kRequiredLabels) / sizeof(kRequiredLabels[0]));

    for (std::size_t index = 0; index < kStepCount; ++index) {
        EXPECT_EQ(std::string_view{kSteps[index].label}, kRequiredLabels[index])
            << "index=" << index;
    }
}

// ⛔ NEGATIVE CONTROL: the separator must be a glyph the 14 px face actually carries. U+00B7 MIDDLE
// DOT is absent from lv_font_montserrat_14 — its cmaps are U+0020-U+007E, then a sparse list from
// U+00B0 whose entries are U+00B0 and U+2022 onward — so a middle dot would draw as nothing and the
// instrument's own label would be unreadable. U+2022 BULLET is present and is what is used.
TEST(BrightnessHarnessTests, LabelsUseOnlyGlyphsTheDiagnosticFaceCarries) {
    for (std::size_t index = 0; index < kStepCount; ++index) {
        const std::string label{kSteps[index].label};

        EXPECT_EQ(label.find("\xc2\xb7"), std::string::npos)
            << "U+00B7 MIDDLE DOT is not in montserrat_14; index=" << index;

        // Everything must be ASCII apart from the bullet.
        std::string stripped = label;
        for (std::string::size_type at = stripped.find("\xe2\x80\xa2"); at != std::string::npos;
             at = stripped.find("\xe2\x80\xa2")) {
            stripped.erase(at, 3);
        }

        for (const char character : stripped) {
            EXPECT_GE(static_cast<unsigned char>(character), 0x20U) << "index=" << index;
            EXPECT_LE(static_cast<unsigned char>(character), 0x7EU)
                << "only ASCII and U+2022 are renderable here; index=" << index;
        }
    }

    // The identifiers are drawn by the 48 px face, whose only cmap is ASCII.
    for (std::size_t index = 0; index < kStepCount; ++index) {
        for (const char character : std::string{kSteps[index].id}) {
            EXPECT_GE(static_cast<unsigned char>(character), 0x20U);
            EXPECT_LE(static_cast<unsigned char>(character), 0x7EU);
        }
    }
}

// --- Ceilings and dims ------------------------------------------------------

TEST(BrightnessHarnessTests, TheCeilingStepsAscendAtFullLight) {
    const std::uint8_t expected[] = {50, 65, 80, 100};

    for (std::size_t index = 0; index < 4; ++index) {
        EXPECT_EQ(kSteps[index].phase, Phase::Ceiling);
        EXPECT_EQ(kSteps[index].ceiling_percent, expected[index]);
        EXPECT_EQ(kSteps[index].light_percent, 100)
            << "a ceiling step shows the ceiling itself, not a fraction of it";
        EXPECT_EQ(kSteps[index].fade_ms, 0U) << "ceiling steps step, they do not ramp";
    }
}

TEST(BrightnessHarnessTests, TheDimStepsDescendInLightAtOneReferenceCeiling) {
    const std::uint8_t expected[] = {30, 20, 12, 6};

    for (std::size_t offset = 0; offset < 4; ++offset) {
        const Step& step = kSteps[4 + offset];

        EXPECT_EQ(step.phase, Phase::Dim);
        EXPECT_EQ(step.light_percent, expected[offset]);
        EXPECT_EQ(step.ceiling_percent, kReferenceCeiling)
            << "judged at one ceiling: the gamma correction is what makes a light fraction mean "
               "the same thing at every ceiling";
    }
}

// The control percentages the panel is actually driven at, through the corrected conversion. Pinned
// so a curve change is visible in a diff rather than discovered on a panel.
TEST(BrightnessHarnessTests, EveryStepResolvesThroughTheCorrectedGammaConversion) {
    const std::uint8_t expected[] = {50, 65, 80, 100, 46, 38, 31, 22, 80, 34, 34, 0, 80};

    for (std::size_t index = 0; index < kStepCount; ++index) {
        const Step& step = kSteps[index];
        const std::uint8_t percent =
            percent_for_light_fraction(step.ceiling_percent, LightFraction{step.light_percent});

        EXPECT_EQ(percent, expected[index]) << "step=" << step.id;
    }
}

// --- The fade flow ----------------------------------------------------------

TEST(BrightnessHarnessTests, TheFadeFlowIsF1ThenF2ThenF3ThenOffThenF4) {
    EXPECT_EQ(next_index(kIndexF1), kIndexF2);
    EXPECT_EQ(next_index(kIndexF2), kIndexF3);
    EXPECT_EQ(next_index(kIndexF3), kIndexOff);
    EXPECT_EQ(next_index(kIndexOff), kIndexF4);
    EXPECT_EQ(next_index(kIndexF4), kFirstStep) << "and then back to C1";

    EXPECT_STREQ(kSteps[kIndexF1].id, "F1");
    EXPECT_STREQ(kSteps[kIndexF2].id, "F2");
    EXPECT_STREQ(kSteps[kIndexF3].id, "F3");
    EXPECT_STREQ(kSteps[kIndexOff].id, "OFF");
    EXPECT_STREQ(kSteps[kIndexF4].id, "F4");
}

TEST(BrightnessHarnessTests, OnlyTheTwoRecessionsFadeAndTheyUseTheProductionDuration) {
    EXPECT_EQ(kSteps[kIndexF2].fade_ms, kProductionFadeMs);
    EXPECT_EQ(kSteps[kIndexOff].fade_ms, kProductionFadeMs);

    EXPECT_EQ(kSteps[kIndexF1].fade_ms, 0U) << "F1 is already lit; there is nothing to ramp into";
    EXPECT_EQ(kSteps[kIndexF3].fade_ms, 0U) << "F3 changes the words, not the level";
    EXPECT_EQ(kSteps[kIndexF4].fade_ms, 0U) << "a wake lands at once — Production UI §10";

    EXPECT_TRUE(fades_in(kIndexF2));
    EXPECT_TRUE(fades_in(kIndexOff));
    EXPECT_FALSE(fades_in(kIndexF1));
    EXPECT_FALSE(fades_in(kIndexF3));
    EXPECT_FALSE(fades_in(kIndexF4));
}

// ⛔ F3 IS A WARNING, NOT A LEVEL, AND THAT IS THE POINT OF IT. The owner has to be able to READ
// that the next press will darken the panel while the panel is still lit. If F3 ever acquires its
// own brightness, the warning becomes a transition and the sentence stops being true.
TEST(BrightnessHarnessTests, F3WarnsAtF2sBrightnessWithoutChangingIt) {
    EXPECT_EQ(kSteps[kIndexF3].light_percent, kSteps[kIndexF2].light_percent);
    EXPECT_EQ(kSteps[kIndexF3].ceiling_percent, kSteps[kIndexF2].ceiling_percent);

    EXPECT_NE(std::string{kSteps[kIndexF3].label}.find("WILL FADE OFF"), std::string::npos);
}

TEST(BrightnessHarnessTests, OffIsTrueOffAndIsReachedOnlyFromF3) {
    EXPECT_EQ(kSteps[kIndexOff].light_percent, 0);
    EXPECT_TRUE(is_off(kIndexOff));

    // Exactly one step is off, and its only predecessor is F3.
    std::size_t off_steps = 0;
    for (std::size_t index = 0; index < kStepCount; ++index) {
        if (is_off(index)) {
            ++off_steps;
        }

        if (next_index(index) == kIndexOff) {
            EXPECT_EQ(index, kIndexF3) << "nothing but F3 may lead into the dark step";
        }
    }

    EXPECT_EQ(off_steps, 1U) << "one off step, so a press while dark has exactly one meaning";
}

// ⛔ THE WAKE FROM A DARK PANEL, WHICH IS THE ONE PRESS THAT HAS NEVER BEEN MEASURED ON HARDWARE.
// While off, the next press must produce a VISIBLE step rather than another dark one — otherwise a
// reviewer holding a dark panel has no way to know the harness is still alive.
TEST(BrightnessHarnessTests, APressWhileOffWakesToAVisibleF4) {
    const std::size_t after_off = next_index(kIndexOff);

    EXPECT_EQ(after_off, kIndexF4);
    EXPECT_FALSE(is_off(after_off)) << "the step after off must be lit";
    EXPECT_EQ(kSteps[after_off].light_percent, 100);
    EXPECT_EQ(kSteps[after_off].fade_ms, 0U) << "and it must land at once, not fade up";
    EXPECT_EQ(percent_for_light_fraction(kSteps[after_off].ceiling_percent,
                                         LightFraction{kSteps[after_off].light_percent}),
              80);
}

TEST(BrightnessHarnessTests, PhaseLabelsCoverEveryPhaseAndNameNoUnknown) {
    for (std::size_t index = 0; index < kStepCount; ++index) {
        EXPECT_STRNE(phase_label(kSteps[index].phase), "unknown") << "index=" << index;
    }

    EXPECT_STREQ(phase_label(Phase::Ceiling), "ceiling");
    EXPECT_STREQ(phase_label(Phase::Dim), "dim");
    EXPECT_STREQ(phase_label(Phase::Fade), "fade");
}
