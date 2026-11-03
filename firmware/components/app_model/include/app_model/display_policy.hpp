// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/app_state.hpp"
#include "app_model/device_data.hpp"

// The display-power policy — Architecture Spec §7.11 and §17.8, Product Brief v0.2 "Display
// behaviour", UI Behaviour Decision 001 §3 and §5.
//
// ⛔ WHY THIS EXISTS, AND WHAT IT REPLACES. §7.11's DisplayState and its On/Dimmed/Off reducer
// paths have existed since Milestone 1, and so has the perceptual backlight ramp in
// display::fade_brightness_to(). Nothing joined them: no production code ever dispatched
// DisplayIdleTimerFired or DisplaySleepTimerFired — only host tests did — and NOTHING anywhere
// read state.display.power. The backlight was switched on once at boot and stayed on for ever.
// That gap is why the first Printer Unavailable candidate reached the owner as a permanently lit
// sentence, and it was rejected on exactly that ground.
//
// This file is the missing seam, and it is deliberately the WHOLE of the decision: app_core reads
// the answers and acts, the reducer applies them to DisplayState, and neither re-decides anything.
//
// ---------------------------------------------------------------------------
// THE FOUR RULES THIS FILE EXISTS TO KEEP
// ---------------------------------------------------------------------------
//
// 1. ⛔ NO PRINTER-STATE→PWM SHORTCUT, EVEN NOW THAT THERE ARE TWO ON LEVELS. Nothing here maps a
//    subject state, a confidence or a scene to a brightness: Active versus Quiet is decided by
//    tier_for(holds_awake(state)), the SAME predicate the recession and the button already share,
//    so there is exactly one structural list and nothing to drift against. The path is: a STRUCTURAL change is activity → activity feeds the recession
//    → the recession moves DisplayState.power → power selects a brightness. A printer that goes
//    unreachable dims for the same reason a Ready printer does: nothing happened for a while.
//
// 2. ⛔ ROUTINE TRAFFIC IS NOT ACTIVITY. UI Behaviour Decision 001 §4: "Routine polling, retries
//    and small telemetry changes shall not become visible." Situation below is the enforcement:
//    it has no field for a temperature, a progress value, a job name or a poll outcome, so a
//    telemetry refresh CANNOT be seen as activity by construction, not merely by intent.
//
// 3. ⛔ THE PANEL IS NEVER THE ONLY COPY OF SOMETHING ACTIONABLE. holds_awake() names every
//    condition that must stay lit until a person resolves it.
//
// 4. ⛔ DISPLAY POWER IS NOT DEVICE POWER. §7.11's closing rule and §17.8's first line: monitoring
//    continues while the panel is dimmed or off. Nothing here sleeps the MCU, stops the radio,
//    stops polling or pauses the application task.
//
// 5. ⛔ NO PANEL CONSTANT LIVES HERE, AND NO BRIGHTNESS ARITHMETIC EITHER. This unit selects among
//    control levels that the display component computed; it does not scale them. The rule exists
//    because breaking it produced a confirmed physical defect: Dimmed was written as a fraction of
//    the CONTROL value, the driver then raised that value to gamma 2.2, and the intended 15 % of
//    light reached the panel as 1.5 %. A percentage is meaningless without its domain, and the
//    domain of the backlight belongs to the backlight.

namespace prd::app_model::display_policy {

// The working baselines, fixed by the owner on 2026-08-30 for this checkpoint.
//
// ⚠️ WORKING BASELINES, NOT PROJECT SOURCE LAW. No accepted source prescribes any of these
// numbers; the owner's ruling calls them "physically tunable working baselines". They are a
// struct rather than constants precisely so a bench pass can vary them without touching the
// decision, and so every test states the timing it is asserting against.
struct Timings {
    // How long the panel stays lit after a meaningful change, so the change can be read. UIBD 001
    // §5: "change occurs; new state becomes understandable; the interface returns to stillness."
    std::uint32_t acknowledge_ms{20U * 1000U};

