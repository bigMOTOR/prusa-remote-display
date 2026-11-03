// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <cstdint>

#include "app_model/display_policy.hpp"
#include "app_model/reducer.hpp"
#include "brightness_curve.hpp"
#include "support/state_builders.hpp"

// The display-power policy — Architecture Spec §7.11/§17.8, Product Brief v0.2 "Display
// behaviour", UI Behaviour Decision 001 §3/§4/§5.
//
// What is protected here is a rejected product experience, not a hypothetical. The first Printer
// Unavailable candidate reached the owner as a permanently lit, full-contrast sentence and was
// rejected on exactly that ground: correct as a system message, wrong as a thing that sits on a
// desk for days. The repair is that unavailability is a MOMENT rather than a STATE — acknowledged
// once, then receding to a dark panel while monitoring continues.
//
// Two properties below are structural rather than asserted, and are worth naming because that is
// what makes them strong:
//
//   • THE POLICY CANNOT INVENT A SLEEP SCREEN. Its whole output is a Step and a brightness. It has
//     no scene, no text id and no composition, so recession changes what the panel is LIT BY and
//     never what it DRAWS. There is nothing to assert because there is nothing to draw with.
//   • THE POLICY CANNOT STOP MONITORING. It touches no radio, no worker, no task and no queue —
//     it returns a value. Display power is not device power, which is §7.11's closing rule.

using namespace prd::app_model;
using namespace prd::app_model::display_policy;
using prd::test::at_ms;

namespace {

constexpr Timings kT{};
constexpr Brightness kB{};

// The recession's two boundaries, from the accepted timeline of UIBD 001 §10.2 and Architecture
// Spec §17.8.3.
//
// ⛔ THE SLEEP BOUNDARY INCLUDES THE FIRST FADE, because dim_hold_ms is STABLE time at Dimmed and
// §10.2 says the 300 s "begins only after the first fade has completed". The phases are
//
//     0 .. 20   Quiet     20 .. 22   fade     22 .. 322  STABLE Dimmed     322 .. 324  fade
//
// so Off is reached at t = 324 s. Writing this as acknowledge + hold would assert 320 s and give
// 298 s of stable Dimmed, which is the defect this checkpoint corrected in next_step().
constexpr std::uint32_t kDimAt = kT.acknowledge_ms;                                 // 20 s
constexpr std::uint32_t kSleepAt = kT.acknowledge_ms + kT.fade_ms + kT.dim_hold_ms; // 322 s
constexpr std::uint32_t kOffAt = kSleepAt + kT.fade_ms;                             // 324 s

AppState unavailable_device() {
    AppState state = prd::test::booted_device();
    state.confidence = Confidence::Unavailable;
    state.subject = SubjectState::Ready;
    state.subject_condition = SubjectCondition::None;
    return state;
}

AppState ready_device() {
    AppState state = unavailable_device();
    state.confidence = Confidence::Current;
    return state;
}

// The panel's control levels for a ceiling, composed EXACTLY as app_core composes them: the
// display component converts the light fraction, and the policy is handed the result.
//
// ⬅️ THE REAL CONVERSION, NOT A STAND-IN. brightness_curve.cpp compiles into this suite, so what
// runs here is the same function the firmware calls and the same gamma the backlight applies. A
// hand-written approximation would let the two drift, and drifting brightness is silent.
Levels levels_with_quiet(const std::uint8_t quiet) {
    Levels levels;
    levels.active_percent = kB.active_percent;
    levels.quiet_percent = quiet;
    levels.dimmed_percent = prd::display::percent_for_light_fraction(
        quiet, prd::display::LightFraction{kB.dimmed_light_percent_of_quiet});
    return levels;
}

Levels production_levels() {
    return levels_with_quiet(kB.quiet_percent);
}

// What the defective rule computed, kept so the two behaviours can be compared rather than
// described. See BrightnessCurve.ControlSpaceScalingEmitsAboutATenthOfTheLightItNames for the
// measurement of what it did to the panel.
std::uint8_t control_space_scaling(const std::uint8_t ceiling, const std::uint8_t light) {
    const std::uint32_t scaled = (static_cast<std::uint32_t>(ceiling) * light) / 100U;
    return static_cast<std::uint8_t>(scaled == 0U ? 1U : scaled);
}

} // namespace

// --- The recession ----------------------------------------------------------

TEST(DisplayPolicyTests, UnavailableAcknowledgesOnceThenRecedesOnToDimmedToOff) {
    const AppState state = unavailable_device();
    ASSERT_FALSE(holds_awake(state)) << "an unreachable printer is not actionable";

    // The acknowledgement: lit, and staying lit, for the whole hold.
    EXPECT_EQ(next_step(DisplayPower::On, 0, false, kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt - 1, false, kT), Step::Hold);

    // Then down, one level at a time, and never further in one decision.
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt, false, kT), Step::Dim);
    EXPECT_EQ(next_step(DisplayPower::On, kSleepAt + 60'000, false, kT), Step::Dim)
        << "a long-quiet panel still goes On -> Dimmed first; the ladder has no shortcut";

    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt - 1, false, kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt, false, kT), Step::Sleep);

    // And Off is the floor.
    EXPECT_EQ(next_step(DisplayPower::Off, kSleepAt * 10, false, kT), Step::Hold);
}

// ⛔ NEGATIVE CONTROL: nothing may climb. A timer that could raise brightness would be the ambient
// loop UI Behaviour Decision 001 §9 prohibits outright.
TEST(DisplayPolicyTests, TheRecessionNeverAscends) {
    for (const std::uint32_t idle : {0U, 1U, kDimAt, kSleepAt, kSleepAt * 4}) {
        EXPECT_NE(next_step(DisplayPower::Off, idle, false, kT), Step::Dim);
        EXPECT_NE(next_step(DisplayPower::Off, idle, false, kT), Step::Sleep);
        EXPECT_EQ(next_step(DisplayPower::Off, idle, false, kT), Step::Hold);
    }
}

