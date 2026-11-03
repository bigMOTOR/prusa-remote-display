// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly. This translation unit is gated on a CONFIG_PRD_UI_*
// macro, and a #if on an undefined macro is silently false — which during Milestone 2
// Phase B turned five gated files into empty stubs with a clean build.
#include "sdkconfig.h"

#include "controller.hpp"

#include <string>

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "fonts/fonts.hpp"
#include "presentation_diff.hpp"
#include "scene_selection.hpp"
#include "scenes/access_scene.hpp"
#include "scenes/factory_reset_scene.hpp"
#include "scenes/firmware_update_scene.hpp"
#include "scenes/operational_scene.hpp"
#include "scenes/provisioning_scene.hpp"
#include "scenes/unavailable_acknowledgement.hpp"
#include "text/text.hpp"

namespace prd::ui::controller {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

lv_obj_t* g_screen = nullptr;

// One scene for the whole operational family — see operational_scene.hpp.
scene::OperationalScene g_operational{};

// The provisional provisioning composition — Milestone 3.6, owner ruling R3.
scene::ProvisioningScene g_provisioning{};
scene::FactoryResetScene g_factory_reset{};
scene::FirmwareUpdateScene g_firmware_update{};
scene::AccessScene g_access{};

#if defined(CONFIG_PRD_UI_SCENE_ANNOTATION)
// Bench annotation for presentations with no composition — see the Kconfig help.
//
// Compiled out of debug and release. It carries no printer data and invents nothing: the
// text is the projection's own log labels, which is why a black panel can be trusted to
// mean a fault rather than an unimplemented screen.
lv_obj_t* g_annotation = nullptr;
#endif

// The accepted Candidate-D acknowledgement of lost observation — Production UI §9, UIAD 001 §11.1.
//
// ⛔ NOT A SCENE, AND DELIBERATELY NOT IN THE Scene ENUMERATION. Architecture Spec §17.2 removes
// printer unavailable from the scene list; this lives in §17.3's transient-overlay slot, above
// whichever scene is selected, and is shown and hidden by the model's phase.
scene::UnavailableAcknowledgement g_observation_lost{};

// The animation's own bookkeeping, and NOTHING ELSE. Ownership — whether an acknowledgement exists
// at all — is AppState::loss_episode; this is only where the motion is up to.
//
// ⛔ THE GENERATION IS THE MODEL'S. A change in it, and only that, restarts the motion. The render
// task never decides from a clock whether an acknowledgement is owed.
//
// Zero is the model's reserved "nothing is acknowledged" value — see next_presentation_generation()
// — so it is what this holds while nothing is on the panel, and an arriving acknowledgement always
// differs from it.
std::uint32_t g_ack_generation = 0;

// When the generation on the panel began, and whether its motion is still moving.
//
// ⛔ `running` IS NOT `visible`. A settled acknowledgement is still on the panel; what has ended is
// only the motion, and with it the loop's need for a tighter cadence.
//
// ⬅️ THE CLOCK IS scene::EntryClock, AND IT DOES NOT START ON ARRIVAL. It starts on the first tick
// at which the panel can actually be seen — see note_panel_visible() and the header's contract on
// EntryClock. Until then the composition is drawn at elapsed 0, so the first frame a viewer sees is
// the first frame of the motion and never a settled one.
scene::EntryClock g_entry{};
bool g_ack_running = false;

// Forwarded by the render task after the backlight has been applied for this tick.
bool g_panel_visible = false;
app_model::LossOriginForm g_ack_origin = app_model::LossOriginForm::Ready;
std::optional<scene::FormPosition> g_ack_start{};

// THE OUTGOING TAIL — Candidate D's accepted retirement, and renderer-owned residue only.
//
// The model retires the acknowledgement at the start of On -> Dimmed, truthfully; the accepted
// storyboard still has the settled copy and horizon on the panel at t = 20.6 s, dimming with the
// ordinary display fade, and an empty result only when that fade completes at t = 22.0 s. So the last
// frame outlives its own retirement by exactly that fade — scene::kUnavailableOutgoingMs, which IS
// the central display-policy fade and not a schedule of this file's own.
//
// ⛔ NOTHING MOVES DURING IT. It is the same pixels, being faded by the backlight. The loop is not
// tightened for it and no new frame is composed.
std::uint32_t g_ack_tail_started_ms = 0;
bool g_ack_tailing = false;

// Draws the entry's very first frame — used while the clock is armed but the panel is not yet
// visible, so that the composition already on the panel IS frame zero when the light arrives.
void draw_first_entry_frame();

// Clears the composition and every piece of its bookkeeping. The one place the overlay goes away.
void hide_acknowledgement() {
    g_ack_generation = 0;
    g_ack_running = false;
    g_ack_tailing = false;
    g_entry.clear();
    g_ack_start.reset();
    g_observation_lost.hide();
}

void draw_first_entry_frame() {
    g_observation_lost.apply(g_ack_start ? scene::phase_from_position(g_ack_origin, *g_ack_start, 0)
                                         : scene::phase_at(g_ack_origin, 0));
}

app_model::Presentation g_applied{};
bool g_has_applied = false;
Scene g_scene = Scene::Unimplemented;

Report g_report{};

// Writes the bench annotation, or hides it when a real composition is being drawn.
//
// A no-op in debug and release, where the parameter is unused and the whole body
// disappears — so app_core and the controller carry no #if of their own around the call.
void annotate([[maybe_unused]] const app_model::Presentation* const presentation) {
#if defined(CONFIG_PRD_UI_SCENE_ANNOTATION)
    if (g_annotation == nullptr) {
        return;
    }

    if (presentation == nullptr) {
        lv_obj_add_flag(g_annotation, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // The projection's own labels. Nothing here is a user-visible product string, so
    // nothing here belongs in the text catalogue.
    const auto layer = app_model::log_label(presentation->layer);
    const auto subject = app_model::log_label(presentation->subject);
    const auto confidence = app_model::log_label(presentation->confidence);

    lv_label_set_text_fmt(g_annotation, "no scene: %.*s / %.*s / %.*s",
                          static_cast<int>(layer.size()), layer.data(),
                          static_cast<int>(subject.size()), subject.data(),
                          static_cast<int>(confidence.size()), confidence.data());
    lv_obj_remove_flag(g_annotation, LV_OBJ_FLAG_HIDDEN);
#endif
}

// Shows exactly one scene and hides the rest. Written as a full pass over every scene
// rather than "hide the old, show the new" so that two scenes can never both be
// visible — including after a build failure, where the old scene's pointer might be
// the only valid one.
void show_only(const Scene scene) {
    g_operational.set_visible(scene == Scene::Operational);
    g_provisioning.set_visible(scene == Scene::Provisioning);
    g_factory_reset.set_visible(scene == Scene::FactoryReset);
    g_firmware_update.set_visible(scene == Scene::FirmwareUpdate);
    g_access.set_visible(scene == Scene::Access);
}

} // namespace

bool build(lv_display_t* const display_handle) {
    lv_obj_t* const screen = lv_display_get_screen_active(display_handle);
    if (screen == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "no active screen");
        return false;
    }

    g_screen = screen;

    // §17.3's background, and §5's canvas colour. Applied to the screen itself rather
    // than to a child: one fewer object, and the screen is the only thing guaranteed
    // to cover every pixel, so no gap can expose LVGL's default grey.
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, theme::colour(theme::Colour::Background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, theme::opacity(theme::Opacity::Full), LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    // §4's Body role is set on the screen so it inherits down the tree. Every label
    // sets its own role explicitly, so this is a backstop rather than a default in
    // use: LVGL's own LV_FONT_DEFAULT is a diagnostic fallback that no product text
    // should ever reach, and inheriting a real face here means a label that somehow
    // missed apply_text_style() is legible while still being wrong.
    lv_obj_set_style_text_font(screen, fonts::for_role(theme::TextRole::Body), LV_PART_MAIN);

    if (!g_operational.build(screen)) {
        return false;
    }

    if (!g_factory_reset.build(screen)) {
        return false;
    }

    if (!g_firmware_update.build(screen)) {
        return false;
    }

    if (!g_provisioning.build(screen)) {
        return false;
    }
    if (!g_access.build(screen)) {
        return false;
    }

    // Built last so it sits above every scene in LVGL's z-order — §17.3's transient overlay. It
    // starts hidden: nothing is acknowledged until a presentation says so.
    if (!g_observation_lost.build(screen)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "acknowledgement build failed");
        return false;
    }

#if defined(CONFIG_PRD_UI_SCENE_ANNOTATION)
    g_annotation = theme::bare_label(screen);
    if (g_annotation == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "scene annotation failed");
        return false;
    }