    // How long Dimmed lasts, as STABLE time at Dimmed, before the backlight goes off. Dimmed is a
    // TRANSITION, not the resting state — the resting state is dark.
    //
    // ⬅️ THE ACCEPTED TIMELINE, and it is measured in stable time rather than wall time. UI
    // Behaviour Decision 001 §10.2 and Architecture Spec §17.8.3, with t = 0 at the last
    // acknowledged activity:
    //
    //     t =   0 .. 20 s   Quiet
    //     t =  20 .. 22 s   fade to Dimmed        (fade_ms)
    //     t =  22 .. 322 s  STABLE Dimmed         (this value — a full 300 s)
    //     t = 322 .. 324 s  fade to true Off      (fade_ms)
    //     t = 324 s         true Off
    //
    // ⛔ THE FADES ARE INSIDE THE TOTAL, NOT ADDED TO IT, and the 300 s begins only once the first
    // fade has completed. The earlier value was 120 000, and it produced a two-minute hold that
    // §10.2 now supersedes; a test pins the whole timeline so the number cannot drift back.
    std::uint32_t dim_hold_ms{300U * 1000U};

    // The perceptual ramp between levels — display::fade_brightness_to()'s duration.
    std::uint32_t fade_ms{2000U};
};

// Owner amendment 2026-10-04: only presentable Completed gets twenty minutes of
// stable Dimmed. This selects a duration, not a hold/wake or a historical scene.
// See docs/decisions/0011-completed-recession.md for the narrow Source exception.
[[nodiscard]] constexpr Timings recession_timings_for(const SubjectState subject,
                                                      const Confidence confidence,
                                                      const Timings& ordinary) {
    Timings selected = ordinary;
    if (subject == SubjectState::Completed && confidence != Confidence::Unavailable) {
        selected.dim_hold_ms = 20U * 60U * 1000U;
    }
    return selected;
}

// The brightness working baselines, SELECTED BY THE OWNER on the physical panel, 2026-08-30.
//
// ⚠️ WORKING BASELINES, NOT PROJECT SOURCE LAW, and not owner configuration either. No accepted
// source prescribes any of these numbers; the persisted config::Config::brightness field remains
// deliberately unconsumed and the configuration/migration decision is a separate checkpoint.
//
// HOW THEY WERE OBTAINED, because it is what makes them evidence rather than taste. An automatic
// ladder was tried first, announcing each 8-15 second step in chat; the owner could not tell which
// level was on screen and that run was ruled INVALID for selection. It was replaced by an
// owner-paced harness that holds every step indefinitely, advances only on a short BOOT press, and
// prints the step label ON THE PANEL. Walking it, the owner selected:
//
//   Active  = 65 % control, duty 1588/4096   (harness step C2)
//   Quiet   = 50 % control, duty  891/4096   (harness step C1)
//   Dimmed  = duty 487/4096                  (harness step D2, as observed)
//
// ⛔ NO SERIAL RECORD EXISTS FOR THAT SESSION and none is claimed. The capture was never attached,
// so no machine observation of Active, Quiet or Dimmed is asserted anywhere; the numeric mapping
// below comes from the host-tested step table and the compiled curve, and the APPEARANCE was
// judged by the owner directly, which is the required evidence for a perceptual property.
//
// ⬅️ ACTIVE IS 1.78x QUIET IN EMITTED LIGHT (1588 / 891), which is why one On level was wrong.
struct Brightness {
    // Held-awake states: a live job, setup, an actionable condition, a modal surface, a device
    // fault. Something a person is expected to read.
    std::uint8_t active_percent{65};

    // Ordinary On: Ready, a Completed or Unavailable acknowledgement, anything not held awake.
    // Present, unobtrusive, and about to recede.
    std::uint8_t quiet_percent{50};

