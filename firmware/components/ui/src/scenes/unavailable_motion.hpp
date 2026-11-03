// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/app_state.hpp"
#include "app_model/display_policy.hpp"
#include "scenes/operational_form.hpp"

// The accepted Candidate-D motion law — Production UI §9, UI Behaviour Decision 001 §5.1.
//
// INTERNAL HEADER of the ui component, and DELIBERATELY LVGL-FREE so the whole law is host-tested.
// Nothing here touches an lv_obj, a colour or a font: it answers "what shape, how far down, how
// opaque, at this many milliseconds", and the scene applies the answer.
//
// ---------------------------------------------------------------------------
// WHAT THE OWNER ACCEPTED, AND THE ONE CORRECTION PM MADE TO IT
// ---------------------------------------------------------------------------
//
// The accepted storyboard raster settles the horizon 18 px below its resting apex. That is not
// implementable: lv_arc draws its stroke INWARD from the radius and the crown's circle centre is
// below the arc, so the stroke extends downward from the path. An 18 px settlement paints to row
// 237 — two pixels from the display edge — and Production UI §7 forbids both leaving canvas.inset
// and touching the edge.
//
// PM's resolution, 2026-08-31: settle by the maximum legal tokenised amount, `space.2` = 8 px. At
// 8 px the crown's ends sit at 219 and the deepest painted pixel is 227, which is the last row
// inside the content box. The raster's own x = 9..310 width is likewise an artefact of a
// centre-drawn preview stroke and is NOT reproduced: the production form paints 13..307, inside
// canvas.inset, and this law changes nothing horizontal.
//
// ---------------------------------------------------------------------------
// THE PHASES
// ---------------------------------------------------------------------------
//
// THE PHASES, AND WHERE EVERY NUMBER COMES FROM
//
// Each constant below was MEASURED off the accepted rasters (candidate-D/ready-0*.png and
// printing-0*.png) rather than chosen: the bands of non-background pixels in each frame give the
// form's row, its accent and the copy's blend fraction at that frame's stated time.
//
//   0 .. 1500 ms   the SHAPE travels down §2.2's energy axis to the resting crown. The accent does
//                  not travel: §5.1 gives the form one colour at every energy (owner ruling,
//                  2026-09-01). A Resting origin is already at the crown, so for it this phase
//                  moves nothing. Measured: the Printing origin's raster is mid-axis at 0.8 s and
//                  at the crown by 1.5 s.
//   0 .. 1600 ms   the 8 px downward settlement, over the whole arrival. Measured: the Ready
//                  raster has travelled half its settlement at 0.8 s and all but a pixel at 1.5 s.
//   +1000 ms       `Printer unavailable` resolves by opacity, in a window that OPENS ONLY ONCE THE
//                  FORM HAS CLEARED ITS ROW — a quarter into the settlement for an origin whose form
//                  never occupies that row, and on the shape's arrival for one whose descent passes
//                  through it.
//
//                  Measured against the accepted rasters, whose blend fraction is read straight off
//                  the copy's pixels: ready-02.png is 95/255 resolved at 0.8 s and this law gives
//                  90; printing-03.png carries NO copy at 1.5 s and neither does this; and
//                  printing-04.png is 223/255 at 2.2 s against this law's 200. The residuals are
//                  inside the rasters' own 90 ms frame quantisation.
//   settled        1600 ms when the copy did not have to wait, 2500 ms when it did. Nothing moves
//                  after it: no pulse, no loop, no ambient animation — UIAD 001 §3 and §9.
//   +2000 ms       THE OUTGOING TAIL, after the model retires the acknowledgement — see
//                  kUnavailableOutgoingMs below. Not part of the motion; nothing moves during it.
//
// ⛔ THE COPY'S WINDOW IS ORIGIN-DEPENDENT FOR A REASON, NOT FOR VARIETY. A single early window
// draws the words underneath a ring that is still unwinding through their row: measured against
// scene::crosses(), the descending form struck the copy's line box on 50 of 201 sampled frames.
// The words wait for their row to be clear, which is exactly what the accepted rasters show.
//
// The universal 20 s On window, the 2 s fade to Dimmed, the 300 s Dimmed hold and true Off are NOT
// here: they belong to the one central display-power policy and this law must never duplicate them.

