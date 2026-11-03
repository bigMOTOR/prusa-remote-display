// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <vector>

#include "scenes/operational_accent.hpp"
#include "scenes/operational_energy.hpp"
#include "scenes/operational_form.hpp"
#include "scenes/unavailable_motion.hpp"
#include "theme/layout.hpp"

// The accepted Candidate-D motion law.
//
// What is protected here is a visual direction the owner accepted from a raster storyboard, and the
// ONE correction PM made to it: the raster settles 18 px, which paints two pixels from the display
// edge, so production settles by the maximum legal tokenised 8 px instead. Every assertion below
// that mentions 8 is that correction, and the containment tests are why it is 8 and not 18.

using namespace prd::ui;
using prd::ui::scene::copy_from_ms;
using prd::ui::scene::copy_top_y;
using prd::ui::scene::form_top_y;
using prd::ui::scene::kReadyLiftY;
using prd::ui::scene::kReadyPivotMs;
using prd::ui::scene::kUnavailableCopyBaseY;
using prd::ui::scene::kUnavailableCopyMs;
using prd::ui::scene::kUnavailableFormMs;
using prd::ui::scene::kUnavailableOutgoingMs;
using prd::ui::scene::kUnavailableSettleMs;
using prd::ui::scene::kUnavailableSettleY;
using prd::ui::scene::origin_energy;
using prd::ui::scene::outgoing_visible;
using prd::ui::scene::phase_at;
using prd::ui::scene::subject_state_of;
using prd::ui::scene::total_ms;
using Origin = prd::app_model::LossOriginForm;

namespace {

// The deepest pixel the crown paints, at an offset. lv_arc draws inward and the crown's centre is
// below the arc, so the stroke extends DOWNWARD from the path — which is the whole reason 18 px is
// illegal.
[[nodiscard]] std::int32_t deepest_painted(const scene::FormGeometry& form,
                                           const std::int32_t offset_y) {
    return scene::end_y(form) + theme::kRingStroke + offset_y;
}

// All six positions on UIDD 004 §2.2's axis, which is exactly the set LossOriginForm names.
[[nodiscard]] std::vector<Origin> every_origin() {
    return {Origin::Ready,     Origin::Stopped, Origin::Preparing,
            Origin::Completed, Origin::Paused,  Origin::Printing};
}

[[nodiscard]] const char* label(const Origin origin) {
    switch (origin) {
    case Origin::Ready:
        return "Ready";
    case Origin::Stopped:
        return "Stopped";
    case Origin::Preparing:
        return "Preparing";
    case Origin::Completed:
        return "Completed";
    case Origin::Paused:
        return "Paused";
    case Origin::Printing:
        return "Printing";
    case Origin::NoOperationalForm:
        return "NoOperationalForm";
    }

    return "?";
}

} // namespace

TEST(UnavailableMotionTests, TheSettlementIsTheTokenisedEightPixels) {
    EXPECT_EQ(kUnavailableSettleY, theme::kSpace2) << "space.2, not a literal";
    EXPECT_EQ(kUnavailableSettleY, 8);
}

// ⛔ FIRING CONTROL for a broken motion endpoint. Both halves fail if the settlement changes: too
// far and the form leaves the content box, too little and it is not the accepted composition.
TEST(UnavailableMotionTests, TheSettledFormStaysInsideTheContentBox) {
    const auto settled = phase_at(Origin::Ready, total_ms(Origin::Ready));

    EXPECT_EQ(settled.offset_y, kUnavailableSettleY);
    EXPECT_LE(deepest_painted(settled.form, settled.offset_y), theme::kCanvasContent.bottom() - 1)
        << "Production UI §7: all content stays inside canvas.inset";
    EXPECT_LT(deepest_painted(settled.form, settled.offset_y), theme::kDisplayHeight - 1)
        << "§7: no component may touch the display edge";

    // And the raster's 18 px would not have.
    EXPECT_GT(deepest_painted(settled.form, 18), theme::kCanvasContent.bottom() - 1)
        << "the accepted raster's settlement is not implementable; 8 px is the legal maximum";
}

TEST(UnavailableMotionTests, TheSettledFormIsTheProductionWidthHorizon) {
    const auto settled = phase_at(Origin::Ready, total_ms(Origin::Ready));
    const scene::FormGeometry resting = scene::form_geometry(scene::kEnergyReady);

    // Same accepted endpoint, unchanged: only the vertical offset differs.
    EXPECT_EQ(settled.form.radius, resting.radius);
    EXPECT_EQ(settled.form.centre_x, resting.centre_x);
    EXPECT_EQ(settled.form.half_angle, resting.half_angle);
    EXPECT_EQ(settled.form.extent, resting.extent);

    // Horizontal containment, and NOT the raster's accidental 9..310.
    //
    // ⛔ BOTH OFFSETS ARE ADDED, BECAUSE BOTH ARE SIGNED. paint_left_offset() returns a NEGATIVE
    // displacement from the form's own centre — form_report() adds it too. Subtracting it produced a
    // left edge of 307, which cleared canvas.inset trivially and asserted nothing; the sign is
    // pinned below so the same mistake cannot pass again.
    const theme::Rect box = scene::bounding_box(settled.form);
    ASSERT_LT(scene::paint_left_offset(settled.form, theme::kRingStroke), 0);
    ASSERT_GT(scene::paint_right_offset(settled.form, theme::kRingStroke), 0);

    const std::int32_t left =
        settled.form.centre_x + scene::paint_left_offset(settled.form, theme::kRingStroke);
    const std::int32_t right =
        settled.form.centre_x + scene::paint_right_offset(settled.form, theme::kRingStroke);
    EXPECT_GE(left, theme::kCanvasContent.x);
    EXPECT_LE(right, theme::kCanvasContent.right() - 1);

    // And the span is the horizon's, not a degenerate one — the second half of the same guard.
    EXPECT_GT(right - left, theme::kCanvasContent.width - theme::kSpace4)
        << "the settled form is the edge-to-edge horizon";
    EXPECT_EQ(box.width, resting.radius * 2);
}

// ---------------------------------------------------------------------------
// THE ORIGIN: ALL SIX OF THE AXIS, AND NO ENDPOINT JUMP
// ---------------------------------------------------------------------------