// --- What holds the panel awake ---------------------------------------------

TEST(DisplayPolicyTests, PreparingPrintingAndPausedDoNotIdleSleep) {
    for (const auto subject :
         {SubjectState::Preparing, SubjectState::Printing, SubjectState::Paused}) {
        AppState state = ready_device();
        state.subject = subject;

        EXPECT_TRUE(holds_awake(state)) << "subject=" << static_cast<int>(subject);
        EXPECT_EQ(next_step(DisplayPower::On, kSleepAt * 10, holds_awake(state), kT), Step::Hold);
    }
}

TEST(DisplayPolicyTests, ReadyAndQuietTerminalStatesShareTheOrdinaryDimBoundary) {
    for (const auto subject :
         {SubjectState::Ready, SubjectState::Completed, SubjectState::Stopped}) {
        AppState state = ready_device();
        state.subject = subject;

        EXPECT_FALSE(holds_awake(state)) << "subject=" << static_cast<int>(subject);
        EXPECT_EQ(next_step(DisplayPower::On, kDimAt, holds_awake(state), kT), Step::Dim);
    }
}

TEST(DisplayPolicyTests, OnlyPresentableCompletedSelectsTwentyMinutesOfStableDimmed) {
    Timings ordinary{17U, 23U, 29U};
    for (const auto subject :
         {SubjectState::AcquiringState, SubjectState::Ready, SubjectState::Preparing,
          SubjectState::Printing, SubjectState::Paused, SubjectState::Completed,
          SubjectState::Stopped}) {
        for (const auto confidence :
             {Confidence::Current, Confidence::Degraded, Confidence::Unavailable}) {
            const auto timings = recession_timings_for(subject, confidence, ordinary);
            EXPECT_EQ(timings.acknowledge_ms, ordinary.acknowledge_ms);
            EXPECT_EQ(timings.fade_ms, ordinary.fade_ms);
            EXPECT_EQ(timings.dim_hold_ms,
                      subject == SubjectState::Completed && confidence != Confidence::Unavailable
                          ? 1'200'000U
                          : ordinary.dim_hold_ms);
        }
    }
}

TEST(DisplayPolicyTests, CompletedSleepBoundaryIncludesFirstFadeAndDoesNotOverrideHolds) {
    AppState state = ready_device();
    state.subject = SubjectState::Completed;
    const auto timings = recession_timings_for(state.subject, state.confidence, kT);
    constexpr std::uint32_t sleep_at = 1'222'000U;
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt - 1U, false, timings), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt, false, timings), Step::Dim);
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt, false, timings), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::Dimmed, sleep_at - 1U, false, timings), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::Dimmed, sleep_at, false, timings), Step::Sleep);
    EXPECT_EQ(next_step(DisplayPower::Off, sleep_at, false, timings), Step::Hold);
    state.subject_condition = SubjectCondition::Attention;
    ASSERT_TRUE(holds_awake(state));
    EXPECT_EQ(next_step(DisplayPower::On, sleep_at * 2U, holds_awake(state), timings), Step::Hold);
    // No retained Completed duration once the live printer is Ready or unavailable.
    state.subject = SubjectState::Ready;
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt, false,
                        recession_timings_for(state.subject, state.confidence, kT)),
              Step::Sleep);
    state.subject = SubjectState::Completed;
    state.confidence = Confidence::Unavailable;
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt, false,
                        recession_timings_for(state.subject, state.confidence, kT)),
              Step::Sleep);
}

// ⛔ NEGATIVE CONTROL, AND A SUBTLE ONE. A printer that was printing and has since gone unreachable
// must not light the desk indefinitely on the strength of a job the product can no longer see.
TEST(DisplayPolicyTests, ALostPrintDoesNotHoldThePanelAwake) {
    AppState state = ready_device();
    state.subject = SubjectState::Printing;
    ASSERT_TRUE(holds_awake(state));

    state.confidence = Confidence::Unavailable;
    EXPECT_FALSE(holds_awake(state))
        << "at Unavailable there is no Printing to hold the panel for — UIAD 001 §7";
}

TEST(DisplayPolicyTests, ActionableAndUserOwnedStatesRemainOn) {
    {
        AppState state = ready_device();
        state.subject_condition = SubjectCondition::Attention;
        EXPECT_TRUE(holds_awake(state));
    }
    {
        AppState state = ready_device();
        state.subject_condition = SubjectCondition::Error;
        EXPECT_TRUE(holds_awake(state));
    }
    {
        AppState state = ready_device();
        state.device_fault = DeviceFaultReason::WifiUnavailable;
        EXPECT_TRUE(holds_awake(state));
    }
    {
        AppState state = ready_device();
        state.provisioning = ProvisioningPhase::SetupActive;
        EXPECT_TRUE(holds_awake(state)) << "a setup panel shows the network name and password";
    }
    {
        AppState state = ready_device();
        state.modal_surface = ModalSurface::FactoryResetConfirmation;
        EXPECT_TRUE(holds_awake(state)) << "a question with an unlit panel is a trap";
    }
    {
        AppState state = ready_device();
        state.access_condition = AccessCondition::AuthenticationRequired;
        EXPECT_TRUE(holds_awake(state));
    }
}

// ⛔ NEGATIVE CONTROL: a held state must survive an arbitrarily long quiet period.
TEST(DisplayPolicyTests, AHeldPanelNeverRecedesHoweverLongItIsIgnored) {
    AppState state = ready_device();
    state.subject_condition = SubjectCondition::Error;

    for (const auto power : {DisplayPower::On, DisplayPower::Dimmed}) {
        EXPECT_EQ(next_step(power, kSleepAt * 100, holds_awake(state), kT), Step::Hold);
    }
}

// --- Routine traffic is not activity ----------------------------------------