namespace prd::ui::scene {

// How long the shape takes to travel down the axis. The accent does not travel — see the phase
// table above and UnavailablePhase::accent below.
inline constexpr std::uint32_t kUnavailableFormMs = 1500;

// How long the downward settlement takes, from the start of the motion.
inline constexpr std::uint32_t kUnavailableSettleMs = 1600;

// How long the copy takes to resolve, once its row is clear.
inline constexpr std::uint32_t kUnavailableCopyMs = 1000;

// The settled downward travel of the crown. `space.2`, and the maximum §7 permits — see above.
inline constexpr std::int32_t kUnavailableSettleY = 8;

// --- B2: the READY-ORIGIN entry, owner-accepted 2026-09-02 -------------------
//
// ⛔ WHY READY NEEDS ITS OWN PHASING, AND NO OTHER ORIGIN DOES. origin_energy(Ready) already IS
// the destination energy, so the energy interpolation is a no-op for it: every frame of the
// general law shares one geometry, and the whole entry was an 8 px translation plus the copy. On
// the panel that read as no motion at all — physical validation 2026-09-02 recorded entry
// continuity as FAIL, with the owner reporting the panel lit already settled. The settlement had
// itself been halved from the accepted raster's 18 px to `space.2` for a legality reason, which
// removed most of what little motion Ready had.
//
// B2 changes NO endpoint, shape, span, stroke, accent, copy or clock. It changes only the PHASE
// RELATIONSHIP, in two halves:
//
//   0 .. 800 ms      the apex lifts 196 -> 180. Copy alpha is EXACTLY 0 for the whole half.
//   800 .. 1600 ms   ONE progress through ONE easing settles the apex 180 -> 204 and reveals the
//                    copy 0 -> 255, so both finish at the same instant.
//
// ⬅️ THE COPY CARRIES THE FORM'S OWN OFFSET AND HAS NO MOTION OF ITS OWN. Both are placed from
// `offset_y`, so the 74 px horizon-to-copy relationship is constant by construction rather than by
// arithmetic that could drift. A negative offset lifts both together.
inline constexpr std::int32_t kReadyLiftY = 16;
inline constexpr std::uint32_t kReadyPivotMs = 800;

static_assert(kReadyPivotMs < kUnavailableSettleMs,
              "B2: the pivot is inside the entry, so a descent half exists");
static_assert(kReadyLiftY > kUnavailableSettleY,
              "B2: the lift is what makes the entry perceptible, so it exceeds the settlement");

// Where the copy's line box sits before the settlement, derived from the form rather than chosen.
//
// ⛔ DEFINED HERE, IN THE LVGL-FREE LAW, AND NOWHERE ELSE. The scene places the label at it, the copy
// window is decided from it, and the firing control measures against it — three consumers, one
// definition, so a moved row cannot pass a test that still measures the old one.
inline constexpr std::int32_t kUnavailableCopyBaseY =
    theme::kHorizonApexY - theme::kLineBody - (theme::kSpace8 + theme::kSpace6);

static_assert(kUnavailableCopyBaseY > theme::kCanvasContent.y,
              "the copy stays inside canvas.inset");
static_assert(kUnavailableCopyBaseY + theme::kLineBody + kUnavailableSettleY < theme::kHorizonApexY,
              "the copy resolves ABOVE the form's apex at every point of the settlement");

// How long the last settled frame stays on the panel AFTER the model has retired the
// acknowledgement — Candidate D's accepted retirement.
//
// ⛔ IT IS THE CENTRAL DISPLAY FADE, NOT A SCHEDULE OF ITS OWN. The accepted storyboard has the
// settled copy and horizon still present at t = 20.6 s, dimming with the ordinary display fade, and
// an empty result only once that fade completes at t = 22.0 s. Hiding at t = 20.0 s the instant the
// model retires the acknowledgement is a different, rejected sequence.
//
// ⬅️ AND IT IS RENDERER-OWNED OUTGOING RESIDUE, NOT A SECOND SEMANTIC ACKNOWLEDGEMENT. The model
// truthfully says the acknowledgement retired at the start of On -> Dimmed; this is only how long the
// pixels of the last frame outlive that, and it is exactly the fade they are being faded by.
inline constexpr std::uint32_t kUnavailableOutgoingMs =
    app_model::display_policy::Timings{}.fade_ms;

// ⛔ THE ONE GUARD AGAINST A SECOND SCHEDULE. If anyone writes an Unavailable-specific literal here,
// or moves the central timeline without moving this, the arithmetic stops agreeing with the ratified
// sequence — 20 s acknowledged, 2 s fade, Dimmed at 22 s.
static_assert(app_model::display_policy::Timings{}.acknowledge_ms + kUnavailableOutgoingMs ==
                  22000U,
              "the outgoing tail must end exactly when the accepted timeline reaches Dimmed");

// When this origin's copy may begin: the moment its row is clear of the form.
[[nodiscard]] std::uint32_t copy_from_ms(app_model::LossOriginForm origin);

// When this origin's motion is wholly arrived — the later of its settlement and its copy.
[[nodiscard]] std::uint32_t total_ms(app_model::LossOriginForm origin);

// The subject state whose form this origin names. Pure, and here only so that origin_energy() can ask
// the PRODUCTION axis rather than carry a table of its own.
[[nodiscard]] app_model::SubjectState subject_state_of(app_model::LossOriginForm origin);

// Whether the retired acknowledgement's last frame is still owed to the panel, `elapsed_ms` after the
// model retired it. False from the moment the fade completes, and false for ever after.
[[nodiscard]] bool outgoing_visible(std::uint32_t elapsed_ms);

// The entry motion's clock, and the ONE rule that governs when it starts.
//
// ⛔ THE MODEL MAY PUBLISH AN ACKNOWLEDGEMENT WHILE THE PANEL IS DARK. update_loss_episode()
// deliberately has no display-power gate — its only gate is §12 precedence — and the wake is
// dispatched separately by the application task. So without this, an entry could run its whole
// 1600 ms behind a backlight at duty 0 and the panel would light already settled. On 2026-09-02 the
// wake happened to lead by 1 ms, which made the ordering incidental rather than guaranteed.
//
// ⛔ IT IS UI-LOCAL AND DECIDES NOTHING ELSE. It does not move display policy into the loss
// reducer, does not touch the model's acknowledgement lifetime or ownership, and does not infer
// visibility from confidence, from the selected scene or from wall time. Visibility is supplied by
// the render task from the display component's own applied-brightness seam.
//
// PURE AND HEADER-ONLY so the rule is host-tested where it can be asked directly, for the reason
// operational_form.hpp records: this repository has paid for arithmetic no build check could see.
// The shared entry easing — the smootherstep the whole product's 1100 ms clock runs on.
//
// ⬅️ ONE DEFINITION, BESIDE THE CLOCK THAT DRIVES IT. It lived in the access scene's anonymous
// namespace until the message family gave it three more callers, and "equivalent transitions shall
// use identical timing" (Production UI §10) is not a property three separate copies of a quintic
// can be trusted to keep.
[[nodiscard]] inline double entry_progress(const std::uint32_t elapsed) {
    const double progress = static_cast<double>(elapsed) / theme::kOperationalMotionMs;
    return progress * progress * progress * ((progress * ((progress * 6.0) - 15.0)) + 10.0);
}

// A clock that HOLDS while the panel cannot be seen, rather than only refusing to start.
//
// ⛔ THE DIFFERENCE FROM EntryClock, AND WHY IT IS A SECOND TYPE. EntryClock refuses to START
// before the panel is visible, which is the whole of the wake-order safeguard it was written for.
// It does not pause a motion that has already begun: the tick keeps running behind a dark
// backlight, so an entry interrupted by a dim at 300 ms is finished and settled by the time
// anybody sees it. Root's 2026-09-12 review names that distinction, and the owner's rule for the
// message and reset paths is the stronger one — no motion finishing unseen while Off.
//
// It is a second type rather than a change to EntryClock because EntryClock's behaviour is part
// of the accepted acknowledgement and access compositions, whose evidence was sealed against it.
// Nothing that already runs on EntryClock is altered here.
//
// ⬅️ AND IT IS NOT THE DOT PAUSE. The Connecting dot loop pauses by dropping its anchor and
// re-anchoring on wake, so it resumes at the phase it was at without catching up. This holds a
// bounded one-shot motion at the position it reached. Both refuse to advance in the dark; only
// one of them has an end to arrive at.
struct HoldClock {
    bool armed{false};
    bool running{false};
    std::uint32_t elapsed_ms{0};
    std::uint32_t last_ms{0};

