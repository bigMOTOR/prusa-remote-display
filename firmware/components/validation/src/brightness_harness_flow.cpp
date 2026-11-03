// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h FIRST, and explicitly. The CONFIG_* macro this file is gated on comes from there,
// and a #if on an undefined macro is silently false: without this include the whole flow below
// compiles to empty stubs and nothing warns.
#include "sdkconfig.h"

#include "validation/validation.hpp"

// The owner-paced brightness harness.
//
// ⛔ WHAT IT REPLACES, AND WHY. An automatic ladder walked the same levels on an 8-15 second
// schedule while chat named each step. The announcements arrived batched, the owner could not tell
// which level was on screen, and the run was ruled INVALID as a selection oracle: no Active, Quiet
// or Dimmed level was recorded from it, and neither a saturation nor a broken-path conclusion may
// be drawn from it. Chat timing is not an oracle. Here the PANEL carries the label, and nothing
// moves until a person presses the button.
//
// ⛔ IT OWNS THE BACKLIGHT WHILE COMPILED, and the display policy stands down — brightness_harness
// _owns_panel() is what app_core asks, on exactly the pattern display_patterns_own_panel()
// established. Two writers on one LEDC channel would interleave and the owner would be judging a
// race rather than a level.
//
// ⛔ IT CHANGES NO PRODUCTION BUTTON SEMANTICS. The gesture still reaches the store as the product
// fact it is; app_core hands this flow the same press afterwards, exactly as it already does for
// the scenario flow and the operational walk. This flow dispatches nothing and mutates no store.

#if defined(CONFIG_PRD_VALIDATION_BRIGHTNESS_HARNESS)

#if !defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
#error "PRD_VALIDATION_BRIGHTNESS_HARNESS requires PRD_UI_DIAGNOSTIC_SCENE; see its Kconfig help"
#endif

#if defined(CONFIG_PRD_VALIDATION_SCENARIOS) || defined(CONFIG_PRD_VALIDATION_OPERATIONAL_WALK)
#error                                                                                             \
    "PRD_VALIDATION_BRIGHTNESS_HARNESS consumes the same short press as the scenario flow and the operational walk; select one"
#endif

#include <cstddef>
#include <cstdint>
#include <string>

#include "brightness_harness_steps.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "display/display.hpp"
#include "ui/ui.hpp"

namespace prd::validation {
namespace {

using brightness_harness::kSteps;
using brightness_harness::Step;

// Owned by ONE task — app_core's button consumer, which is where every press arrives and where
// begin() is called from. No other task reads or writes it, so there is nothing to serialise.
std::size_t g_index = brightness_harness::kFirstStep;
bool g_started = false;

// A settled row owed once an outstanding ramp has landed. Only the two recession transitions
// carry a fade; every other step settles at the instant of the press.
bool g_settle_owed = false;
std::int64_t g_settle_due_us = 0;
std::size_t g_settle_index = 0;

[[nodiscard]] std::uint8_t control_percent(const Step& step) {
    // ⛔ THE DISPLAY COMPONENT CONVERTS, NEVER THIS FILE. The light fraction is a fraction of
    // EMITTED LIGHT, and brightness percentages are perceptually pre-distorted — the defect this
    // whole checkpoint corrected was that same fraction applied to the control value, which
    // emitted a tenth of what it named. Asking here means the bench shows exactly what the
    // product will show, because it is the same function.
    return display::percent_for_light_fraction(step.ceiling_percent,
                                               display::LightFraction{step.light_percent});
}

void emit_row(const char* const event, const std::size_t index, const std::uint8_t percent,
              const std::uint32_t transition_ms, const std::uint32_t duty) {
    const Step& step = kSteps[index];

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        std::string("harness ") + event + " step=" + step.id + " index=" +
            std::to_string(index + 1) + "/" + std::to_string(brightness_harness::kStepCount) +
            " phase=" + brightness_harness::phase_label(step.phase) + " ceiling=" +
            std::to_string(step.ceiling_percent) + " light=" + std::to_string(step.light_percent) +
            " percent=" + std::to_string(percent) + " duty=" + std::to_string(duty) +
            " transition_ms=" + std::to_string(transition_ms) + " label=\"" + step.label + "\"");