// ⛔ THE LOAD-BEARING NEGATIVE CONTROL FOR UIBD 001 §4. Situation is what app_core compares to
// decide whether anything happened. If a telemetry value could change it, every poll would restart
// the recession and the panel would never sleep — which is the rejected experience by another
// route.
TEST(DisplayPolicyTests, RoutinePollsAndTelemetryDoNotCountAsActivity) {
    AppState before = ready_device();
    before.subject = SubjectState::Printing;

    AppState after = before;

    // Everything a poll can move: progress, temperatures, job facts, freshness bookkeeping.
    after.current_job.progress.known = true;
    after.current_job.progress.percent = 42;
    after.temperatures.nozzle_current.known = true;
    after.temperatures.nozzle_current.celsius = 215;
    after.temperatures.bed_current.known = true;
    after.temperatures.bed_current.celsius = 60;
    after.printer_connection.freshness = DataFreshness::Aging;
    after.printer_connection.last_latency_ms = 27;

    EXPECT_EQ(situation_of(before), situation_of(after))
        << "a telemetry refresh must be invisible to the display policy";
}

TEST(DisplayPolicyTests, EveryStructuralChangeIsActivity) {
    const AppState base = ready_device();

    {
        AppState next = base;
        next.confidence = Confidence::Unavailable;
        EXPECT_NE(situation_of(base), situation_of(next)) << "printer unavailable";
    }
    {
        AppState next = base;
        next.subject = SubjectState::Printing;
        EXPECT_NE(situation_of(base), situation_of(next)) << "print started";
    }
    {
        AppState next = base;
        next.subject_condition = SubjectCondition::Attention;
        EXPECT_NE(situation_of(base), situation_of(next)) << "attention required";
    }
    {
        AppState next = base;
        next.provisioning = ProvisioningPhase::SetupActive;
        EXPECT_NE(situation_of(base), situation_of(next)) << "setup opened";
    }
    {
        AppState next = base;
        next.modal_surface = ModalSurface::FactoryResetConfirmation;
        EXPECT_NE(situation_of(base), situation_of(next)) << "confirmation armed";
    }
}

// Recovery is the mirror of the entry, and UIBD 001 §4 names it explicitly.
TEST(DisplayPolicyTests, CommunicationRestoredIsAMeaningfulChange) {
    const AppState lost = unavailable_device();
    const AppState restored = ready_device();

    EXPECT_NE(situation_of(lost), situation_of(restored));
    EXPECT_FALSE(holds_awake(restored)) << "and then it recedes again like any quiet state";
}

// --- Wrap safety ------------------------------------------------------------

// ⛔ NEGATIVE CONTROL: the 32-bit millisecond counter wraps after 49.7 days. Without wrap-safe
// arithmetic a device quiet across that boundary is handed an interval of nearly the whole range
// and sleeps the instant it wakes.
TEST(DisplayPolicyTests, CounterWrapNeitherExtendsNorEndsAPhasePrematurely) {
    constexpr std::uint32_t kMax = 0xFFFFFFFFU;

    // Activity 10 ms before the wrap, now 10 ms after it: 20 ms of real time.
    EXPECT_EQ(elapsed_ms(9U, kMax - 10U), 20U);

    // The phase boundary is honoured across the wrap in both directions.
    const std::uint32_t activity = kMax - 5U;
    EXPECT_EQ(next_step(DisplayPower::On, elapsed_ms(activity + kDimAt - 1U, activity), false, kT),
              Step::Hold)
        << "a wrap must not end the acknowledgement early";
    EXPECT_EQ(next_step(DisplayPower::On, elapsed_ms(activity + kDimAt, activity), false, kT),
              Step::Dim)
        << "a wrap must not extend the acknowledgement either";
}

// ⛔ NEGATIVE CONTROL: a backwards reading is not an interval. Unsigned subtraction reports it as
// almost 2^32, which is how Milestone 3.11 T4 tore down an access point 8 ms after raising it.
TEST(DisplayPolicyTests, ABackwardsReadingYieldsNoElapsedTime) {
    EXPECT_EQ(elapsed_ms(1000U, 5000U), 0U);
    EXPECT_EQ(next_step(DisplayPower::On, elapsed_ms(1000U, 5000U), false, kT), Step::Hold);
}

// --- Active, Quiet and Dimmed ----------------------------------------------
//
// ⛔ THE LEVELS ARE OWNER-SELECTED PHYSICAL EVIDENCE, AND THE NUMBERS ARE PINNED HERE FOR THAT
// REASON. Walked on an owner-paced harness on 2026-08-30 — every step held indefinitely, advanced
// only by a short press, labelled ON THE PANEL — the owner selected Active = C2 (control 65, duty
// 1588), Quiet = C1 (control 50, duty 891) and Dimmed = D2 (duty 487). No serial capture was
// attached to that session and none is claimed; the appearance was the owner's direct judgment and
// the numeric mapping is the host-tested step table plus the compiled curve.

TEST(DisplayPolicyTests, HeldAwakeStatesAreActiveAndOrdinaryOnStatesAreQuiet) {
    const Levels levels = production_levels();

    // Printing, setup, actionable conditions, a modal surface and a device fault: someone is
    // expected to read the panel.
    {
        AppState state = ready_device();
        state.subject = SubjectState::Printing;
        ASSERT_TRUE(holds_awake(state));
        EXPECT_EQ(tier_for(holds_awake(state)), Tier::Active);
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 65);
    }
    {
        AppState state = ready_device();
        state.provisioning = ProvisioningPhase::SetupActive;
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 65);
    }
    {
        AppState state = ready_device();
        state.subject_condition = SubjectCondition::Attention;
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 65);
    }
    {
        AppState state = ready_device();
        state.modal_surface = ModalSurface::FactoryResetConfirmation;
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 65);
    }
    {
        AppState state = ready_device();
        state.device_fault = DeviceFaultReason::WifiUnavailable;
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 65);
    }

    // Ready, the Completed acknowledgement, Stopped and an unreachable printer: present and
    // unobtrusive, and on their way to Dimmed.
    for (const auto subject :
         {SubjectState::Ready, SubjectState::Completed, SubjectState::Stopped}) {
        AppState state = ready_device();
        state.subject = subject;

        ASSERT_FALSE(holds_awake(state)) << "subject=" << static_cast<int>(subject);
        EXPECT_EQ(tier_for(holds_awake(state)), Tier::Quiet);
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 50);
    }
    {
        const AppState state = unavailable_device();
        ASSERT_FALSE(holds_awake(state));
        EXPECT_EQ(brightness_for(DisplayPower::On, tier_for(holds_awake(state)), levels), 50)
            << "an unreachable printer is not actionable; it acknowledges at Quiet and recedes";
    }
}