    // Dimmed as a fraction of the LIGHT emitted at QUIET — never at Active, and never a fraction
    // of a control value.
    //
    // ⛔ RELATIVE TO QUIET BECAUSE RECESSION ONLY EVER HAPPENS FROM QUIET. A held-awake state does
    // not recede at all, so an Active-relative dim level would describe a transition that cannot
    // occur, and would drift the moment Active moved.
    //
    // ⬅️ 55 % REPRODUCES THE OUTPUT THE OWNER ACTUALLY SAW. The harness expressed its dim steps
    // against a reference ceiling of 80, so the selected D2 was duty 487; 55 % of Quiet's light is
    // control 38, which is duty 487 on this curve. Deriving 20 % of Quiet instead would give
    // control 24 / duty 177 — a materially darker output nobody evaluated.
    std::uint8_t dimmed_light_percent_of_quiet{55U};
};

// Which of the two On levels a situation asks for.
enum class Tier : std::uint8_t {
    // Something a person is expected to read: a live job, setup, an actionable condition, a modal
    // surface, a device fault.
    Active,

    // Ordinary On. Present and unobtrusive, and on its way to Dimmed.
    Quiet,
};

[[nodiscard]] std::string_view log_label(Tier tier);

// ⛔ THE ONE MAPPING FROM SITUATION TO ON LEVEL, AND IT IS holds_awake() — NOT A SECOND LIST.
//
// This is the whole of rule 1 under the new two-level model. There is no switch on SubjectState
// anywhere, no branch on Confidence, no scene consulted: the question "must this stay lit until a
// person deals with it?" is exactly the question "is this a level someone is reading?", and both
// are answered by the predicate the recession and the button already share. A separate Active list
// would drift from holds_awake() silently, and the drift would be a state the timer respected but
// the brightness did not.
//
// Base tier for an indefinite hold. The output selector below adds the owner-approved
// bounded Completed acknowledgement without making Completed an indefinite awake hold.
[[nodiscard]] constexpr Tier tier_for(const bool held_awake) {
    return held_awake ? Tier::Active : Tier::Quiet;
}

// Owner ruling 2026-09-08: Completed uses Active during its ordinary On acknowledgement.
// The existing activity clock ends On after20s; no second clock or extra wake is introduced.
// Unavailable, manual Off and ordinary Dimmed remain governed by their existing policies.
[[nodiscard]] constexpr Tier output_tier_for(const bool held_awake, const SubjectState subject,
                                             const Confidence confidence,
                                             const DisplayPower power) {
    return held_awake || (subject == SubjectState::Completed &&
                          confidence != Confidence::Unavailable && power == DisplayPower::On)
               ? Tier::Active
               : Tier::Quiet;
}

// The three lit control levels the panel is driven at, in the driver's own percentage units.
//
// ⛔ THE POLICY SELECTS AMONG THESE AND NEVER COMPUTES THEM, WHICH IS THE WHOLE POINT. The gamma
// defect was an arithmetic operation performed in the wrong domain; the repair is that this unit
// performs no brightness arithmetic at all. There is no multiplication here to get wrong — the
// same discipline as Situation below, where the defect is prevented by the shape of the type
// rather than by a rule someone has to remember.
//
// Produced by app_core from Brightness above, through display::percent_for_light_fraction(), which
// owns the panel's gamma. Every field is a CONTROL percentage, so `dimmed_percent` is not the light
// fraction and must not be read as one.
struct Levels {
    std::uint8_t active_percent{100};
    std::uint8_t quiet_percent{100};