// ⛔ THE LOAD-BEARING EQUALITY. At elapsed zero the transition's form must be EXACTLY the production
// shape for that state — form_geometry(energy_for(origin)) — for all six. Anything else is a jump
// away from what the panel was showing, at the very instant of loss.
TEST(UnavailableMotionTests, EveryOriginBeginsAtItsOwnProductionForm) {
    for (const Origin origin : every_origin()) {
        const std::optional<scene::Energy> axis = scene::energy_for(subject_state_of(origin));
        ASSERT_TRUE(axis.has_value()) << label(origin) << " must be ON the axis";
        EXPECT_EQ(origin_energy(origin), *axis) << label(origin);

        const scene::FormGeometry expected = scene::form_geometry(*axis);
        const scene::FormGeometry actual = phase_at(origin, 0).form;

        EXPECT_EQ(actual.radius, expected.radius) << label(origin);
        EXPECT_EQ(actual.centre_x, expected.centre_x) << label(origin);
        EXPECT_EQ(actual.centre_y, expected.centre_y) << label(origin);
        EXPECT_EQ(actual.sagitta, expected.sagitta) << label(origin);
        EXPECT_EQ(actual.half_width, expected.half_width) << label(origin);
        EXPECT_EQ(actual.half_angle, expected.half_angle) << label(origin);
        EXPECT_EQ(actual.extent, expected.extent) << label(origin);
        EXPECT_EQ(actual.rotation, expected.rotation) << label(origin);
        EXPECT_EQ(actual.closed, expected.closed) << label(origin);

        // And the entry frame is drawn in the one accent — §5.1 as ruled on 2026-09-01. The entry
        // SHAPE is the state's own, asserted exhaustively above; the colour is not, and must not be.
        EXPECT_EQ(phase_at(origin, 0).accent, scene::accent()) << label(origin);
    }
}

// ⛔ FIRING CONTROL for the two-bucket collapse. `LossOriginForm::{Resting, Carrying}` made Preparing
// and Paused start at Printing's closed cycle and Completed and Stopped start at Ready's horizon: six
// states, two shapes. Six DISTINCT entry shapes is what proves the origin is no longer lossy.
TEST(UnavailableMotionTests, TheSixOriginsAreSixDistinctEntryShapes) {
    std::set<scene::Energy> energies;
    std::set<std::int32_t> sagittas;
    std::set<std::int32_t> extents;
    std::set<std::int32_t> half_angles;

    for (const Origin origin : every_origin()) {
        const auto entry = phase_at(origin, 0);
        energies.insert(entry.energy);
        sagittas.insert(entry.form.sagitta);
        extents.insert(entry.form.extent);
        half_angles.insert(entry.form.half_angle);
    }

    EXPECT_EQ(energies.size(), 6U) << "two buckets would give 2";
    EXPECT_EQ(sagittas.size(), 6U) << "two buckets would give 2";
    EXPECT_EQ(extents.size(), 6U) << "two buckets would give 2";
    EXPECT_EQ(half_angles.size(), 6U) << "two buckets would give 2";

    // ⬅️ NOT THE RADIUS, AND THAT IS THE AXIS's OWN PROPERTY RATHER THAN A COLLAPSE HERE. Once the
    // sagitta reaches the half width the accepted relation pins the radius to that half width, so
    // Completed, Paused and Printing all sit on a 96 px circle and differ by sagitta and span alone.
    // Measured: radii are 693, 197, 102, 96, 96, 96.
    std::set<std::int32_t> radii;
    for (const Origin origin : every_origin()) {
        radii.insert(phase_at(origin, 0).form.radius);
    }
    EXPECT_EQ(radii.size(), 4U) << "the accepted axis pins r to w once |s| >= w";

    // Only Printing's shape is a closed cycle; the other five are open, which is §3.2's whole point.
    for (const Origin origin : every_origin()) {
        EXPECT_EQ(phase_at(origin, 0).form.closed, origin == Origin::Printing) << label(origin);
    }
}

// Whatever the origin, the body travels to the SAME resting crown and never re-energises on the way.
TEST(UnavailableMotionTests, EveryOriginDescendsMonotonicallyToTheOneSettledFrame) {
    const scene::FormGeometry resting = scene::form_geometry(scene::kEnergyReady);

    for (const Origin origin : every_origin()) {
        scene::Energy previous = phase_at(origin, 0).energy;

        for (std::uint32_t t = 0; t <= total_ms(origin); t += 10) {
            const auto phase = phase_at(origin, t);
            EXPECT_LE(phase.energy, previous) << label(origin) << " re-energised at t = " << t;
            previous = phase.energy;

            // Every intermediate shape is one the family already reaches, and stays inside §7's box.
            EXPECT_EQ(phase.form.sagitta, scene::form_geometry(phase.energy).sagitta)
                << label(origin) << " at t = " << t;
            EXPECT_LE(deepest_painted(phase.form, phase.offset_y),
                      theme::kCanvasContent.bottom() - 1)
                << label(origin) << " at t = " << t;
        }

        const auto settled = phase_at(origin, total_ms(origin));
        EXPECT_EQ(settled.energy, scene::kEnergyReady) << label(origin);
        EXPECT_EQ(settled.form.sagitta, resting.sagitta) << label(origin);
        EXPECT_EQ(settled.form.radius, resting.radius) << label(origin);
        EXPECT_EQ(settled.offset_y, kUnavailableSettleY) << label(origin);
        EXPECT_EQ(settled.copy_alpha, 255) << label(origin);
    }
}

// ---------------------------------------------------------------------------
// §3.2's TREATMENT: NO CLOSED ACCENT CYCLE, EVER
// ---------------------------------------------------------------------------

// ⛔ THE PHASE CARRIES THE OPERATIONAL FAMILY'S OWN VERDICT, and the renderer paints that verdict. So
// this asserts the decision production actually uses rather than a parallel copy of the rule.
TEST(UnavailableMotionTests, TheTreatmentIsTheOperationalFamilysOwnDecision) {
    for (const Origin origin : every_origin()) {
        for (std::uint32_t t = 0; t <= total_ms(origin) + 200; t += 10) {
            const auto phase = phase_at(origin, t);

            // draws_record is FALSE for ever here: a lost observation has no reading to host.
            EXPECT_EQ(phase.treatment, scene::form_treatment(true, phase.form.closed, false))
                << label(origin) << " at t = " << t;
        }
    }
}

