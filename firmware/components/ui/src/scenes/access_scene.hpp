// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/presentation.hpp"
#include "components/message_form.hpp"
#include "scenes/unavailable_motion.hpp"

namespace prd::ui::scene {

// Connection acquisition and positive access failures, never lost observation.
// No operational form, progress, temperature, credential or fabricated identity.
//
// ⬅️ THE COMPOSITION MOVED OUT, THE BEHAVIOUR STAYED — 2026-09-12. What this scene draws is
// component::MessageForm, the one full-screen message composition the whole product now shares.
// What it still owns is everything that is ITS behaviour and nobody else's: the accepted
// Connecting dot loop with its Off pause, the two classified conditions, and the clocks that
// carry the entry and the printer-outline-to-warning morph. Acquisition and a classified failure
// are the two endpoints of one object, which is why the morph lives on this scene rather than in
// the component: nothing else has two endpoints to travel between.
class AccessScene {
  public:
    [[nodiscard]] bool build(lv_obj_t* parent);
    void set_visible(bool visible);
    void apply(const app_model::Presentation& presentation);
    [[nodiscard]] bool step(bool panel_visible, std::uint32_t now);

  private:
    void paint(std::int32_t level);
    bool step_dots(bool panel_visible, std::uint32_t now);
    std::uint32_t dots_at_{0};
    std::uint8_t dots_{1};
    bool dots_anchored_{false};
    component::MessageForm form_{};
    // Both HOLD while the panel is Off rather than only refusing to start — see HoldClock. The
    // dot loop keeps its own separate, accepted pause: it re-anchors on wake instead of holding,
    // because it is an unbounded cycle with no end to arrive at.
    HoldClock clock_{};
    HoldClock icon_clock_{};
    double icon_mix_{0};
    double icon_from_{0};
    double icon_target_{0};
    bool icon_running_{false};
    app_model::AccessCondition condition_{app_model::AccessCondition::None};
    bool visible_{false};
    bool applied_{false};
    bool running_{false};
    std::int32_t level_{-1};
};

} // namespace prd::ui::scene