// ⛔ THE FIRING NEGATIVE CONTROL FOR THE TWO-LEVEL MODEL. Active and Quiet must be genuinely
// different outputs; if a refactor ever collapsed them, every test above would still pass while
// the distinction the owner selected disappeared.
TEST(DisplayPolicyTests, ActiveAndQuietAreDifferentOutputsAtTheSamePowerLevel) {
    const Levels levels = production_levels();

    EXPECT_NE(levels.active_percent, levels.quiet_percent);
    EXPECT_GT(levels.active_percent, levels.quiet_percent) << "Active is the brighter of the two";

    EXPECT_NE(brightness_for(DisplayPower::On, Tier::Active, levels),
              brightness_for(DisplayPower::On, Tier::Quiet, levels));

    EXPECT_NE(output_for(DisplayPower::On, Tier::Active, levels, kT),
              output_for(DisplayPower::On, Tier::Quiet, levels, kT))
        << "the same DisplayPower::On at two tiers is two different outputs; a cache that could "
           "not tell them apart would swallow every Printing->Completed recession";
}

TEST(DisplayPolicyTests, DimmedIsFiftyFivePercentOfQuietsLightWhichIsControlThirtyEight) {
    const Levels levels = production_levels();

    EXPECT_EQ(kB.dimmed_light_percent_of_quiet, 55);
    EXPECT_EQ(levels.dimmed_percent, 38)
        << "the output the owner observed at harness step D2 — duty 487 of 4096";

    EXPECT_EQ(brightness_for(DisplayPower::Dimmed, Tier::Quiet, levels), 38);
    EXPECT_EQ(brightness_for(DisplayPower::Dimmed, Tier::Active, levels), 38)
        << "Dimmed is one level: a held-awake state never recedes, so the tier cannot change it";
}

// ⛔ NEGATIVE CONTROL: the materially darker level the owner never evaluated. Deriving Dimmed as
// 20 % of Quiet's light — the fraction the harness printed for step D2, which it expressed against
// its own reference ceiling of 80 — would give control 24 and duty 177 instead of 487.
TEST(DisplayPolicyTests, DimmedIsNotTheUnevaluatedTwentyPercentOfQuiet) {
    const std::uint8_t unevaluated =
        prd::display::percent_for_light_fraction(kB.quiet_percent, prd::display::LightFraction{20});

    EXPECT_EQ(unevaluated, 24);
    EXPECT_NE(production_levels().dimmed_percent, unevaluated)
        << "control 24 is a level nobody looked at; the selection was the observed D2 output";
}

// ⛔ THE FIRING NEGATIVE CONTROL FOR THE CONFIRMED GAMMA DEFECT, now against Quiet.
TEST(DisplayPolicyTests, TheDimLevelIsNotAControlSpaceFractionOfQuiet) {
    EXPECT_EQ(control_space_scaling(kB.quiet_percent, kB.dimmed_light_percent_of_quiet), 27);

    EXPECT_NE(production_levels().dimmed_percent,
              control_space_scaling(kB.quiet_percent, kB.dimmed_light_percent_of_quiet))
        << "a fraction of the control value is not a fraction of the light; if these are equal the "
           "gamma-domain correction has been reverted";
}

TEST(DisplayPolicyTests, OffIsTrueOffAtEveryTierAndEveryQuietLevel) {
    for (std::uint16_t quiet = 1; quiet <= 100; ++quiet) {
        const Levels levels = levels_with_quiet(static_cast<std::uint8_t>(quiet));

        for (const auto tier : {Tier::Active, Tier::Quiet}) {
            EXPECT_EQ(brightness_for(DisplayPower::Off, tier, levels), 0) << "quiet=" << quiet;
            EXPECT_EQ(output_for(DisplayPower::Off, tier, levels, kT).target_percent, 0);
        }
    }
}

// ⛔ NEGATIVE CONTROL: a dim panel may never be brighter than the level it receded from, and may
// never collapse into Off. Recession runs from Quiet, so Quiet is what it is measured against.
TEST(DisplayPolicyTests, DimmedIsNeverBrighterThanQuietAndNeverCollapsesIntoOff) {
    for (std::uint16_t quiet = 1; quiet <= 100; ++quiet) {
        const Levels levels = levels_with_quiet(static_cast<std::uint8_t>(quiet));

        EXPECT_LE(levels.dimmed_percent, levels.quiet_percent) << "quiet=" << quiet;
        EXPECT_GT(levels.dimmed_percent, 0) << "quiet=" << quiet;
    }
}

// --- The effective output, and the cache key it is -------------------------