// ⛔ FIRING CONTROL against restoring the full accent cycle. A closed shape with no record MUST be
// TrackOnly: a closed cycle painted in the accent is exactly what a completed 100 % record looks
// like, and this composition has no reading at all.
TEST(UnavailableMotionTests, AClosedFormIsNeverPaintedAsAnAccentCycle) {
    // The Printing origin's entry frame IS closed — that is the case the defect was in.
    const auto entry = phase_at(Origin::Printing, 0);
    ASSERT_TRUE(entry.form.closed) << "the Printing origin begins at the closed cycle";
    EXPECT_EQ(entry.treatment, scene::FormTreatment::TrackOnly)
        << "a closed no-record form is the silent track, never the accent";

    std::int32_t closed_frames = 0;
    std::int32_t open_frames = 0;

    for (const Origin origin : every_origin()) {
        for (std::uint32_t t = 0; t <= total_ms(origin) + 200; t += 10) {
            const auto phase = phase_at(origin, t);

            if (phase.form.closed) {
                ++closed_frames;
                EXPECT_EQ(phase.treatment, scene::FormTreatment::TrackOnly)
                    << label(origin) << " at t = " << t;
            } else {
                ++open_frames;
                EXPECT_EQ(phase.treatment, scene::FormTreatment::FormAccent)
                    << label(origin) << " at t = " << t;
            }

            // And no record sweep exists anywhere in this acknowledgement, at any frame.
            EXPECT_NE(phase.treatment, scene::FormTreatment::RecordSweep)
                << label(origin) << " at t = " << t;
            EXPECT_NE(phase.treatment, scene::FormTreatment::None)
                << label(origin) << " at t = " << t;
        }
    }

    EXPECT_GT(closed_frames, 0) << "the control must actually have sampled a closed form";
    EXPECT_GT(open_frames, 0) << "and an open one";
}

// ---------------------------------------------------------------------------
// THE OUTGOING TAIL: CANDIDATE D's ACCEPTED RETIREMENT
// ---------------------------------------------------------------------------

// ⛔ FIRING CONTROL A — an immediate hide at t = 20.0. The accepted storyboard still has the settled
// copy and horizon on the panel at t = 20.6 s, dimming with the ordinary fade.
//
// ⛔ FIRING CONTROL B — content surviving past t = 22.0. The accepted result at 22.0 s is empty.
TEST(UnavailableMotionTests, TheLastFrameOutlivesItsRetirementByExactlyTheDisplayFade) {
    // A: still owed, from the instant of retirement to the last millisecond of the fade.
    EXPECT_TRUE(outgoing_visible(0)) << "hiding at t = 20.0 is the rejected sequence";
    EXPECT_TRUE(outgoing_visible(600)) << "t = 20.6 s, the accepted storyboard's own frame";
    EXPECT_TRUE(outgoing_visible(kUnavailableOutgoingMs - 1));

    // B: and owed nothing at all from the moment the fade completes, for ever.
    EXPECT_FALSE(outgoing_visible(kUnavailableOutgoingMs)) << "t = 22.0 s is empty";
    for (const std::uint32_t t : {2001U, 3000U, 302000U, 3600000U}) {
        EXPECT_FALSE(outgoing_visible(t)) << "t = " << t;
    }
}

// ⛔ AND IT IS THE CENTRAL FADE, NOT A SCHEDULE OF ITS OWN. A second literal here would let the
// display policy and the overlay drift apart silently.
TEST(UnavailableMotionTests, TheTailIsTheDisplayPolicysOwnFadeAndNotASecondSchedule) {
    const prd::app_model::display_policy::Timings timings{};

    EXPECT_EQ(kUnavailableOutgoingMs, timings.fade_ms) << "the same fade, not a copy of its value";
    EXPECT_EQ(timings.acknowledge_ms, 20000U) << "the ratified On window";
    EXPECT_EQ(timings.acknowledge_ms + kUnavailableOutgoingMs, 22000U)
        << "the tail ends exactly where the accepted timeline reaches Dimmed";

    // The tail is strictly shorter than the Dimmed hold that follows it, so it cannot bleed into a
    // phase where the acknowledgement is long gone.
    EXPECT_LT(kUnavailableOutgoingMs, timings.dim_hold_ms);
}

TEST(UnavailableMotionTests, ReadyBeginsAtTheAcceptedHorizonAndPrintingAtTheRing) {
    const auto ready = phase_at(Origin::Ready, 0);
    const auto printing = phase_at(Origin::Printing, 0);

    EXPECT_EQ(ready.form.radius, scene::form_geometry(scene::kEnergyReady).radius);
    EXPECT_FALSE(ready.form.closed);

    EXPECT_EQ(printing.form.radius, scene::form_geometry(scene::kEnergyPrinting).radius);
    EXPECT_TRUE(printing.form.closed) << "the Printing origin begins at the accepted ring";

    EXPECT_EQ(scene::origin_energy(Origin::Ready), scene::kEnergyReady);
    EXPECT_EQ(scene::origin_energy(Origin::Printing), scene::kEnergyPrinting);
}

TEST(UnavailableMotionTests, ThePrintingRingChangesContinuouslyIntoTheCrown) {
    std::int32_t previous = scene::form_geometry(scene::kEnergyPrinting).sagitta;

    // Sampled every 50 ms: the sagitta must descend monotonically from the ring's to the crown's,
    // never reverse, and arrive exactly at the accepted resting value.
    for (std::uint32_t t = 0; t <= kUnavailableFormMs; t += 50) {
        const auto phase = phase_at(Origin::Printing, t);
        EXPECT_LE(phase.form.sagitta, previous)
            << "at t = " << t << " the form must not re-energise";
        previous = phase.form.sagitta;
    }

    EXPECT_EQ(previous, scene::form_geometry(scene::kEnergyReady).sagitta);
}

TEST(UnavailableMotionTests, EveryIntermediateShapeStaysInsideTheContentBox) {
    for (const Origin origin : {Origin::Ready, Origin::Printing}) {
        for (std::uint32_t t = 0; t <= total_ms(origin) + 500; t += 25) {
            const auto phase = phase_at(origin, t);
            EXPECT_LE(deepest_painted(phase.form, phase.offset_y),
                      theme::kCanvasContent.bottom() - 1)
                << "t = " << t;
        }
    }
}

// ⛔ FIRING CONTROL against a reinstated accent ramp. §5.1, as ruled by the owner on 2026-09-01,
// gives the operational form ONE accent at every point on the axis. The descent is expressed by the
// shape alone. This suite previously asserted the exact opposite — that the colour travelled with
// the energy — and it is inverted here rather than deleted, because the frames it sweeps are the
// ones a reinstatement would show up in first.
//
// The rejected Ready endpoint, #623212, is recomputed rather than trusted as a literal: see
// operational_accent_tests.cpp, which proves the same derivation lands on it.
TEST(UnavailableMotionTests, EveryFrameOfEveryOriginCarriesTheOneAccent) {
    constexpr std::uint32_t kRejectedRestingAccent = 0x623212U;

    const std::uint32_t token = scene::accent();
    ASSERT_EQ(token, theme::kColorAccent);
    ASSERT_NE(token, kRejectedRestingAccent) << "the control is aimed at a value production draws";

    // EVERY origin, and every frame of each — entry, the whole descent, the settled frame and past
    // it. A ramp reinstated anywhere on the axis fails here, including one that agreed with the
    // token at the endpoints and diverged only in between.
    for (const Origin origin : {Origin::Ready, Origin::Stopped, Origin::Preparing,
                                Origin::Completed, Origin::Paused, Origin::Printing}) {
        for (std::uint32_t t = 0; t <= total_ms(origin) + 500; t += 10) {
            const auto phase = phase_at(origin, t);

            ASSERT_EQ(phase.accent, token) << label(origin) << " at t = " << t;
            ASSERT_NE(phase.accent, kRejectedRestingAccent) << label(origin) << " at t = " << t;
        }
    }
}