    void arm() {
        armed = true;
        running = false;
        elapsed_ms = 0;
        last_ms = 0;
    }

    void clear() {
        armed = false;
        running = false;
        elapsed_ms = 0;
        last_ms = 0;
    }

    // Called once per render tick. Returns whether time moved this tick; elapsed() is the
    // position to paint either way, so a held clock paints the frame it reached and no other.
    bool advance(const bool panel_visible, const std::uint32_t now_ms) {
        if (!armed) {
            return false;
        }

#ifdef PRD_OLD_ENTRY_CLOCK
        // The pre-2026-09-12 behaviour, restored for the negative control in
        // tools/test-message-controls.sh: start-gated, but not held. A motion already running
        // carried on through a dim and was settled before anybody saw it.
        const bool held = !panel_visible && !running;
#else
        const bool held = !panel_visible;
#endif

        if (held) {
            running = false;
            return false;
        }

        if (!running) {
            running = true;
            last_ms = now_ms;
            return true;
        }

        elapsed_ms += now_ms - last_ms;
        last_ms = now_ms;
        return true;
    }

    [[nodiscard]] std::uint32_t elapsed() const {
        return elapsed_ms;
    }
};

struct EntryClock {
    bool armed{false};
    bool started{false};
    std::uint32_t start_ms{0};