// ⛔ THE REGRESSION TEST FOR A CONFIRMED DEFECT, AND THE TWO-LEVEL MODEL MAKES IT LOAD-BEARING
// AGAIN. display_tick() first suppressed its post whenever DisplayPower was unchanged. Active and
// Quiet are both DisplayPower::On, so a cache keyed on the level alone would now swallow every
// Printing->Completed transition as well as every brightness change.
TEST(DisplayPolicyTests, ATierChangeIsDeliveredWhilePowerIsUnchanged) {
    const Levels levels = production_levels();

    const Output active = output_for(DisplayPower::On, Tier::Active, levels, kT);
    const Output quiet = output_for(DisplayPower::On, Tier::Quiet, levels, kT);

    EXPECT_NE(active, quiet);
    EXPECT_EQ(active.target_percent, 65);
    EXPECT_EQ(quiet.target_percent, 50);
    EXPECT_EQ(active.fade_ms, 0U) << "neither is a recession, so neither fades";
    EXPECT_EQ(quiet.fade_ms, 0U);
}

TEST(DisplayPolicyTests, ThePowerLevelAloneIsNotASufficientCacheKey) {
    const Levels lower = levels_with_quiet(40);
    const Levels higher = levels_with_quiet(100);

    EXPECT_NE(output_for(DisplayPower::On, Tier::Quiet, lower, kT),
              output_for(DisplayPower::On, Tier::Quiet, higher, kT));
    EXPECT_NE(output_for(DisplayPower::Dimmed, Tier::Quiet, lower, kT),
              output_for(DisplayPower::Dimmed, Tier::Quiet, higher, kT));

    // Off is the one level that genuinely collapses: it is zero everywhere, which is what "true
    // off" means.
    EXPECT_EQ(output_for(DisplayPower::Off, Tier::Quiet, lower, kT),
              output_for(DisplayPower::Off, Tier::Active, higher, kT));
}

// ⬅️ AND THE WHOLE OUTPUT IS THE KEY, WHICH IS WHY target_percent ALONE IS NOT ENOUGH TO COMPARE.
TEST(DisplayPolicyTests, TheFadeIsPartOfTheCacheKey) {
    const Output dimmed = output_for(DisplayPower::Dimmed, Tier::Quiet, production_levels(), kT);

    Output same_percent_different_fade = dimmed;
    same_percent_different_fade.fade_ms = 0;

    EXPECT_NE(dimmed, same_percent_different_fade);
}

TEST(DisplayPolicyTests, OnlyARecessionFades) {
    const Levels levels = production_levels();

    for (const auto tier : {Tier::Active, Tier::Quiet}) {
        EXPECT_EQ(output_for(DisplayPower::On, tier, levels, kT).fade_ms, 0U)
            << "a wake and a tier change must land at once — Production UI §10";
    }

    EXPECT_EQ(output_for(DisplayPower::Dimmed, Tier::Quiet, levels, kT).fade_ms, kT.fade_ms);
    EXPECT_EQ(output_for(DisplayPower::Off, Tier::Quiet, levels, kT).fade_ms, kT.fade_ms);
}

// --- Printing -> Completed --------------------------------------------------

// ⛔ THE TRANSITION THE BENCH ALREADY CLASSIFIED, NOW WITH THE LEVEL IT SHOULD CARRY. The capture
// of 2026-08-30 timed a real print finishing: the status payload dropped 306 -> 263 bytes and the
// activity clock reset 5 ms later, and Completed then held for exactly 20.000 s before dimming.
// The temporal behaviour was correct and is untouched here; what was wrong was the brightness. It
// Owner ruling 2026-09-08 supersedes the immediate drop: retain Active during the20s window.
TEST(DisplayPolicyTests, PrintingToCompletedKeepsActiveUntilTheAcknowledgementEnds) {
    const Levels levels = production_levels();

    AppState printing = ready_device();
    printing.subject = SubjectState::Printing;

    AppState completed = ready_device();
    completed.subject = SubjectState::Completed;

    // The tier changes, and it changes because holds_awake() does — not because anything switched
    // on a subject state.
    ASSERT_TRUE(holds_awake(printing));
    ASSERT_FALSE(holds_awake(completed));
    EXPECT_EQ(tier_for(holds_awake(printing)), Tier::Active);
    EXPECT_EQ(tier_for(holds_awake(completed)), Tier::Quiet);

    const Output before = output_for(DisplayPower::On, tier_for(holds_awake(printing)), levels, kT);
    const Output after = output_for(DisplayPower::On,
                                    output_tier_for(holds_awake(completed), completed.subject,
                                                    completed.confidence, DisplayPower::On),
                                    levels, kT);

    EXPECT_EQ(before.target_percent, 65);
    EXPECT_EQ(after.target_percent, 65);
    EXPECT_EQ(before, after) << "completion must not reduce brightness at entry";
    EXPECT_EQ(after.fade_ms, 0U) << "Production UI §10: the new state is represented immediately";

    // And it is a STRUCTURAL change, which is what resets the acknowledgement clock in app_core.
    EXPECT_NE(situation_of(printing), situation_of(completed));

    // The acknowledgement itself is unchanged: Completed holds for the full 20 s, then dims.
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt - 1, holds_awake(completed), kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt, holds_awake(completed), kT), Step::Dim);
    const auto dim_tier =
        output_tier_for(false, completed.subject, completed.confidence, DisplayPower::Dimmed);
    EXPECT_EQ(dim_tier, Tier::Quiet);
    EXPECT_EQ(output_for(DisplayPower::Dimmed, dim_tier, levels, kT).target_percent, 38);
    EXPECT_EQ(output_tier_for(false, completed.subject, Confidence::Unavailable, DisplayPower::On),
              Tier::Quiet);
    EXPECT_EQ(output_tier_for(false, completed.subject, completed.confidence, DisplayPower::Off),
              Tier::Quiet);

    // Printing, by contrast, never recedes however long it runs — 568 s of it was measured.
    EXPECT_EQ(next_step(DisplayPower::On, kSleepAt * 10, holds_awake(printing), kT), Step::Hold);
}