// ⬅️ AND THE SHAPE STILL TRAVELS, which is what the ruling did NOT change. Removing the colour ramp
// must not quietly flatten the descent too: Candidate D's motion is unchanged, and the energy still
// answers the form at every frame.
TEST(UnavailableMotionTests, TheShapeStillTravelsTheAxisWithTheEnergy) {
    for (std::uint32_t t = 0; t <= total_ms(Origin::Printing); t += 20) {
        const auto phase = phase_at(Origin::Printing, t);
        EXPECT_EQ(phase.form.sagitta, scene::form_geometry(phase.energy).sagitta)
            << "the shape answers the same energy, at t = " << t;
    }

    // The Printing origin begins closed at its own crown and arrives at the resting horizon.
    EXPECT_EQ(phase_at(Origin::Printing, 0).form.sagitta,
              scene::form_geometry(scene::kEnergyPrinting).sagitta);
    EXPECT_EQ(phase_at(Origin::Printing, kUnavailableFormMs).form.sagitta,
              scene::form_geometry(scene::kEnergyReady).sagitta);

    // And it does not simply switch at the end: the midpoint is neither endpoint.
    const std::int32_t middle = phase_at(Origin::Printing, kUnavailableFormMs / 2).form.sagitta;
    EXPECT_NE(middle, scene::form_geometry(scene::kEnergyPrinting).sagitta);
    EXPECT_NE(middle, scene::form_geometry(scene::kEnergyReady).sagitta);
}

// The copy's window, measured off the accepted rasters — see the header's phase table.
TEST(UnavailableMotionTests, TheCopyWaitsForItsRowAndThenResolvesMonotonically) {
    // ⬅️ SUPERSEDED FOR READY BY B2 — owner ruling 2026-09-02, and the reason is recorded rather
    // than the numbers quietly moved. The measured raster window (400 -> 1400, with 95/255 resolved
    // at 0.8 s off ready-02.png) belonged to the entry that PHYSICAL VALIDATION REJECTED: for a
    // Ready origin the general law's energy interpolation is a no-op, so its whole entry was an
    // 8 px translation and the panel read as already settled. B2 holds the copy at EXACTLY 0
    // through the 800 ms lift and then reveals it on the descent's own progress. The raster
    // figures stay in this comment as the record of what was measured, not as the contract.
    //
    // Every other origin below is untouched, and that is asserted rather than assumed.
    EXPECT_EQ(copy_from_ms(Origin::Ready), kReadyPivotMs);
    EXPECT_EQ(phase_at(Origin::Ready, 0).copy_alpha, 0);
    EXPECT_EQ(phase_at(Origin::Ready, kReadyPivotMs).copy_alpha, 0)
        << "B2: the lift carries no copy at all";
    EXPECT_EQ(phase_at(Origin::Ready, 1500).copy_alpha, 244);
    EXPECT_EQ(phase_at(Origin::Ready, kUnavailableSettleMs).copy_alpha, 255);

    // A carrying origin's words wait for the shape to arrive. Measured: the accepted
    // printing-02.png and printing-03.png carry NO copy at 0.8 s or at 1.5 s.
    EXPECT_EQ(copy_from_ms(Origin::Printing), kUnavailableFormMs);
    EXPECT_EQ(phase_at(Origin::Printing, 800).copy_alpha, 0);
    EXPECT_EQ(phase_at(Origin::Printing, kUnavailableFormMs).copy_alpha, 0);
    EXPECT_NEAR(phase_at(Origin::Printing, 2200).copy_alpha, 223, 30)
        << "the accepted printing raster's own blend fraction at 2.2 s";
    EXPECT_EQ(phase_at(Origin::Printing, total_ms(Origin::Printing)).copy_alpha, 255);

    // Monotonic for both: it resolves, it does not flicker.
    for (const auto origin : {Origin::Ready, Origin::Printing}) {
        std::uint8_t previous = 0;
        for (std::uint32_t t = 0; t <= total_ms(origin); t += 25) {
            const std::uint8_t alpha = phase_at(origin, t).copy_alpha;
            EXPECT_GE(alpha, previous) << "t = " << t;
            previous = alpha;
        }
    }
}

// ⛔ FIRING CONTROL: THE FORM MUST NEVER BE DRAWN THROUGH THE WORDS. This is the defect the earlier
// single early window had — the descending ring struck the copy's line box on 50 of 201 sampled
// frames — and scene::crosses() is the project's own predicate for it, the one the operational scene
// logs a warning on.
TEST(UnavailableMotionTests, TheFormNeverStrikesTheCopyWhileTheCopyIsVisible) {
    // The composition's own geometry, restated here from its two production constants so that
    // moving either of them fails this test rather than silently reintroducing the overlap.
    const std::int32_t base_y =
        theme::kHorizonApexY - theme::kLineBody - (theme::kSpace8 + theme::kSpace6);

    std::int32_t visible_frames = 0;
    for (const auto origin : {Origin::Ready, Origin::Printing}) {
        for (std::uint32_t t = 0; t <= total_ms(origin) + 200; t += 10) {
            const auto phase = phase_at(origin, t);
            if (phase.copy_alpha == 0) {
                continue;
            }

            ++visible_frames;

            // The form and the copy carry the same settlement, so the box moves with it.
            scene::FormGeometry drawn = phase.form;
            drawn.centre_y += phase.offset_y;
            const theme::Rect box{theme::kCanvasContent.x, base_y + phase.offset_y,
                                  theme::kCanvasContent.width, theme::kLineBody};

            EXPECT_FALSE(scene::crosses(drawn, theme::kRingStroke, box))
                << "the form is drawn through the visible copy at t = " << t;
        }
    }

    EXPECT_GT(visible_frames, 100) << "the control must actually have sampled visible copy";
}