    // Metadata role, dimmest step on the luminance scale, inside the canvas inset and
    // clear of every region a Phase C composition occupies. It is deliberately hard to
    // mistake for product typography.
    theme::apply_text_style(g_annotation, theme::TextRole::Metadata, theme::Colour::Tertiary,
                            theme::Opacity::Medium);
    lv_obj_set_width(g_annotation, theme::kCanvasContent.width);
    lv_obj_set_style_text_align(g_annotation, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(g_annotation, theme::kCanvasContent.x,
                   theme::kCanvasContent.bottom() - theme::kLineMicro);
    lv_label_set_text(g_annotation, "");
    lv_obj_add_flag(g_annotation, LV_OBJ_FLAG_HIDDEN);
#endif

    // Nothing is presentable until the first presentation arrives. §9's Boot screen is
    // a Phase D composition, so what shows until then is the background — which is
    // honest: the device has resolved no state to display yet.
    show_only(Scene::Unimplemented);

    g_has_applied = false;
    g_scene = Scene::Unimplemented;
    g_report = Report{};
    g_report.scene = log_label(g_scene);

    diagnostics::log(diagnostics::LogLevel::Info, kTag, "scene controller built");
    return true;
}

void apply(const app_model::Presentation& presentation) {
    if (g_screen == nullptr) {
        return;
    }

    ++g_report.applied;

    // The cheapest and most important check: an unchanged presentation draws nothing.
    // The Printing composition receives one of these on every poll that reported the
    // same values, and §11 forbids redrawing then.
    if (g_has_applied && presentation == g_applied) {
        ++g_report.redundant;
        return;
    }

    const PresentationDelta delta = diff(g_applied, presentation, !g_has_applied);
    const Scene selected = select(presentation);
    const Scene previous_scene = g_scene;
    // Read the old painted composition before show_only hides it. No semantic value crosses
    // this seam, and nothing is inferred by reversing a diagnostic geometry report.
    const auto departing_form =
        g_scene == Scene::Operational ? g_operational.painted_form() : std::nullopt;

    if (delta.scene || !g_has_applied) {
        ++g_report.scene_changes;
        show_only(selected);
        g_scene = selected;
        g_report.scene = log_label(selected);

        diagnostics::log(diagnostics::LogLevel::Info, kTag,
                         "scene=" + std::string{log_label(selected)} +
                             " layer=" + std::string{app_model::log_label(presentation.layer)} +
                             " rev_applied=" + std::to_string(g_report.applied));
    }

    if (selected == Scene::Operational) {
        ++g_report.operational;
        annotate(nullptr);
        if (g_observation_lost.visible()) {
            const auto& painted = g_observation_lost.painted_phase();
            g_operational.resume_from_form(
                {painted.energy, painted.offset_y, painted.form_opacity});
        } else if ((g_has_applied && g_applied.observation_lost) ||
                   previous_scene == Scene::Access || previous_scene == Scene::FactoryReset ||
#if !defined(PRD_OLD_SETUP_SUCCESS_ENTRY)
                   previous_scene == Scene::Provisioning ||
#endif
                   previous_scene == Scene::FirmwareUpdate) {
            // ⬅️ AND THE RETURN FROM A MESSAGE IS AN ENTRY TOO — root finding R4, 2026-09-12.
            // Cancelling the reset question, or a device fault clearing, put the operational
            // composition back on a panel that was showing something else entirely; without this
            // it simply reappeared, fully drawn, in one frame. It is the same accepted mechanism
            // Access already used and the same accepted 1100 ms appearance — the ENDPOINT is
            // untouched, and the subject's own values are not re-acquired or re-animated: the
            // seed carries every pose value except the appearance alpha.
            g_operational.resume_from_blank();
        }
        g_operational.apply(presentation, delta);
    } else if (selected == Scene::Access) {
        annotate(nullptr);
        g_access.apply(presentation);
    } else if (selected == Scene::Provisioning) {
        ++g_report.provisioning;
        annotate(nullptr);
        g_provisioning.apply(presentation);
    } else if (selected == Scene::FactoryReset) {
        ++g_report.factory_reset;
        annotate(nullptr);
        g_factory_reset.apply(presentation);
    } else if (selected == Scene::FirmwareUpdate) {
        ++g_report.firmware_update;
        annotate(nullptr);
        g_firmware_update.apply(presentation);
    } else {
        // Counted, not silent. A presentation with no composition is a gap in the
        // implementation, and it should be visible in the log rather than inferred
        // from a blank panel — and, in the validation profile, on the panel too.
        ++g_report.unimplemented;
        annotate(&presentation);
    }

    // The transient acknowledgement, applied AFTER the scene so it is the last word on what is
    // visible — Production UI §9, UIAD 001 §11.1.
    //
    // ⛔ PHASE, NOT INFERENCE. The projection has already decided whether an acknowledgement is on
    // the panel and which generation it belongs to; the reducer decided that from the loss episode
    // and §12's precedence. Nothing here consults a timer, the current brightness or the visible
    // scene, and nothing here may.
    if (presentation.observation_lost_acknowledgement) {
        // ⛔ THE GENERATION RESTARTS THE MOTION, AND NOTHING ELSE DOES. A presentation carrying the
        // same generation is the same acknowledgement — it may be the fifth failed poll of an
        // unreachable printer — so the motion continues from where it is instead of jumping back to
        // its first frame. An inequality, not an ordering comparison, because the model's counter is
        // documented to wrap.
        //
        // ⛔ AND THE CONDITION IS THE GENERATION ALONE. Keying it on "or the motion is not running"
        // instead would replay a SETTLED acknowledgement whenever any unrelated field of the same
        // presentation changed, which is a second acknowledgement of one loss.
        if (presentation.observation_lost_generation != g_ack_generation) {
            g_ack_generation = presentation.observation_lost_generation;
            g_ack_origin = presentation.observation_lost_origin;
            g_ack_start = g_ack_origin == app_model::LossOriginForm::NoOperationalForm
                              ? std::nullopt
                              : departing_form;
            g_ack_running = true;

            // ⛔ ARMED, NOT STARTED. The model may have published this while the panel is still
            // dark; stamping a start here is what would let the whole entry run behind a backlight
            // at duty 0. The clock begins in step_acknowledgement(), on the first visible tick.
            g_entry.arm();
        }

        // A new generation during an outgoing tail is a new acknowledgement — an explicit press from
        // true Off, typically — so the residue ends and the entry motion begins.
        g_ack_tailing = false;

        // Drawn here as well as from the loop's tick, so the first frame lands with the
        // presentation rather than up to one wait later. Discarded deliberately: this call site
        // draws, and it is the loop that cares whether anything is still moving.
        static_cast<void>(step_acknowledgement());
    } else if (g_ack_generation != 0 && presentation.observation_lost &&
               presentation.observation_lost_eligible) {
        // ⬅️ RETIRED WHILE STILL ELIGIBLE, WHICH IS THE ORDINARY RECESSION: the printer is still
        // unobservable and nothing has taken the panel, so this is the 20 s retirement or a permitted
        // manual sleep. The last frame is owed the fade it is being faded by.
        //
        // ⛔ THE TWO CONDITIONS ARE BOTH LOAD-BEARING AND NEITHER IS A PRECEDENCE DECISION.
        // `observation_lost` false is recovery; `observation_lost_eligible` false is a higher-priority
        // layer or an unresolved subject condition. Both fall through to the immediate hide below.
        // The precedence list itself is resolved once, in the model.
        if (!g_ack_tailing) {
            g_ack_tailing = true;
            g_ack_tail_started_ms = lv_tick_get();
            g_ack_running = false;
        }

        static_cast<void>(step_acknowledgement());
    } else {
        // ⬅️ RECOVERY AND DISPLACEMENT LAND HERE IDENTICALLY, AND INTERRUPT AT ANY PHASE — entry
        // motion, settled, or mid-tail. Neither owes the old frame anything: the current scene is
        // what should be on the panel, immediately.
        hide_acknowledgement();
    }

    g_applied = presentation;
    g_has_applied = true;
}

void note_panel_visible(const bool visible) {
    g_panel_visible = visible;
}

void set_setup_session(const SetupSession& session) {
    // Applied whatever is on screen. The session and the provisioning phase arrive on
    // separate queues and either order has to produce the same panel — the scene writes
    // its rows now and the phase reveals them, or the reverse.
    g_provisioning.apply_session(session);
}

void clear_setup_session() {
    g_provisioning.clear_session();
}

bool step_acknowledgement() {
    // The render loop's cadence covers the whole operational family, not only loss.
    const bool operational_running =
        g_scene == Scene::Operational && g_operational.step(g_panel_visible, lv_tick_get());
    const bool access_running =
        g_scene == Scene::Access && g_access.step(g_panel_visible, lv_tick_get());

    // ⬅️ THE MESSAGE FAMILY TICKS TOO — 2026-09-12. Both scenes draw the shared message
    // composition and both now have a 1100 ms entry to run, so both have to be reached from the
    // render loop. Before this they were the only compositions in the product that appeared by
    // substitution, which is what made the owner's all-transitions rule unmet for them.
    const bool provisioning_running =
        g_scene == Scene::Provisioning && g_provisioning.step(g_panel_visible, lv_tick_get());
    const bool factory_reset_running =
        g_scene == Scene::FactoryReset && g_factory_reset.step(g_panel_visible, lv_tick_get());
    const bool firmware_update_running =
        g_scene == Scene::FirmwareUpdate && g_firmware_update.step(g_panel_visible, lv_tick_get());
    const bool scene_running = operational_running || access_running || provisioning_running ||
                               factory_reset_running || firmware_update_running;
    // The outgoing tail first: nothing about it moves, so it never tightens the loop. It only has to
    // be noticed when it expires — and while it runs the backlight ramp is holding the loop at its
    // ramp cadence anyway, so the hide lands within a frame of the fade completing.
    if (g_ack_tailing) {
        if (!scene::outgoing_visible(lv_tick_elaps(g_ack_tail_started_ms))) {
            hide_acknowledgement();
        }

        return scene_running;
    }

    if (!g_ack_running) {
        return scene_running;
    }

    // ⛔ THE WAKE-ORDER SAFEGUARD, AND IT IS THE WHOLE OF IT. Until the panel can be seen the clock
    // does not advance, and the composition is drawn at elapsed 0 — so the first VISIBLE frame is
    // the first frame of the motion. Returning true keeps the loop at its ramp cadence, so the
    // brightness this iteration applied is noticed within a frame rather than at the idle wait.
    if (!g_entry.advance(g_panel_visible, lv_tick_get())) {
        draw_first_entry_frame();
        return true;
    }

    // lv_tick_elaps() rather than a subtraction: LVGL's tick is a 32-bit millisecond counter that
    // wraps after ~49 days, and this is the function that accounts for the wrap.
    const auto elapsed = lv_tick_elaps(g_entry.start_ms);
    const scene::UnavailablePhase phase =
        g_ack_start ? scene::phase_from_position(g_ack_origin, *g_ack_start, elapsed)
                    : scene::phase_at(g_ack_origin, elapsed);

    g_observation_lost.apply(phase);

    // ⛔ SETTLED MEANS THE TICK STOPS, and this return value is the whole reason the law reports it.
    // The composition stays on the panel — the acknowledgement is not over — but nothing about it
    // will change again without a new generation, so the loop returns to its ordinary wait. There
    // is no pulse, no loop and no ambient animation to keep alive (UIAD 001 §3, §9).
    if (phase.settled) {
        g_ack_running = false;
        return scene_running;
    }

    return true;
}

Report report() {
    // ⛔ ANSWERED AT READ TIME, NOT AT apply() TIME, AND THAT IS THE POINT. The overlay outlives its
    // own retirement by the outgoing fade and is then hidden by the loop's tick, with no presentation
    // involved — a flag written during apply() would still be claiming it was on the panel. This asks
    // the composition what it is actually showing.
    //
    // Still no LVGL call: visible() is a plain retained bool, which is what keeps this read-back
    // usable from outside the render task.
    g_report.observation_lost_acknowledged = g_observation_lost.visible();

    return g_report;
}

OperationalFormReport operational_form_report() {
    return g_operational.form_report();
}

OperationalFormReport observation_lost_form_report() {
    if (!g_observation_lost.visible()) {
        return {};
    }
    const auto& painted = g_observation_lost.painted_phase();
    auto form = painted.form;
    form.centre_y += painted.offset_y;
    return scene::form_report(painted.energy, form, painted.accent, false);
}

} // namespace prd::ui::controller