    // The control percentage that emits Brightness::dimmed_light_percent_of_quiet % of QUIET's
    // light. Guaranteed by its producer to be <= quiet_percent, and >= 1 when quiet_percent is
    // non-zero — a dim panel is never brighter than the level it receded from, and never collapses
    // into Off.
    std::uint8_t dimmed_percent{100};
};

// The structural identity of what the device is showing.
//
// ⛔ EVERY FIELD IS A STATE, AND NOT ONE IS A VALUE. This is rule 2 above made structural. The
// fields are the same facts UI Behaviour Decision 001 §4 calls meaningful — "print started;
// print paused; print completed; print stopped; attention required; printer unavailable;
// communication restored" — and the fields it calls routine are absent, so no amount of polling
// can construct a difference here.
//
// Taken from AppState rather than from a projected Presentation, deliberately: the projection
// runs inside a store observer that must not dispatch, so the tick would have to project a second
// time to consult it. These are the same facts the projection resolves its layer from.
// Whether the printer can be observed at all.
//
// ⛔ TWO VALUES, NOT THREE, AND THAT IS THE WHOLE POINT OF THIS TYPE. Situation carried the full
// three-valued Confidence until 2026-08-31, which made the Current → Degraded crossing a structural
// change — so a printer that had merely missed a poll reset the recession clock and woke a dark
// panel. UI Behaviour Decision 001 §4, as amended: deterioration into Degraded "is not itself a
// meaningful change", "shall not wake the panel and shall not restart display-power recession", and
// only expiry INTO Unavailable is meaningful.
//
// Collapsing Current and Degraded here makes that structural rather than conditional: there is no
// field a Degraded crossing can move, so it cannot reach the recession by any route.
//
// ⬅️ AND IT DOES NOT COLLAPSE THE SEMANTIC STATES. AppState::confidence keeps all three values, the
// presentation still distinguishes Degraded's stale treatment from Unavailable's absence, and
// UIAD 001 §7's three confidence values are untouched. What is two-valued is only the question the
// RECESSION asks, which was never the same question.
enum class Observation : std::uint8_t {
    // Current or Degraded: a subject state may be presented, with or without its stale indicator.
    Present,

    // Unavailable: no previous Subject State may be presented as current (UIAD 001 §7).
    Lost,
};

// The one place confidence becomes the recession's two-valued question.
[[nodiscard]] constexpr Observation observation_of(const Confidence confidence) {
    return confidence == Confidence::Unavailable ? Observation::Lost : Observation::Present;
}

struct Situation {
    DeviceOperation operation{DeviceOperation::None};
    DeviceFaultReason fault{DeviceFaultReason::None};
    BootPhase boot{BootPhase::Starting};
    ProvisioningPhase provisioning{ProvisioningPhase::Inactive};
    AccessCondition access{AccessCondition::None};
    ModalSurface modal{ModalSurface::None};
    SubjectState subject{SubjectState::AcquiringState};
    SubjectCondition condition{SubjectCondition::None};

    // ⬅️ NOT Confidence. See Observation above: a Degraded crossing must leave this struct
    // identical, and only entry into or exit from Unavailable may move it. Entering Unavailable is
    // therefore exactly one activity — the single transient acknowledgement of UIBD 001 §5.1 — and
    // every failed poll after it leaves this field where it already was.
    Observation observation{Observation::Lost};
};

[[nodiscard]] bool operator==(const Situation& lhs, const Situation& rhs);
[[nodiscard]] bool operator!=(const Situation& lhs, const Situation& rhs);

[[nodiscard]] Situation situation_of(const AppState& state);

// Whether the panel must stay lit until a person or the machine resolves what is on it.
//
// Owner ruling, 2026-08-30: "Attention, Error, Device Fault, setup, confirmation, and other
// actionable/user-owned states wake and remain On until resolved", and "Preparing, Printing, and
// Paused remain On by default".
//
// ⬅️ AND UNAVAILABLE IS NOT AMONG THEM, WHICH IS THE POINT OF THE WHOLE CHECKPOINT. An
// unreachable printer is not actionable — on this desk it is usually a printer someone switched
// off deliberately — so it follows the ordinary recession like Ready does. Same ruling: it
// "follows the same universal idle policy rather than owning a special backlight rule".
[[nodiscard]] bool holds_awake(const AppState& state);

// What the recession wants next.
enum class Step : std::uint8_t {
    // Stay where you are. Also the answer whenever something holds the panel awake.
    Hold,

    // On → Dimmed. DisplayIdleTimerFired.
    Dim,