    // A new presentation generation. The clock is owed, but it has not begun.
    void arm() {
        armed = true;
        started = false;
        start_ms = 0;
    }

    void clear() {
        armed = false;
        started = false;
        start_ms = 0;
    }

    // Called once per render tick. Returns whether the clock is running; while it is not, the
    // caller must render at elapsed 0, so the first frame the viewer can SEE is the first frame.
    bool advance(const bool panel_visible, const std::uint32_t now_ms) {
        if (!armed) {
            return false;
        }

        if (!started && panel_visible) {
            started = true;
            start_ms = now_ms;
        }

        return started;
    }
};

// Render-task handoff of painted structure only: never a record, state or diagnostic report.
struct FormPosition {
    Energy energy{0};
    std::int32_t offset_y{0};
    std::uint8_t opacity{255};
};

// One frame of the motion.
struct UnavailablePhase {
    // Where on §5.1's accepted axis this frame is. The single parameter the rest is derived from,
    // carried here so the shape and the colour are provably answers to the SAME energy.
    Energy energy{0};

    // The operational form's geometry this frame, from the accepted sagitta law.
    FormGeometry form{};

    // How far down the composition is drawn, 0 .. kUnavailableSettleY.
    //
    // ⬅️ THE COPY CARRIES IT TOO, which is what the accepted rasters show: both bands descend by the
    // same amount between their 0.8 s and 1.5 s frames. One settlement, one composition.
    std::int32_t offset_y{0};