// ⛔ NEGATIVE CONTROL, AND THE ONE THAT KEEPS UIBD 001 §4 HONEST UNDER TWO LEVELS. Routine polling
// must create no activity AND must not move the brightness. Both follow from Situation carrying no
// value field, so a telemetry refresh cannot construct a difference in either answer.
TEST(DisplayPolicyTests, RoutineTelemetryCreatesNoActivityAndCannotMoveTheLevel) {
    const Levels levels = production_levels();

    AppState before = ready_device();
    before.subject = SubjectState::Printing;

    AppState after = before;
    after.temperatures.nozzle_current.known = true;
    after.temperatures.nozzle_current.celsius = 215;
    after.temperatures.bed_current.known = true;
    after.temperatures.bed_current.celsius = 60;

    EXPECT_EQ(situation_of(before), situation_of(after))
        << "a telemetry refresh is not a structural change, so it is not activity";

    EXPECT_EQ(holds_awake(before), holds_awake(after));
    EXPECT_EQ(tier_for(holds_awake(before)), tier_for(holds_awake(after)));

    EXPECT_EQ(output_for(DisplayPower::On, tier_for(holds_awake(before)), levels, kT),
              output_for(DisplayPower::On, tier_for(holds_awake(after)), levels, kT))
        << "and it therefore cannot change what the panel is lit at";
}

// ⛔ THE STARTUP CASE. The driver lights the panel at display::kDefaultBrightnessPercent (50) and
// app_core observes that into the model. The levels are ABSOLUTE product defaults and are NOT
// scaled from that observation — if they were, a driver bring-up constant nobody chose would become
// the user's configuration. The repair still holds by agreement: at boot nothing is held awake, so
// the tier is Quiet, and Quiet is 50 — the level already on the panel.
TEST(DisplayPolicyTests, TheFirstTickAsksForTheLevelTheDriverAlreadyApplied) {
    AppState state = ready_device();
    ASSERT_FALSE(holds_awake(state)) << "a booted, idle device is not held awake";

    const Output output =
        output_for(state.display.power, tier_for(holds_awake(state)), production_levels(), kT);

    EXPECT_EQ(output.target_percent, 50)
        << "the policy must never raise the panel off its validated startup level";
    EXPECT_EQ(output.fade_ms, 0U);

    // ⬅️ AND THE OBSERVATION IS NO LONGER AN INPUT. Whatever the model records the panel as being
    // at, the level the policy asks for is the product default for the tier.
    state = reduce(state, Event::display_brightness(at_ms(500), 17));
    ASSERT_EQ(state.display.brightness_percent, 17);

    EXPECT_EQ(output_for(state.display.power, tier_for(holds_awake(state)), production_levels(), kT)
                  .target_percent,
              50)
        << "an observed level is a record of the panel, not a setting for it";
}

// --- The manual toggle ------------------------------------------------------

TEST(DisplayPolicyTests, AShortPressWakesADarkPanelAndSleepsALitOne) {
    AppState state = ready_device();
    state.display.power = DisplayPower::Off;

    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::ShortPress));
    ASSERT_EQ(state.display.power, DisplayPower::On) << "Off -> On";

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::ShortPress));
    EXPECT_EQ(state.display.power, DisplayPower::Off) << "On -> manual Off";

    state = reduce(state, Event::button_gesture(at_ms(3000), ButtonGesture::ShortPress));
    EXPECT_EQ(state.display.power, DisplayPower::On) << "and back again";
}

TEST(DisplayPolicyTests, AShortPressSleepsFromDimmedToo) {
    AppState state = ready_device();
    state = reduce(state, Event::display_idle_timer_fired(at_ms(1000)));
    ASSERT_EQ(state.display.power, DisplayPower::Dimmed);

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::ShortPress));
    EXPECT_EQ(state.display.power, DisplayPower::Off);
}

// ⛔ NEGATIVE CONTROL, AND THE ONE THE OWNER'S RULING NAMES: a gesture another surface owns must
// not "accidentally become a sleep command".
TEST(DisplayPolicyTests, AShortPressNeverSleepsAPanelSomethingElseOwns) {
    const auto press_stays_lit = [](AppState state, const char* what) {
        ASSERT_EQ(state.display.power, DisplayPower::On);
        EXPECT_FALSE(short_press_may_sleep(state)) << what;

        state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::ShortPress));
        EXPECT_EQ(state.display.power, DisplayPower::On) << what;
    };

    {
        AppState state = ready_device();
        state.provisioning = ProvisioningPhase::SetupActive;
        press_stays_lit(state, "setup");
    }
    {
        AppState state = ready_device();
        state.subject_condition = SubjectCondition::Error;
        press_stays_lit(state, "error");
    }
    {
        AppState state = ready_device();
        state.device_fault = DeviceFaultReason::WifiUnavailable;
        press_stays_lit(state, "device fault");
    }
    {
        AppState state = ready_device();
        state.subject = SubjectState::Printing;
        press_stays_lit(state, "a live print");
    }
}

// The confirmation keeps its existing meaning — owner ruling A of Milestone 3.10 — and cancelling
// a destructive prompt must never be read as a request to sleep.
TEST(DisplayPolicyTests, AShortPressStillCancelsAnArmedConfirmationAndDoesNotSleep) {
    AppState state = prd::test::printing_device(at_ms(0));
    state = reduce(state, Event::button_gesture(at_ms(1000), ButtonGesture::VeryLongPress));
    ASSERT_EQ(state.modal_surface, ModalSurface::FactoryResetConfirmation);

    state = reduce(state, Event::button_gesture(at_ms(2000), ButtonGesture::ShortPress));

    EXPECT_EQ(state.modal_surface, ModalSurface::None) << "the cancel still happens";
    EXPECT_EQ(state.display.power, DisplayPower::On) << "and it is not also a sleep command";
}

// --- The wake event ---------------------------------------------------------