// ⛔ NO PERPETUAL ANIMATION. `settled` is what lets the UI loop drop back to its idle cadence, and
// the law must report it exactly once the motion is over and for ever after.
TEST(UnavailableMotionTests, MotionSettlesAndStaysSettled) {
    // Each origin settles when everything it has to do is done, and not before: the later of its
    // settlement and its copy. A resting origin therefore stops ticking 800 ms sooner.
    EXPECT_EQ(total_ms(Origin::Ready), kUnavailableSettleMs);
    EXPECT_EQ(total_ms(Origin::Printing), kUnavailableFormMs + kUnavailableCopyMs);
    EXPECT_GT(total_ms(Origin::Printing), total_ms(Origin::Ready));

    for (const auto origin : {Origin::Ready, Origin::Printing}) {
        EXPECT_FALSE(phase_at(origin, 0).settled);
        EXPECT_FALSE(phase_at(origin, total_ms(origin) - 1).settled);
        EXPECT_TRUE(phase_at(origin, total_ms(origin)).settled);

        // Far beyond the motion, and beyond the whole 324 s display-power timeline, the frame is the
        // same settled frame — nothing continues, nothing pulses.
        const auto a = phase_at(origin, total_ms(origin));
        for (const std::uint32_t t : {3000U, 20000U, 324000U, 3600000U}) {
            const auto b = phase_at(origin, t);
            EXPECT_TRUE(b.settled) << "t = " << t;
            EXPECT_EQ(b.offset_y, a.offset_y);
            EXPECT_EQ(b.copy_alpha, a.copy_alpha);
            EXPECT_EQ(b.form.sagitta, a.form.sagitta);
        }
    }
}

TEST(UnavailableMotionTests, BothOriginsConvergeOnTheIdenticalSettledFrame) {
    const auto ready = phase_at(Origin::Ready, total_ms(Origin::Ready));
    const auto printing = phase_at(Origin::Printing, total_ms(Origin::Printing));

    EXPECT_EQ(ready.form.sagitta, printing.form.sagitta);
    EXPECT_EQ(ready.form.radius, printing.form.radius);
    EXPECT_EQ(ready.offset_y, printing.offset_y);
    EXPECT_EQ(ready.copy_alpha, printing.copy_alpha);
}

// ===========================================================================================
// B2 — THE READY-ORIGIN ENTRY, owner-accepted 2026-09-02
// ===========================================================================================
//
// Physical validation on 2026-09-02 passed the copy, the settled composition and the retirement,
// and FAILED entry continuity: no animation was perceived, and the owner reported the panel lit
// already settled. The capture showed why. For a Ready origin the energy interpolation is a no-op,
// so every frame of the general law shared one geometry and the entire entry was an 8 px
// translation plus the copy — a spec consequence, not a rendering defect.
//
// B2 changes ONLY the phase relationship. What follows pins all nine accepted keyframes, both
// endpoints, and the properties the owner's contract names; the firing controls below each detect
// one specific way of undoing it.

namespace {

// The nine accepted B2 keyframes: t, offset_y, copy alpha. Derived from the integer law, not from
// the design script — the pack that carried them was a preview; this is the contract.
struct B2Keyframe {
    std::uint32_t t_ms;
    std::int32_t offset_y;
    std::uint8_t copy_alpha;
};

constexpr std::array<B2Keyframe, 9> kB2{{
    {0, 0, 0},
    {200, -2, 0},
    {400, -8, 0},
    {600, -13, 0},
    {800, -16, 0},
    {1000, -13, 39},
    {1200, -4, 127},
    {1400, 4, 214},
    {1600, 8, 255},
}};

} // namespace

TEST(ReadyEntryB2Tests, AllNineAcceptedKeyframesAreExact) {
    for (const auto& k : kB2) {
        const auto phase = phase_at(Origin::Ready, k.t_ms);

        EXPECT_EQ(phase.offset_y, k.offset_y) << "offset at t = " << k.t_ms;
        EXPECT_EQ(phase.copy_alpha, k.copy_alpha) << "alpha at t = " << k.t_ms;
    }
}

TEST(ReadyEntryB2Tests, BothEndpointsAreTheProductionContract) {
    // t = 0 IS the production Ready frame: the resting geometry, no settlement, no copy.
    const auto first = phase_at(Origin::Ready, 0);
    const scene::FormGeometry resting = scene::form_geometry(scene::kEnergyReady);

    EXPECT_EQ(first.offset_y, 0);
    EXPECT_EQ(first.copy_alpha, 0);
    EXPECT_EQ(first.form.radius, resting.radius);
    EXPECT_EQ(first.form.centre_y, resting.centre_y);
    EXPECT_EQ(first.form.sagitta, resting.sagitta);
    EXPECT_EQ(first.form.extent, resting.extent);
    EXPECT_EQ(first.form.rotation, resting.rotation);

    // t = 1600 IS the accepted settled Candidate-D frame.
    const auto last = phase_at(Origin::Ready, kUnavailableSettleMs);

    EXPECT_TRUE(last.settled);
    EXPECT_EQ(last.offset_y, kUnavailableSettleY);
    EXPECT_EQ(last.copy_alpha, 255);
    EXPECT_EQ(last.form.radius, resting.radius);
    EXPECT_EQ(last.form.sagitta, resting.sagitta);
    EXPECT_EQ(last.form.extent, resting.extent);
}

// ⛔ FIRING CONTROL 2 — no copy before the pivot. Every millisecond, not the keyframes alone: a
// rounded fraction of an alpha anywhere in the lift is the ghosting the owner rejected.
TEST(ReadyEntryB2Tests, CopyAlphaIsExactlyZeroThroughTheWholeLift) {
    for (std::uint32_t t = 0; t <= kReadyPivotMs; ++t) {
        ASSERT_EQ(phase_at(Origin::Ready, t).copy_alpha, 0) << "ghosting at t = " << t;
    }

    // And it is still zero AT the pivot, so the boundary belongs to the lift.
    EXPECT_EQ(phase_at(Origin::Ready, kReadyPivotMs).copy_alpha, 0);
}

TEST(ReadyEntryB2Tests, TheLiftRisesMonotonicallyToTheFullAmplitude) {
    std::int32_t previous = 0;

    for (std::uint32_t t = 0; t <= kReadyPivotMs; ++t) {
        const std::int32_t offset = phase_at(Origin::Ready, t).offset_y;

        ASSERT_LE(offset, previous) << "the lift reversed at t = " << t;
        ASSERT_GE(offset, -kReadyLiftY) << "the lift overshot at t = " << t;
        previous = offset;
    }

    EXPECT_EQ(phase_at(Origin::Ready, kReadyPivotMs).offset_y, -kReadyLiftY);
}

