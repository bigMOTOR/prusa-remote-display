// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// The operational accent — UI Design Decision 004 §5.1, as ruled by the owner on 2026-09-01.
//
// INTERNAL to the ui component. LVGL-free, so the rule is host-testable, for the reason
// operational_form.hpp already records: this repository has paid for arithmetic that was wrong in
// a way no build check could see. A colour is worse than geometry in that respect — a wrong shape
// is visible in a photograph, and a hue that has drifted a few per cent is not.
//
// §5.1, as it now stands: "There shall be one accent colour, `color.accent`, and it is applied to
// the operational form alone ... The accent does not vary with energy."
//
// --- Why this function takes no energy -------------------------------------
//
// ⛔ BECAUSE ENERGY MUST NOT BE ABLE TO REACH THE ACCENT. Until 2026-09-01 this unit exposed
// accent_level(Energy) and accent_for(Energy): a ramp from a WORKING-BASELINE resting level of 40 %
// up to the full token, which rendered Ready's form at `#623212`. The owner rejected the endpoint
// AND the mechanism.
//
// A function that took an Energy and ignored it would leave the rejected shape available to any
// future edit — one line inside the body brings the ramp back, and nothing outside would change.
// Removing the parameter makes the ruling structural: there is no argument to attenuate by, and no
// level to scale. That is the same discipline theme/tokens.hpp now applies at the token end, where
// the ramp's two endpoints no longer exist.
//
// --- What still expresses energy, and what expresses display power ---------
//
// ENERGY IS SHAPE, NOT COLOUR. §3 gives the form its shape, position and sweep along the axis, and
// §4 gives the composition's achromatic body its own luminance ramp
// (scenes/operational_luminance.hpp). Both are untouched. What is gone is the third expression of
// the same axis, the one that dimmed the accent's own channels.
//
// DISPLAY POWER IS THE BACKLIGHT. UI Behaviour Decision 001 §10 expresses Quiet, Dimmed and Off
// through the panel's duty cycle, under one central policy owner. An RGB attenuation on top of it
// dimmed a resting form twice — once in the pixel and again in the backlight — which is what the
// ruling removes.
//
// THE FORM IS DRAWN AT FULL OPACITY, at every energy, and there is no opacity in this interface.
// A rule that cannot express transparency cannot secretly become one.

namespace prd::ui::scene {

// The accent, as 0xRRGGBB — the form theme/tokens.hpp states §5's palette in.
//
// Always exactly theme::kColorAccent. It is a function rather than a direct use of the token at
// each call site so that §5.1's "applied to the operational form alone" has one named referent
// that a test can hold, and so the rule has one place to be read.
[[nodiscard]] std::uint32_t accent();

} // namespace prd::ui::scene
