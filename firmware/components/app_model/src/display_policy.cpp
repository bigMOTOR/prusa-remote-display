// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_model/display_policy.hpp"

namespace prd::app_model::display_policy {
namespace {

// The same question presentation.cpp's own resolve_layer() asks first: a boot that has not
// finished is a device-owned state, and the panel belongs to it until it has.
[[nodiscard]] bool boot_is_unresolved(const BootPhase phase) {
    return phase != BootPhase::Ready && phase != BootPhase::FatalError;
}

// The three subject states that carry a live job. Owner ruling: they "remain On by default".
//
// ⛔ GATED ON PRESENTABLE CONFIDENCE. At Unavailable there is no Printing to hold the panel for —
// UIAD 001 §7 says no previous subject state may be presented as current — so a printer that was
// printing an hour ago and has since gone unreachable recedes like anything else. Without this
// gate a single lost printer would light the desk indefinitely on the strength of a job the
// product can no longer see.
[[nodiscard]] bool carries_live_job(const SubjectState subject, const Confidence confidence) {
    if (confidence == Confidence::Unavailable) {
        return false;
    }

    return subject == SubjectState::Preparing || subject == SubjectState::Printing ||
           subject == SubjectState::Paused;
}

} // namespace

bool operator==(const Situation& lhs, const Situation& rhs) {
    return lhs.operation == rhs.operation && lhs.fault == rhs.fault && lhs.boot == rhs.boot &&
           lhs.provisioning == rhs.provisioning && lhs.access == rhs.access &&
           lhs.modal == rhs.modal && lhs.subject == rhs.subject && lhs.condition == rhs.condition &&
           lhs.observation == rhs.observation;
}

bool operator!=(const Situation& lhs, const Situation& rhs) {
    return !(lhs == rhs);
}

Situation situation_of(const AppState& state) {
    Situation situation;

    situation.operation = state.device_operation;
    situation.fault = state.device_fault;
    situation.boot = state.boot.phase;
    situation.provisioning = state.provisioning;
    situation.access = state.access_condition;
    situation.modal = state.modal_surface;
    situation.subject = state.subject;
    situation.condition = state.subject_condition;
    situation.observation = observation_of(state.confidence);

    return situation;
}

bool holds_awake(const AppState& state) {
    // §12.1 and §12.2: a fault and an exclusive operation own the panel outright.
    if (state.device_operation != DeviceOperation::None) {
        return true;
    }

    // §12.1's fact, separately from the operation derived from it — the reducer keeps the two
    // apart deliberately, and a fault reason that has not yet resolved into an operation is still
    // something a person has to see.
    if (state.device_fault != DeviceFaultReason::None) {
        return true;
    }

    if (boot_is_unresolved(state.boot.phase)) {
        return true;
    }

    // §12.3: setup and recovery are user-owned. A setup access point whose panel went dark would
    // hide the network name and password the user is trying to type.
    if (state.provisioning != ProvisioningPhase::Inactive) {
        return true;
    }

    // §12.4: authentication required and unsupported interface are both actionable.
    if (state.access_condition != AccessCondition::None) {
        return true;
    }

    // §12.5: a confirmation is a question, and a question with an unlit panel is a trap. Owner
    // ruling A of Milestone 3.10 already requires the factory-reset confirmation to be ON SCREEN.
    if (state.modal_surface != ModalSurface::None) {
        return true;
    }

    // §7.10's condition layer: Attention and Error attach to the subject and stay until resolved.
    if (state.subject_condition != SubjectCondition::None) {
        return true;
    }

    return carries_live_job(state.subject, state.confidence);
}

std::string_view log_label(const Step step) {
    switch (step) {
    case Step::Hold:
        return "hold";

    case Step::Dim:
        return "dim";

    case Step::Sleep:
        return "sleep";
    }

    return "unknown";
}

std::uint32_t elapsed_ms(const std::uint32_t now_ms, const std::uint32_t since_ms) {
    const std::uint32_t elapsed = now_ms - since_ms;

    if (static_cast<std::int32_t>(elapsed) < 0) {
        return 0U;
    }

    return elapsed;
}

Step next_step(const DisplayPower power, const std::uint32_t since_activity_ms,
               const bool held_awake, const Timings& timings) {
    // ⛔ THE HOLD OUTRANKS THE CLOCK, AND IS CHECKED BEFORE IT. An actionable condition does not
    // become less actionable because nobody touched the button; a recession that ran anyway would
    // put a device fault behind a dark panel.
    if (held_awake) {
        return Step::Hold;
    }

    switch (power) {
    case DisplayPower::On:
        return since_activity_ms >= timings.acknowledge_ms ? Step::Dim : Step::Hold;

    case DisplayPower::Dimmed:
        // Measured from the same activity moment rather than from the moment of dimming, so the
        // two thresholds are two points on one timeline. Restarting the clock at each level would
        // make the total depend on how often the policy happened to be asked.
        //
        // ⛔ THE FADE IS INSIDE THE TOTAL, AND dim_hold_ms IS STABLE TIME AT DIMMED. UI Behaviour
        // Decision 001 §10.2 and Architecture Spec §17.8.3 fix the phases as
        //
        //     0 .. 20   Quiet          20 .. 22   fade
        //    22 .. 322  STABLE Dimmed  322 .. 324 fade      Off at 324
        //
        // and say the 300 s "begins only after the first fade has completed". So the threshold is
        // acknowledge + fade + hold, not acknowledge + hold.
        //
        // ⛔ OMITTING THE FADE HERE IS A REAL DEFECT AND WAS THE STATE OF THIS LINE UNTIL
        // 2026-08-31: it slept at t = 320, giving 298 s of stable Dimmed and a 322 s total against
        // an accepted 324. Two seconds is small; being unable to say which of two numbers the
        // product implements is not. A test pins every boundary.
        return since_activity_ms >= timings.acknowledge_ms + timings.fade_ms + timings.dim_hold_ms
                   ? Step::Sleep
                   : Step::Hold;

    case DisplayPower::Off:
        // Already at rest. There is nothing below Off, and nothing here may climb back up.
        return Step::Hold;
    }

    return Step::Hold;
}

std::string_view log_label(const Tier tier) {
    switch (tier) {
    case Tier::Active:
        return "active";

    case Tier::Quiet:
        return "quiet";
    }

    return "unknown";
}

std::uint8_t brightness_for(const DisplayPower power, const Tier tier, const Levels& levels) {
    switch (power) {
    case DisplayPower::On:
        // ⬅️ TWO ON LEVELS, ONE STRUCTURAL QUESTION. The tier comes from output_tier_for(),
        // so this is still a selection and still has no idea what a printer is doing. Owner
        // selections, 2026-08-30: Active 65 (duty 1588), Quiet 50 (duty 891) — 1.78x in light.
        return tier == Tier::Active ? levels.active_percent : levels.quiet_percent;

    case DisplayPower::Dimmed:
        // ⛔ SELECTED, NOT DERIVED — AND THAT IS THE REPAIR ITSELF. This used to read
        // `(on_percent * dimmed_percent_of_user) / 100`, which is a fraction of the CONTROL
        // value; the driver's gamma 2.2 then turned the intended 15 % of light into 1.5 %.
        // Both the invariants that expression carried — never brighter than the level it receded
        // from, never rounding into Off — now live with the exponent, in
        // display::percent_for_light_fraction(), where they can be stated in the units they are
        // actually about.
        //
        // The Dimmed endpoint remains derived from Quiet, including after the owner-approved
        // Completed Active acknowledgement (2026-09-08). Owner selection: 55 % of Quiet = control 38,
        // duty 487 — the output observed at harness step D2.
        return levels.dimmed_percent;

    case DisplayPower::Off:
        // True off: display::set_brightness_percent(0) takes the same duty-0 path as disabling
        // the backlight, so the LED string is not driven at all rather than driven faintly.
        return 0U;
    }

    return levels.quiet_percent;
}

bool operator==(const Output& lhs, const Output& rhs) {
    return lhs.target_percent == rhs.target_percent && lhs.fade_ms == rhs.fade_ms;
}

bool operator!=(const Output& lhs, const Output& rhs) {
    return !(lhs == rhs);
}

Output output_for(const DisplayPower power, const Tier tier, const Levels& levels,
                  const Timings& timings) {
    Output output;

    output.target_percent = brightness_for(power, tier, levels);
    output.fade_ms = power == DisplayPower::On ? 0U : timings.fade_ms;

    return output;
}

bool short_press_may_sleep(const AppState& state) {
    // ⛔ EXACTLY THE CONDITIONS THAT HOLD THE PANEL AWAKE, AND FOR THE SAME REASON. If a condition
    // is important enough that a timer may not darken it, a stray tap must not darken it either.
    //
    // Sharing one predicate is deliberate: two lists would drift, and the drift would be silent —
    // a state that a timer respected but a button did not is exactly the "accidentally became a
    // sleep command" case the owner's ruling names.
    //
    // The brightness selector also consumes this predicate. Completed's bounded Active
    // acknowledgement is an output exception, not an awake hold or a manual-sleep prohibition.
    return !holds_awake(state);
}

} // namespace prd::app_model::display_policy