// ⛔ FIRING CONTROL 3 — the descent and the reveal are ONE progress. Reconstructing the shared
// eased value from the offset must reproduce the alpha at every frame of phase two.
TEST(ReadyEntryB2Tests, DescentAndRevealSharePhaseTwosProgress) {
    for (std::uint32_t t = kReadyPivotMs + 1; t <= kUnavailableSettleMs; ++t) {
        const auto phase = phase_at(Origin::Ready, t);

        // offset = -lift + ((settle + lift) * s) / 1000  =>  s = (offset + lift) * 1000 / span
        const std::int32_t span = kUnavailableSettleY + kReadyLiftY;
        const std::int32_t recovered = ((phase.offset_y + kReadyLiftY) * 1000) / span;

        // The same s must produce the alpha. Integer truncation in both directions bounds the
        // agreement to one step of each quantity rather than to an exact equality.
        const std::int32_t expected = (255 * recovered) / 1000;

        ASSERT_LE(std::abs(static_cast<std::int32_t>(phase.copy_alpha) - expected), 11)
            << "copy and descent disagree at t = " << t << ": alpha " << +phase.copy_alpha
            << " against " << expected;
    }
}

TEST(ReadyEntryB2Tests, TheDescentAndTheRevealFinishAtTheSameInstant) {
    // Neither arrives early: one millisecond before the end, both are still short of their ends.
    const auto nearly = phase_at(Origin::Ready, kUnavailableSettleMs - 1);
    EXPECT_LT(nearly.offset_y, kUnavailableSettleY);
    EXPECT_LT(nearly.copy_alpha, 255);

    // And both arrive together, exactly at the entry's end.
    const auto arrived = phase_at(Origin::Ready, kUnavailableSettleMs);
    EXPECT_EQ(arrived.offset_y, kUnavailableSettleY);
    EXPECT_EQ(arrived.copy_alpha, 255);
}

TEST(ReadyEntryB2Tests, TheCopyNeverMovesIndependentlyOfTheHorizon) {
    // ⬅️ REWRITTEN 2026-09-02, BECAUSE THE PREVIOUS VERSION WAS VACUOUS. It subtracted two
    // expressions the TEST itself built from the same supplied offset, so the 74 px answer was
    // guaranteed algebraically and would have survived any divergence in what the renderer drew.
    //
    // ⛔ THIS READS PRODUCTION'S OWN PLACEMENT SEAM. scene::form_top_y() and scene::copy_top_y()
    // are what UnavailableAcknowledgement::apply() places both elements through, so mutating either
    // of them fails this test — which is the firing control recorded alongside it.
    const std::int32_t gap =
        form_top_y(phase_at(Origin::Ready, 0)) - copy_top_y(phase_at(Origin::Ready, 0));

    EXPECT_EQ(gap, 74) << "the accepted horizon-to-copy relationship";

    for (std::uint32_t t = 0; t <= kUnavailableSettleMs; ++t) {
        const auto phase = phase_at(Origin::Ready, t);

        ASSERT_EQ(form_top_y(phase) - copy_top_y(phase), gap)
            << "the copy moved independently of the form at t = " << t;
    }

    // Stated as the mechanism too, so a placement that happened to preserve the gap by a second
    // arithmetic route still reads as a change here: BOTH rows carry the form's own offset.
    for (const std::uint32_t t : {0U, 400U, 800U, 1200U, 1600U}) {
        const auto phase = phase_at(Origin::Ready, t);

        ASSERT_EQ(copy_top_y(phase) - kUnavailableCopyBaseY, phase.offset_y) << "t = " << t;
        ASSERT_EQ(form_top_y(phase) - scene::bounding_box(phase.form).y, phase.offset_y)
            << "t = " << t;
    }
}

// ⛔ FIRING CONTROL 1 — the rejected 8 px-only path. The old law's offset was a single eased ramp
// 0 -> 8 over the whole entry, so it NEVER went negative and its peak-to-peak was 8. B2's does
// both. This detects a straight restoration of the rejected path.
TEST(ReadyEntryB2Tests, TheRejectedEightPixelOnlyPathIsNotWhatIsDrawn) {
    std::int32_t lowest = 0;
    std::int32_t highest = 0;

    for (std::uint32_t t = 0; t <= kUnavailableSettleMs; ++t) {
        const std::int32_t offset = phase_at(Origin::Ready, t).offset_y;
        lowest = std::min(lowest, offset);
        highest = std::max(highest, offset);
    }

    // The rejected path never lifted: its minimum offset was 0.
    EXPECT_EQ(lowest, -kReadyLiftY) << "no lift — the rejected 8 px-only path has been restored";
    EXPECT_EQ(highest, kUnavailableSettleY);
    EXPECT_EQ(highest - lowest, kUnavailableSettleY + kReadyLiftY)
        << "peak-to-peak travel against the rejected path's " << kUnavailableSettleY;
    EXPECT_GT(highest - lowest, 2 * kUnavailableSettleY);
}

TEST(ReadyEntryB2Tests, TheEntryNeitherJumpsOnItsFirstFrameNorAtThePivot) {
    // No first-frame jump: smoothstep has zero slope at both ends, so the opening step is small.
    EXPECT_EQ(phase_at(Origin::Ready, 0).offset_y, 0);
    EXPECT_EQ(phase_at(Origin::Ready, 1).offset_y, 0);

    // No corner at the pivot either, in either direction.
    for (std::uint32_t t = 1; t <= kUnavailableSettleMs; ++t) {
        const std::int32_t step =
            phase_at(Origin::Ready, t).offset_y - phase_at(Origin::Ready, t - 1).offset_y;
        ASSERT_LE(std::abs(step), 1) << "discontinuity at t = " << t;
    }
}

TEST(ReadyEntryB2Tests, EveryFrameStaysInsideTheContentBox) {
    // The lift raises the crown, so the risk moves to the TOP of the box rather than the bottom.
    for (std::uint32_t t = 0; t <= kUnavailableSettleMs + 500; t += 5) {
        const auto phase = phase_at(Origin::Ready, t);

        ASSERT_LE(deepest_painted(phase.form, phase.offset_y), theme::kCanvasContent.bottom() - 1)
            << "below the content box at t = " << t;
        ASSERT_GE(scene::bounding_box(phase.form).y + phase.offset_y, theme::kCanvasContent.y)
            << "above the content box at t = " << t;
        ASSERT_GT(kUnavailableCopyBaseY + phase.offset_y, theme::kCanvasContent.y)
            << "the copy left the content box at t = " << t;
    }
}

TEST(ReadyEntryB2Tests, TheEntryLengthAndTheRetirementClocksAreUnchanged) {
    // B2 must not lengthen the entry: the general formula would have given 1800 once the copy
    // window moved to the pivot, which is exactly the silent clock change total_ms() prevents.
    EXPECT_EQ(total_ms(Origin::Ready), kUnavailableSettleMs);
    EXPECT_EQ(copy_from_ms(Origin::Ready), kReadyPivotMs);

    // And the retirement tail is untouched — still the central display fade, not a schedule here.
    EXPECT_EQ(kUnavailableOutgoingMs, 2000U);
    EXPECT_TRUE(outgoing_visible(kUnavailableOutgoingMs - 1));
    EXPECT_FALSE(outgoing_visible(kUnavailableOutgoingMs));
}

