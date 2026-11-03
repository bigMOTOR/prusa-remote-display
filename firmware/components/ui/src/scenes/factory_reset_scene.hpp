// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/presentation.hpp"
#include "components/message_form.hpp"
#include "scenes/unavailable_motion.hpp"
#include "theme/theme.hpp"

// Production UI Specification §9's Factory Reset screen — Milestone 3.10.
//
// INTERNAL HEADER of the ui component.
//
// §9: "Factory Reset. Visible: Reset confirmation, Reset progress. Hidden: Operational telemetry."
// Two presentations, one composition, because they are the two halves of one operation.
//
// ⛔ WHY THIS EXISTS, AND WHY IT IS NOT OPTIONAL. Architecture Spec §5.6 requires factory reset to
// be requested "with an ON-SCREEN confirmation sequence". Before this scene the confirmation had no
// composition at all: the presentation resolved to no scene and the panel showed the controller's
// diagnostic annotation. That was harmless only for as long as nothing could complete a
// confirmation — and Milestone 3.10 makes a second deliberate very-long press erase the device. A
// destructive act one press away from a diagnostic string is the defect this scene closes.
//
// ⬅️ ITS OWN SCENE, NOT A BRANCH OF THE PROVISIONING ONE. §9 gives Factory Reset its own screen
// entry, and the provisional provisioning composition is frozen by owner rulings R3 and D11 —
// growing it to carry an unrelated screen would breach both. This is the pattern the scene layer
// already uses: Scene::Operational carries §9's six operational screens as one composition, and
// Scene::Provisioning carries the provisioning phases. One scene per §9 screen family, and no new
// architectural concept.
//
// ⬅️ FULL CANVAS RATHER THAN A TRANSLUCENT OVERLAY. UIAD 001 §9 calls the confirmation a modal over
// the resolved background state, and §12.5 makes it the presentation that wins; the background
// state continues to EXIST, which it does — the projection still reports the subject beneath, and
// the store still holds it. Compositing it visually would need the transient-overlay layer
// Architecture §17.3 sketches and the controller does not have, which would be a new architectural
// concept for a destructive prompt that is better off filling the screen: a confirmation that could
// be mistaken for a passing indicator is the wrong kind of quiet.
//
// ⛔ NEVER COLOUR ALONE. The Product Brief forbids communicating status through colour alone, so the
// confirmation's weight comes from the §4 size step — a 24 px question over a 20 px instruction —
// and from words that name both actions. The Warning token reinforces; it does not carry.
//
// ⬅️ THE QUESTION IS NOW THE SHARED MESSAGE COMPOSITION — 2026-09-12. It was this scene's own
// arrangement: a StateTitle question in Warning over a Body instruction in Secondary, a third
// variant of the same idea after the access errors and the provisioning statements. The owner's
// cross-screen decision makes all three one, and moves the semantic colour off the title and onto
// the symbol: Warning because something is waiting on the reader, not because something failed.
// The distinction from a failure is carried by the words, which is where §12's colour-independence
// rule wanted it in the first place.
//
// ⛔ THE RUNNING HALF'S TYPOGRAPHY AND GEOMETRY ARE UNCHANGED AND MUST STAY THAT WAY. The erase's
// neutral endpoint — one StateTitle line in Primary, at its accepted position, left-aligned at the
// content inset, with no instruction — was outside the 2026-09-12 composition work and remains
// outside it. What that work added was the transition INTO it: the running line enters on the
// shared 1100 ms clock, so the destructive act is not a silent substitution of one screen for
// another.
//
// ⬅️ ONE LATER, SEPARATELY AUTHORISED EXTENSION, AND IT IS THE TRAILING ELLIPSIS ONLY. The owner's
// ruling after physical review adds the Connecting dot cycle to this title's last three characters.
// Face, size, weight, colour, origin, row and measure are all the accepted ones; the words do not
// move, because the label is left-aligned and only characters to their right change. Everything
// this paragraph says about the endpoint therefore still holds — what it may no longer be read to
// say is that the title's TEXT is fixed.
//
// ⛔ AND THE ANSWERED QUESTION DOES NOT LINGER TO PROVIDE THAT TRANSITION. "A short press cancels"
// stops being true the instant the erase starts. It is retired on the same tick as the operation
// change, and only the valid half is animated — root finding R2, 2026-09-12.
//
// ⛔ NO PROGRESS BAR AND NO COUNTDOWN. §10 forbids representing progress beyond reported values, and
// the erase is six key operations that complete in milliseconds — a bar would imply a duration that
// does not exist. §10 also reserves the operational Progress Ring, explicitly for the analogous
// firmware-update case. A ticking countdown of the confirmation window was considered and rejected:
// UI Behaviour Decision 001 keeps the product from manufacturing urgency, and the window expiring
// is a SAFE outcome the user does not need to race.
//
// ⬅️ AND THE RUNNING TITLE'S DOTS ARE NOT A COUNTDOWN EITHER — owner ruling after physical review,
// 2026-09-12. The line cycles `Erasing settings.`, `..`, `...` every 500 ms. That is ACTIVITY, on
// exactly the terms Production UI §9 already grants Connecting: it represents nothing the firmware
// measures, claims no duration, and its position in the cycle carries no information — so a reader
// cannot read progress out of it. The paragraph above still holds in full; what changed is that a
// screen which is genuinely doing something no longer looks frozen while it does it.
//
// ⛔ THE EXTENSION IS TO THIS STATE AND NO OTHER. §9's refinement calls the loop a Connecting-only
// exception and says so twice; this ruling names FactoryResetting as the second and last case. It
// is not authority for an ambient loop on any other settled screen.
//
// ⬅️ THE ENDPOINT IS OTHERWISE UNTOUCHED. Face, colour, origin and row are the ones the physical
// review accepted; only the trailing dots change, and the label is left-aligned so the words do
// not move. The 1100 ms entry, the immediate retirement of the answered question and the reset
// lifecycle itself are all unchanged.

namespace prd::ui::scene {

class FactoryResetScene {
  public:
    // Builds the composition under `parent`. Returns false if any LVGL allocation failed.
    // Render task only.
    [[nodiscard]] bool build(lv_obj_t* parent);

    // Applies a presentation. Only the modal surface and the device operation are read.
    void apply(const app_model::Presentation& presentation);

    void set_visible(bool visible);

    // One render tick: the entry ramp, and the running half's activity cadence.
    [[nodiscard]] bool step(bool panel_visible, std::uint32_t now);

    [[nodiscard]] bool built() const {
        return root_ != nullptr;
    }

  private:
    void paint(std::int32_t level);

    // The running title at its current dot count, and the 500 ms cycle that advances it.
    void set_running_title();
    [[nodiscard]] bool step_dots(bool panel_visible, std::uint32_t now);

    lv_obj_t* root_{nullptr};

    // The running line. The question is form_; these are the two halves of one operation and
    // exactly one of them is ever legible.
    lv_obj_t* title_{nullptr};
    component::MessageForm form_;

    // Holds while the panel is Off rather than only refusing to start — see HoldClock.
    HoldClock clock_{};

    bool running_{false};
    bool applied_{false};
    bool progress_{false};
    bool visible_{false};
    std::int32_t level_{-1};

    // The activity cadence. `dots_anchored_` is dropped whenever the panel cannot be seen, which
    // is what makes the pause a pause rather than a catch-up.
    std::uint32_t dots_at_{0};
    std::uint8_t dots_{1};
    bool dots_anchored_{false};
};

} // namespace prd::ui::scene