    // Dimmed → Off. DisplaySleepTimerFired.
    Sleep,
};

[[nodiscard]] std::string_view log_label(Step step);

// The recession decision.
//
// `since_activity_ms` is the caller's ONE monotonic observation minus the recorded activity
// moment, computed wrap-safely by elapsed_ms() below. Never reads a clock itself: a unit that
// took its own reading could disagree with the reading the rest of the tick was decided from,
// which is the defect app_core's monotonic_ms() note records at length.
//
// ⛔ IT ONLY EVER DESCENDS. There is no path from here back to On — waking is an event (a button,
// a meaningful change, an actionable condition), never the absence of one. A timer that could
// raise brightness would be an ambient loop, which UI Behaviour Decision 001 §9 prohibits.
[[nodiscard]] Step next_step(DisplayPower power, std::uint32_t since_activity_ms, bool held_awake,
                             const Timings& timings);

// Wrap-safe elapsed milliseconds.
//
// ⛔ UNSIGNED SUBTRACTION FIRST, SIGNED SANITY SECOND — the idiom reentry.cpp already carries, for
// the reason recorded there. The 32-bit millisecond counter wraps after 49.7 days, and a device
// that had been quiet across that boundary would otherwise be handed an interval of nearly the
// whole range: the panel would sleep the instant it woke. A reading that PRECEDES the recorded
// moment is a different fault — it is not an interval at all — and unsigned arithmetic reports it
// as almost 2^32 too. Read signed, the genuine wrap stays small and positive while the backwards
// reading is negative, so the two are separable.
[[nodiscard]] std::uint32_t elapsed_ms(std::uint32_t now_ms, std::uint32_t since_ms);

// The brightness a power level asks for, as a percentage of the panel's full scale.
//
// ⛔ TAKES A POWER LEVEL, NEVER A SCENE OR A SUBJECT STATE. Levels carries the ceiling the user
// chose and the dim level derived from it, so lowering the ceiling lowers everything with it and
// a dim panel can never be brighter than an awake one.
//
// ⬅️ AND IT IS A SELECTION, NOT A CALCULATION. Three inputs, three outputs, no arithmetic — see
// the note on Levels for why that is the shape the gamma defect argued for.
[[nodiscard]] std::uint8_t brightness_for(DisplayPower power, Tier tier, const Levels& levels);

// The COMPLETE effective output the render task needs — not the power level it was derived from.
//
// ⛔ THIS EXISTS BECAUSE CACHING THE POWER ENUM ALONE IS A DEFECT. Two different outputs share one
// power level: On at a 50 % ceiling and On at 80 % are both DisplayPower::On, and a caller that
// suppressed its post whenever the level was unchanged would swallow every brightness change the
// user ever made. The bug is latent only while nothing dispatches DisplayBrightnessObserved —
// and the startup-brightness repair makes it the FIRST thing that happens on every boot.
//
// So the cache key is the whole output. A struct rather than two returns, so a caller cannot
// compare one field and believe it compared the answer.
struct Output {
    std::uint8_t target_percent{100};

    // ⬅️ A WAKE IS IMMEDIATE; ONLY A RECESSION FADES. Production UI §10: "the interface shall
    // immediately represent newly received printer state." A fault arriving on a dark panel must
    // be readable at once, and a user moving the brightness ceiling wants the panel to answer,
    // not to drift.
    std::uint32_t fade_ms{0};
};

[[nodiscard]] bool operator==(const Output& lhs, const Output& rhs);
[[nodiscard]] bool operator!=(const Output& lhs, const Output& rhs);

[[nodiscard]] Output output_for(DisplayPower power, Tier tier, const Levels& levels,
                                const Timings& timings);

// Whether a short press may put the panel to sleep, rather than being owned by something else.
//
// Owner ruling: "In normal non-modal operation, short press is the manual display-power toggle:
// Off → On; On/Dimmed → manual Off. Setup, recovery, factory-reset confirmation, and other
// modal/gesture ownership retain priority and must not accidentally become sleep commands."
//
// Waking is unconditional and is not asked about here — a press on a dark panel always lights it,
// including during an exclusive operation, because seeing what the device is doing is not a
// mutation.
[[nodiscard]] bool short_press_may_sleep(const AppState& state);

} // namespace prd::app_model::display_policy