// ⛔ FIRING CONTROL 5 — no non-Ready origin moved. Every other origin's offset and alpha are the
// general law's at every millisecond, and the general law is restated here independently of the
// implementation so a change to either side is caught.
TEST(ReadyEntryB2Tests, NoNonReadyOriginFrameLawChanged) {
    const auto ease = [](std::int32_t p) {
        p = std::clamp(p, 0, 1000);
        return (p * p * (3000 - (2 * p))) / (1000 * 1000);
    };
    const auto progress = [](const std::uint32_t t, const std::uint32_t from,
                             const std::uint32_t to) -> std::int32_t {
        if (t <= from) {
            return 0;
        }
        if (t >= to) {
            return 1000;
        }
        return static_cast<std::int32_t>(((t - from) * 1000U) / (to - from));
    };

    for (const Origin origin : {Origin::Stopped, Origin::Preparing, Origin::Completed,
                                Origin::Paused, Origin::Printing}) {
        // The general law's own copy window and total, restated.
        const std::uint32_t copy_from = origin_energy(origin) >= scene::kEnergyPreparing
                                            ? kUnavailableFormMs
                                            : kUnavailableSettleMs / 4U;
        ASSERT_EQ(copy_from_ms(origin), copy_from) << label(origin);
        ASSERT_EQ(total_ms(origin), std::max(kUnavailableSettleMs, copy_from + kUnavailableCopyMs))
            << label(origin);

        for (std::uint32_t t = 0; t <= total_ms(origin); t += 5) {
            const auto phase = phase_at(origin, t);

            if (phase.settled) {
                continue;
            }

            const std::int32_t settling = ease(progress(t, 0, kUnavailableSettleMs));
            const std::int32_t resolved =
                ease(progress(t, copy_from, copy_from + kUnavailableCopyMs));

            ASSERT_EQ(phase.offset_y, (kUnavailableSettleY * settling) / 1000)
                << label(origin) << " offset at t = " << t;
            ASSERT_EQ(phase.copy_alpha, static_cast<std::uint8_t>((255 * resolved) / 1000))
                << label(origin) << " alpha at t = " << t;
            ASSERT_GE(phase.offset_y, 0) << label(origin) << " lifted at t = " << t;
        }
    }
}

// ===========================================================================================
// THE WAKE-ORDER SAFEGUARD — scene::EntryClock
// ===========================================================================================

TEST(EntryClockTests, MotionStaysAtZeroWhileArmedAndTheDisplayIsNotVisible) {
    scene::EntryClock clock;
    clock.arm();

    // Many ticks pass with the panel dark. The clock never starts, so the caller renders frame 0.
    for (std::uint32_t now = 1000; now < 5000; now += 16) {
        ASSERT_FALSE(clock.advance(false, now)) << "started while dark at " << now;
        ASSERT_FALSE(clock.started);
        ASSERT_EQ(phase_at(Origin::Ready, 0).offset_y, 0);
        ASSERT_EQ(phase_at(Origin::Ready, 0).copy_alpha, 0);
    }
}

// ⛔ FIRING CONTROL 6 — the old incidental ordering. Before the safeguard the clock was stamped on
// arrival, so an entry published while the panel was dark burned its whole 1600 ms invisibly and the
// first visible frame was the settled one. This asserts the opposite.
TEST(EntryClockTests, TheFirstVisibleFrameIsFrameZeroHoweverLateTheLightArrives) {
    scene::EntryClock clock;
    clock.arm();

    // 4000 ms of darkness — well past the 1600 ms entry, which is what the old ordering would have
    // consumed. Under the safeguard the clock has not started, so nothing has elapsed.
    constexpr std::uint32_t kArrival = 1000;
    constexpr std::uint32_t kLight = kArrival + 4000;

    for (std::uint32_t now = kArrival; now < kLight; now += 16) {
        ASSERT_FALSE(clock.advance(false, now));
    }

    // The light arrives. The clock starts HERE, not at arming.
    ASSERT_TRUE(clock.advance(true, kLight));
    EXPECT_TRUE(clock.started);
    EXPECT_EQ(clock.start_ms, kLight);

    // So the first frame the viewer can see is the entry's first frame, not its last.
    const auto first_visible = phase_at(Origin::Ready, kLight - clock.start_ms);
    EXPECT_EQ(first_visible.offset_y, 0);
    EXPECT_EQ(first_visible.copy_alpha, 0);
    EXPECT_FALSE(first_visible.settled);
}

TEST(EntryClockTests, OnceStartedTheClockRunsFromItsOwnStartAndIgnoresLaterDarkness) {
    scene::EntryClock clock;
    clock.arm();

    ASSERT_TRUE(clock.advance(true, 2000));
    EXPECT_EQ(clock.start_ms, 2000);

    // A later dark tick must not restart or stall it: the motion is already visible to the viewer,
    // and the retirement fade legitimately dims the panel while the composition is still up.
    EXPECT_TRUE(clock.advance(false, 2500));
    EXPECT_EQ(clock.start_ms, 2000);
}

TEST(EntryClockTests, AnUnarmedClockNeverStartsAndClearingDisarmsIt) {
    scene::EntryClock clock;

    EXPECT_FALSE(clock.advance(true, 1234));
    EXPECT_FALSE(clock.started);

    clock.arm();
    EXPECT_TRUE(clock.advance(true, 1234));

    clock.clear();
    EXPECT_FALSE(clock.armed);
    EXPECT_FALSE(clock.started);
    EXPECT_EQ(clock.start_ms, 0U);
    EXPECT_FALSE(clock.advance(true, 9999));
}

TEST(EntryClockTests, ReArmingRestartsTheMotionRatherThanResumingIt) {
    // A new generation — an explicit press from true Off, typically — must begin its entry again.
    scene::EntryClock clock;
    clock.arm();
    ASSERT_TRUE(clock.advance(true, 1000));

    clock.arm();
    EXPECT_FALSE(clock.started);
    EXPECT_EQ(clock.start_ms, 0U);
    ASSERT_TRUE(clock.advance(true, 8000));
    EXPECT_EQ(clock.start_ms, 8000U);
}