TEST(DisplayPolicyTests, AWakeLightsThePanelWithoutClaimingAUserTouchedIt) {
    AppState state = ready_device();
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(1000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    const Timestamp interaction_before = state.display.last_interaction_at;

    state = reduce(state, Event::display_wake(at_ms(2000)));

    EXPECT_EQ(state.display.power, DisplayPower::On);
    EXPECT_FALSE(state.display.idle_mode);
    EXPECT_EQ(state.display.last_interaction_at, interaction_before)
        << "§7.11's lastInteractionAt means a person touched the device; a printer did not";
}

// The wake must reach observers, and must be counted as applied like every sibling in that switch.
//
// ⬅️ THESE ARE TWO DIFFERENT MECHANISMS, AND CONFUSING THEM COSTS A REAL ASSERTION. §7.1's revision
// is bumped by reduce() from content_equals() — a STATE COMPARISON — so any case that genuinely
// changes the state publishes a new revision and the store notifies, whatever the case returned.
// The bool the switch returns feeds diagnostics.events_applied and nothing else.
//
// The wake case was first written with `break` where every sibling uses `return true`. The panel
// would still have woken and the renderer would still have been told; what was actually lost was
// the event count. Asserting the revision alone would have passed either way, which is why the
// counter is asserted too.
TEST(DisplayPolicyTests, AWakeReachesObserversAndIsCountedAsApplied) {
    AppState state = ready_device();
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(1000)));
    ASSERT_EQ(state.display.power, DisplayPower::Off);

    const auto revision_before = state.revision;
    const auto applied_before = state.diagnostics.events_applied;

    state = reduce(state, Event::display_wake(at_ms(2000)));

    EXPECT_GT(state.revision, revision_before)
        << "the state changed, so §7.1 requires a new revision — that is what notifies observers";
    EXPECT_GT(state.diagnostics.events_applied, applied_before)
        << "and the event must be counted as applied, which is what the switch's bool feeds";
}

// And the recession's own two events must report their mutations for the same reason.
TEST(DisplayPolicyTests, DimAndSleepAreReportedAsMutations) {
    AppState state = ready_device();

    const auto before_dim = state.revision;
    state = reduce(state, Event::display_idle_timer_fired(at_ms(1000)));
    EXPECT_GT(state.revision, before_dim);

    const auto before_sleep = state.revision;
    state = reduce(state, Event::display_sleep_timer_fired(at_ms(2000)));
    EXPECT_GT(state.revision, before_sleep);
}

// --- The accepted lost-observation contract --------------------------------
//
// UI Behaviour Decision 001 §4/§5.1/§10.2, Production UI §9, UIAD 001 §11.1 and Architecture Spec
// §17.8, all accepted 2026-08-31. Every test below is a load-bearing transition of that contract,
// and each names the negative control it would fail under.

// ⛔ NEGATIVE CONTROL for the old three-valued Situation. Before 2026-08-31 Situation carried
// Confidence, so this crossing moved a field, counted as activity, restarted the recession and woke
// a dark panel. §4: deterioration into Degraded "is not itself a meaningful change".
TEST(LostObservationTests, DegradedIsStructurallyInvisibleToTheRecession) {
    AppState current = ready_device();
    AppState degraded = current;
    degraded.confidence = Confidence::Degraded;

    EXPECT_EQ(situation_of(current), situation_of(degraded));
    EXPECT_EQ(observation_of(Confidence::Current), Observation::Present);
    EXPECT_EQ(observation_of(Confidence::Degraded), Observation::Present);
}

// The other half: expiry INTO Unavailable is the one crossing that IS meaningful.
TEST(LostObservationTests, OnlyExpiryIntoUnavailableMovesTheSituation) {
    AppState degraded = ready_device();
    degraded.confidence = Confidence::Degraded;

    AppState lost = degraded;
    lost.confidence = Confidence::Unavailable;

    EXPECT_NE(situation_of(degraded), situation_of(lost));
    EXPECT_EQ(observation_of(Confidence::Unavailable), Observation::Lost);
    EXPECT_EQ(situation_of(lost).observation, Observation::Lost);
}

// ⛔ THE SEMANTIC STATES ARE NOT COLLAPSED. Two-valued is the RECESSION's question only; AppState
// keeps all three values and the projection still tells Degraded from Unavailable.
TEST(LostObservationTests, DegradedAndUnavailableRemainDistinctSemanticStates) {
    EXPECT_NE(Confidence::Degraded, Confidence::Unavailable);
    EXPECT_NE(observation_of(Confidence::Degraded), observation_of(Confidence::Unavailable));
}

// Repeated failed polls while already Unavailable change nothing structural, so they cannot
// acknowledge again — §5.1's "exactly one acknowledgement for that loss episode".
TEST(LostObservationTests, RepeatedFailuresWhileLostDoNotMoveTheSituationAgain) {
    AppState lost = unavailable_device();
    const Situation first = situation_of(lost);

    for (int poll = 0; poll < 20; ++poll) {
        AppState again = lost;
        again.confidence = Confidence::Unavailable; // every subsequent expired poll
        EXPECT_EQ(first, situation_of(again));
    }
}

// Recovery is the reverse crossing, and it is activity.
TEST(LostObservationTests, RecoveryMovesTheSituationBackAndIsThereforeActivity) {
    AppState lost = unavailable_device();
    AppState recovered = lost;
    recovered.confidence = Confidence::Current;

    EXPECT_NE(situation_of(lost), situation_of(recovered));
    EXPECT_EQ(situation_of(recovered).observation, Observation::Present);
}

// ⛔ UNAVAILABLE STILL DOES NOT HOLD THE PANEL AWAKE, so it recedes like Ready does and reaches a
// dark panel. This is what makes the acknowledgement transient rather than a lit state.
TEST(LostObservationTests, LostObservationDoesNotHoldThePanelAwake) {
    const AppState lost = unavailable_device();

    EXPECT_FALSE(holds_awake(lost));
    EXPECT_EQ(tier_for(holds_awake(lost)), Tier::Quiet);
    EXPECT_TRUE(short_press_may_sleep(lost));
}