    // Appearance interrupted while returning from an empty panel. Not energy attenuation.
    std::uint8_t form_opacity{255};

    // 0 .. 255. The copy resolves by opacity alone; it never moves independently of the form.
    std::uint8_t copy_alpha{0};

    // How the form is painted this frame — §3.2's rule, from the operational family's own
    // form_treatment().
    //
    // ⛔ CARRIED RATHER THAN DECIDED AT THE DRAW SITE, and that is the whole point: the renderer
    // consumes this verdict, so the host tests assert the decision production actually paints. A
    // closed shape with no record MUST be TrackOnly. Painting a closed cycle in the accent instead
    // produces the one thing this composition may never show — a full accent cycle, which is what a
    // completed 100 % record looks like, on a panel that has no reading at all.
    FormTreatment treatment{FormTreatment::None};

    // The form's colour this frame — always `color.accent`, at every frame of every origin.
    //
    // ⛔ IT DOES NOT TRAVEL WITH THE ENERGY. §5.1, as ruled by the owner on 2026-09-01, gives the
    // operational form one accent at every point on the axis; the descent is carried by the shape
    // alone. The field is retained per frame rather than read from the token at the draw site for
    // the reason ui.hpp gives about the report: what is recorded is the value handed to LVGL.
    //
    // ⬅️ REJECTED HISTORY. This field once carried a ramp's output, from 40 % of the accent at rest
    // (#623212) up to the token at Printing. Both that endpoint and the mechanism are rejected.
    // UnavailableMotionTests::EveryFrameOfEveryOriginCarriesTheOneAccent is the firing control.
    std::uint32_t accent{0};

    // True once nothing will move again without a new generation. The UI loop stops tightening its
    // cadence here, which is what keeps a resting device from being spun up to animate nothing.
    bool settled{false};
};

// The energy a structural origin starts from — one per axis position, through energy_for().
//
// ⛔ AT ELAPSED ZERO THE TRANSITION FORM IS EXACTLY form_geometry(origin_energy(origin)) FOR ALL SIX,
// which is the existing production shape for that state. That equality is what makes the form leave
// as one continuous body instead of jumping to an endpoint, and it is asserted for every origin.
[[nodiscard]] Energy origin_energy(app_model::LossOriginForm origin);

// Continuous handoff when loss interrupts a still-moving operational composition. Canonical
// entries retain phase_at(origin, elapsed) exactly; only the actual painted start is substituted.
[[nodiscard]] UnavailablePhase phase_from_position(app_model::LossOriginForm origin,
                                                   FormPosition start, std::uint32_t elapsed_ms);

// The motion at `elapsed_ms` since the presentation began.
//
// Deterministic and total: every input yields a frame, and `elapsed_ms` beyond the motion yields
// the settled frame rather than running on.
[[nodiscard]] UnavailablePhase phase_at(app_model::LossOriginForm origin, std::uint32_t elapsed_ms);

// WHERE THE TWO ELEMENTS ARE PLACED — the ONE placement rule, and the only one.
//
// ⛔ THESE EXIST SO THE PLACEMENT IS TESTABLE AGAINST PRODUCTION RATHER THAN AGAINST ITSELF. The
// regression that claimed the copy never moves independently of the form previously subtracted two
// expressions the TEST built from the same supplied offset, so its answer was guaranteed
// algebraically and would have survived any divergence in what the renderer actually did.
// scene::UnavailableAcknowledgement::apply() now places both elements through these, so a
// regression reading them is reading what is drawn.
[[nodiscard]] std::int32_t form_top_y(const UnavailablePhase& phase);
[[nodiscard]] std::int32_t copy_top_y(const UnavailablePhase& phase);

} // namespace prd::ui::scene