    diagnostics::timeline(diagnostics::tag::kApp, "harness", "index",
                          static_cast<std::int64_t>(index + 1), "percent", percent);
}

// Puts a step on the panel: the backlight target and the panel's own label, together.
void apply(const std::size_t index, const std::int64_t now_us) {
    const Step& step = kSteps[index];
    const std::uint8_t percent = control_percent(step);

    ui::DisplayIntent intent;
    intent.target_percent = percent;
    intent.fade_ms = step.fade_ms;
    ui::submit_display(intent);

    // ⬅️ THE LABEL GOES UP WITH THE LEVEL, NOT AFTER IT. It is the oracle; a label that lagged the
    // brightness would reproduce the defect that invalidated the previous run.
    ui::submit_diagnostic_label(ui::DiagnosticLabel{step.id, step.label});

    // The duty the target maps to, read out of the driver's own curve rather than recomputed, so
    // the row states the panel's output and not a percentage.
    emit_row("press", index, percent, step.fade_ms, display::duty_for_brightness_percent(percent));

    if (brightness_harness::fades_in(index)) {
        g_settle_owed = true;
        g_settle_index = index;
        g_settle_due_us = now_us + (static_cast<std::int64_t>(step.fade_ms) * 1000);
    } else {
        // Nothing to wait for: the press and the settled output are the same instant, and the row
        // above already reported it.
        g_settle_owed = false;
    }
}

} // namespace

bool brightness_harness_compiled() {
    return true;
}

bool brightness_harness_owns_panel() {
    return true;
}

void brightness_harness_begin(const app_model::Timestamp now) {
    if (g_started) {
        return;
    }

    g_started = true;
    g_index = brightness_harness::kFirstStep;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "harness begin — owner-paced; nothing advances without a short press");

    apply(g_index, now.count());
}

void brightness_harness_tick(const app_model::Timestamp now) {
    if (!g_settle_owed || now.count() < g_settle_due_us) {
        return;
    }

    g_settle_owed = false;

    // The MEASURED duty now that the ramp has landed, not the predicted one — the difference is
    // the only way a ramp that stopped short would ever show up in the record.
    emit_row("settled", g_settle_index, display::brightness_percent(),
             kSteps[g_settle_index].fade_ms, display::backlight_duty());
}

void brightness_harness_on_gesture(const app_model::ButtonGesture gesture,
                                   const app_model::Timestamp now) {
    if (!g_started) {
        return;
    }

    // ⛔ ONE PRESS, ONE STEP — AND ONLY A SHORT PRESS. Long and very long press keep whatever
    // product meaning the reducer gives them; they must not also walk the instrument, or a
    // factory-reset confirmation would silently change the level being judged.
    if (gesture != app_model::ButtonGesture::ShortPress) {
        return;
    }

    g_index = brightness_harness::next_index(g_index);

    // ⬅️ THE CALLER'S READING, NOT ONE TAKEN HERE. The settle deadline is `now + fade`, so a zero
    // here would mark every ramp as already landed and the settled row would fire before the panel
    // had moved — reporting a level that was never reached.
    apply(g_index, now.count());
}

} // namespace prd::validation

#else // CONFIG_PRD_VALIDATION_BRIGHTNESS_HARNESS

namespace prd::validation {

bool brightness_harness_compiled() {
    return false;
}

// ⛔ FALSE IS WHAT KEEPS THE PRODUCT POLICY IN CHARGE. Without the harness compiled, app_core's
// display_tick() runs the real recession and nothing here touches the backlight.
bool brightness_harness_owns_panel() {
    return false;
}

void brightness_harness_begin(app_model::Timestamp /*now*/) {}
void brightness_harness_tick(app_model::Timestamp /*now*/) {}
void brightness_harness_on_gesture(app_model::ButtonGesture /*gesture*/,
                                   app_model::Timestamp /*now*/) {}

} // namespace prd::validation

#endif // CONFIG_PRD_VALIDATION_BRIGHTNESS_HARNESS