// --- The exact accepted timeline -------------------------------------------
//
// §10.2 and §17.8.3: t = 0…20 Quiet, 20…22 fade, 22…322 STABLE Dimmed, 322…324 fade, Off at 324.
//
// ⛔ NEGATIVE CONTROL for the 120-second hold. Every assertion below fails if dim_hold_ms returns
// to 120 000: kSleepAt becomes 140 000, and the 322 s and 324 s boundaries move.
TEST(LostObservationTests, TheUniversalTimelineIsTheAcceptedThreeHundredAndTwentyFourSeconds) {
    EXPECT_EQ(kT.acknowledge_ms, 20U * 1000U);
    EXPECT_EQ(kT.fade_ms, 2U * 1000U);
    EXPECT_EQ(kT.dim_hold_ms, 300U * 1000U) << "the accepted hold is 300 s of STABLE Dimmed";

    // The two step boundaries the policy actually decides.
    EXPECT_EQ(kDimAt, 20U * 1000U);
    EXPECT_EQ(kSleepAt, 322U * 1000U) << "Dimmed ends at t = 322 s, not t = 140 s";

    // The accepted total, fades included rather than added.
    EXPECT_EQ(kOffAt, 324U * 1000U) << "true Off at t = 324 s";
}

TEST(LostObservationTests, EachPhaseBoundaryHoldsToTheMillisecond) {
    const AppState lost = unavailable_device();
    const bool held = holds_awake(lost);

    // t = 0 … just before 20 s: Quiet, nothing moves.
    EXPECT_EQ(next_step(DisplayPower::On, 0U, held, kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt - 1U, held, kT), Step::Hold);

    // t = 20 s: the fade to Dimmed begins.
    EXPECT_EQ(next_step(DisplayPower::On, kDimAt, held, kT), Step::Dim);

    // t = 22 … just before 322 s: stable Dimmed, a full 300 s of it.
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kDimAt + kT.fade_ms, held, kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::Dimmed, 140U * 1000U, held, kT), Step::Hold)
        << "t = 140 s is mid-Dimmed under the accepted timeline; the old 120 s hold slept here";
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt - 1U, held, kT), Step::Hold);

    // t = 322 s: the fade to true Off begins.
    EXPECT_EQ(next_step(DisplayPower::Dimmed, kSleepAt, held, kT), Step::Sleep);

    // Off is terminal; nothing here climbs back.
    EXPECT_EQ(next_step(DisplayPower::Off, kSleepAt + kT.fade_ms, held, kT), Step::Hold);
    EXPECT_EQ(next_step(DisplayPower::Off, 10U * 60U * 1000U, held, kT), Step::Hold);
}

// ⛔ NEGATIVE CONTROL for a direct state/confidence→PWM shortcut. The levels are selected from the
// power and the tier ONLY; no confidence value and no scene reaches brightness_for(). Architecture
// Spec §17.8.1 and UIBD 001 §10.3.
TEST(LostObservationTests, LostObservationSelectsNoBrightnessOfItsOwn) {
    const Levels levels = production_levels();

    // Lost observation is not held awake, so it takes exactly the Quiet ladder — the same values a
    // Ready printer takes. There is no Unavailable level anywhere.
    EXPECT_EQ(brightness_for(DisplayPower::On, Tier::Quiet, levels), kB.quiet_percent);
    EXPECT_EQ(brightness_for(DisplayPower::Dimmed, Tier::Quiet, levels), levels.dimmed_percent);
    EXPECT_EQ(brightness_for(DisplayPower::Off, Tier::Quiet, levels), 0U);

    // And the ratified control values are unchanged by this checkpoint.
    EXPECT_EQ(kB.active_percent, 65U);
    EXPECT_EQ(kB.quiet_percent, 50U);
    EXPECT_EQ(levels.dimmed_percent, 38U);

    // The same three outputs a Ready printer produces, proving there is one ladder and not two.
    const AppState ready = ready_device();
    const AppState lost = unavailable_device();
    EXPECT_EQ(tier_for(holds_awake(ready)), tier_for(holds_awake(lost)));
    EXPECT_EQ(output_for(DisplayPower::On, tier_for(holds_awake(lost)), levels, kT),
              output_for(DisplayPower::On, tier_for(holds_awake(ready)), levels, kT));
}

// Unavailable and Ready retain the ordinary schedule. The owner's Completed-only
// amendment must not change either of them.
TEST(LostObservationTests, LostAndReadyRetainTheSameOrdinaryRecession) {
    const AppState lost = unavailable_device();
    const AppState ready = ready_device();

    for (const std::uint32_t elapsed : {0U, 19999U, 20000U, 140000U, 321999U, 322000U}) {
        EXPECT_EQ(next_step(DisplayPower::On, elapsed, holds_awake(lost), kT),
                  next_step(DisplayPower::On, elapsed, holds_awake(ready), kT));
        EXPECT_EQ(next_step(DisplayPower::Dimmed, elapsed, holds_awake(lost), kT),
                  next_step(DisplayPower::Dimmed, elapsed, holds_awake(ready), kT));
    }
}

TEST(DisplayPolicyTests, CompletedTelemetryAndDegradationDoNotRearmButLiveReadyDoes) {
    AppState completed = ready_device();
    completed.subject = SubjectState::Completed;
    AppState polled = completed;
    polled.confidence = Confidence::Degraded;
    EXPECT_EQ(situation_of(completed), situation_of(polled));
    EXPECT_EQ(recession_timings_for(completed.subject, completed.confidence, kT).dim_hold_ms,
              recession_timings_for(polled.subject, polled.confidence, kT).dim_hold_ms);
    polled.subject = SubjectState::Ready;
    EXPECT_NE(situation_of(completed), situation_of(polled));
    polled.subject = SubjectState::Printing;
    EXPECT_NE(situation_of(completed), situation_of(polled));
    EXPECT_TRUE(holds_awake(polled));
}