// ===========================================================================================
// THE NO-OPERATIONAL-FORM ORIGIN — B2 must NOT reach it
// ===========================================================================================
//
// AcquiringState has no operational form, and until 2026-09-02 the reducer folded it into
// LossOriginForm::Ready — so the accepted Ready-ONLY B2 entry ran for it as well. That was a scope
// leak the owner never ruled on. The origin is now explicit, and its law is the staged tree's
// pre-B2 fallback, preserved exactly.

namespace {

// The staged pre-B2 law, restated here from its own two constants so that a change to either side
// fails rather than silently agreeing. The evidence pack proves the same thing the other way, by
// compiling a dumper against the staged tree export and comparing every millisecond.
[[nodiscard]] std::int32_t staged_ease(std::int32_t p) {
    p = std::clamp(p, 0, 1000);
    return (p * p * (3000 - (2 * p))) / (1000 * 1000);
}

[[nodiscard]] std::int32_t staged_progress(const std::uint32_t t, const std::uint32_t from,
                                           const std::uint32_t to) {
    if (t <= from) {
        return 0;
    }
    if (t >= to) {
        return 1000;
    }
    return static_cast<std::int32_t>(((t - from) * 1000U) / (to - from));
}

} // namespace

TEST(NoOperationalFormOriginTests, ItIsNotTheRealReadyOriginAndKeepsTheStagedPreB2Law) {
    // Its metadata is the staged fallback's, not B2's.
    EXPECT_EQ(copy_from_ms(Origin::NoOperationalForm), kUnavailableSettleMs / 4);
    EXPECT_EQ(total_ms(Origin::NoOperationalForm), kUnavailableSettleMs);
    EXPECT_NE(copy_from_ms(Origin::NoOperationalForm), kReadyPivotMs);

    // Its endpoint energy and form are the resting horizon, through energy_for()'s documented
    // off-axis fallback rather than a second table.
    EXPECT_EQ(origin_energy(Origin::NoOperationalForm), scene::kEnergyReady);
    EXPECT_EQ(subject_state_of(Origin::NoOperationalForm),
              prd::app_model::SubjectState::AcquiringState);
    EXPECT_FALSE(scene::energy_for(subject_state_of(Origin::NoOperationalForm)).has_value());

    // ⛔ AND ITS WHOLE LAW IS THE STAGED ONE, AT EVERY MILLISECOND.
    const std::uint32_t copy_from = kUnavailableSettleMs / 4;

    for (std::uint32_t t = 0; t <= total_ms(Origin::NoOperationalForm); ++t) {
        const auto phase = phase_at(Origin::NoOperationalForm, t);

        if (phase.settled) {
            ASSERT_EQ(phase.offset_y, kUnavailableSettleY) << "t = " << t;
            ASSERT_EQ(phase.copy_alpha, 255) << "t = " << t;
            continue;
        }

        const std::int32_t settling = staged_ease(staged_progress(t, 0, kUnavailableSettleMs));
        const std::int32_t resolved =
            staged_ease(staged_progress(t, copy_from, copy_from + kUnavailableCopyMs));

        ASSERT_EQ(phase.offset_y, (kUnavailableSettleY * settling) / 1000) << "t = " << t;
        ASSERT_EQ(phase.copy_alpha, static_cast<std::uint8_t>((255 * resolved) / 1000))
            << "t = " << t;

        // ⛔ NEVER LIFTS. The 0 -> 8 settlement is one-directional, which is the single clearest
        // discriminator against B2 and the firing control for the leak.
        ASSERT_GE(phase.offset_y, 0) << "the no-form path lifted at t = " << t;
    }
}

TEST(NoOperationalFormOriginTests, OnlyGenuineReadyReceivesB2) {
    // The two paths share their endpoints and differ everywhere between them.
    EXPECT_EQ(phase_at(Origin::Ready, 0).offset_y, phase_at(Origin::NoOperationalForm, 0).offset_y);
    EXPECT_EQ(phase_at(Origin::Ready, kUnavailableSettleMs).offset_y,
              phase_at(Origin::NoOperationalForm, kUnavailableSettleMs).offset_y);

    std::int32_t differing = 0;
    for (std::uint32_t t = 0; t <= kUnavailableSettleMs; ++t) {
        if (phase_at(Origin::Ready, t).offset_y !=
            phase_at(Origin::NoOperationalForm, t).offset_y) {
            ++differing;
        }
    }

    EXPECT_GT(differing, 1000) << "B2 is not distinguishable from the no-form fallback";

    // And B2's signature — the lift, and no copy during it — belongs to Ready alone.
    EXPECT_EQ(phase_at(Origin::Ready, kReadyPivotMs).offset_y, -kReadyLiftY);
    EXPECT_GT(phase_at(Origin::NoOperationalForm, kReadyPivotMs).offset_y, 0);
    EXPECT_EQ(phase_at(Origin::Ready, kReadyPivotMs).copy_alpha, 0);
    EXPECT_GT(phase_at(Origin::NoOperationalForm, kReadyPivotMs).copy_alpha, 0);
}

// ⛔ PRESERVATION OVER ALL SEVEN ORIGINS — the six real form positions plus the no-form path. The
// previous matrix enumerated five and omitted the no-form path entirely, which is how the leak
// reached hardware evidence unnoticed.
TEST(NoOperationalFormOriginTests, PreservationCoversEveryOriginIncludingTheNoFormPath) {
    const std::array<Origin, 7> all{{Origin::Ready, Origin::Stopped, Origin::Preparing,
                                     Origin::Completed, Origin::Paused, Origin::Printing,
                                     Origin::NoOperationalForm}};

    std::set<Origin> seen;
    for (const Origin origin : all) {
        EXPECT_TRUE(seen.insert(origin).second) << label(origin) << " enumerated twice";

        // Every origin ends at the accepted settled frame, whatever its entry.
        const auto settled = phase_at(origin, total_ms(origin));
        EXPECT_TRUE(settled.settled) << label(origin);
        EXPECT_EQ(settled.offset_y, kUnavailableSettleY) << label(origin);
        EXPECT_EQ(settled.copy_alpha, 255) << label(origin);
        EXPECT_EQ(settled.energy, scene::kEnergyReady) << label(origin);
    }

    EXPECT_EQ(seen.size(), 7U);

    // Exactly ONE origin lifts, and it is the real Ready origin.
    std::int32_t lifting = 0;
    for (const Origin origin : all) {
        bool lifts = false;
        for (std::uint32_t t = 0; t <= total_ms(origin); t += 5) {
            if (phase_at(origin, t).offset_y < 0) {
                lifts = true;
            }
        }
        if (lifts) {
            ++lifting;
            EXPECT_EQ(origin, Origin::Ready) << label(origin) << " lifts but must not";
        }
    }

    EXPECT_EQ(lifting, 1) << "exactly one origin carries B2";
}
