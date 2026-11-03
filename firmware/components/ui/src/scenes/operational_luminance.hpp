// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "scenes/operational_energy.hpp"

// The composition's luminance — UI Design Decision 004 §4.
//
// INTERNAL to the ui component. LVGL-free, so the ramp is host-testable, for the same reason
// operational_form.hpp and operational_accent.hpp are: arithmetic that decides what appears on
// the panel is tested where it can be asked directly.
//
// §4 in full: "The composition recedes as one body as energy falls. In every state, luminance is
// floored by legibility of primary operational information at approximately one to two metres.
// Low energy shall never mean difficult to read."
//
// This is divergence OF-7. Until Phase F no luminance varied by state at all, so Ready and
// Printing rendered their text at identical weight and the only thing separating them was the
// form. §13.2 lists "the luminance of the composition as one body" among the things that VARY
// across the six states; it was the one entry not implemented.
//
// --- As one body -----------------------------------------------------------
//
// ONE SCALAR, APPLIED TO EVERY PARTICIPATING ELEMENT. The composition's internal hierarchy is
// already set by §4's text roles and §5's opacity steps — the hero numeral above the state word,
// a telemetry value above its label. This level multiplies all of them by the same factor, so
// the hierarchy is preserved exactly and what changes is the whole body's presence. A per-role
// curve would be six compositions that happen to share a layout, which is what §1 forbids.
//
// --- What does not recede --------------------------------------------------
//
// THE OPERATIONAL FORM. §5.1 gives it one accent, `color.accent`
// (scenes/operational_accent.hpp), drawn at full opacity at every energy. This level is not
// applied to it: §4 governs the composition's achromatic body, and the form is not part of that
// body. Applying it here would make this ramp the one thing attenuating a resting form — which is
// the shape of the mechanism the owner rejected on 2026-09-01, arriving through a different door.
//
// THE STALE INDICATOR — owner ruling, 2026-08-08, and §10 behind it. Confidence is orthogonal to
// energy and "states itself explicitly"; a marker that said the machine cannot currently be
// observed, dimmed because the machine is carrying no job, would be energy speaking about
// observability. It keeps its own luminance.
//
// TELEMETRY DOES RECEDE — owner ruling, same date. §9's "it recedes with the composition, but
// never on the job's schedule" is read as governing presence and content, which §7 already
// rules, rather than luminance; the first clause is explicit permission to include it here.
//
// --- Why alpha is the right instrument here --------------------------------
//
// §4 governs achromatic text, where there is no saturation to lose: `color.primary` composited
// against `color.background` stays neutral at every level, because both are neutral. Luminance is
// precisely what alpha over a fixed dark field controls, so here it is the right instrument.
//
// ⬅️ REJECTED HISTORY, kept because it explains why this ramp is scoped as it is. A parallel ramp
// once attenuated the ACCENT, and it deliberately avoided alpha: compositing a saturated orange
// against the dark field changes its hue character — on the panel it desaturates toward a pale tan.
// The owner rejected that ramp outright on 2026-09-01, by either mechanism. Nothing attenuates the
// accent now, and this ramp's scope — achromatic text and telemetry, never the form — is unchanged
// by that ruling.

namespace prd::ui::scene {

// The composition's luminance at an energy, as a 0...255 multiplier on each element's own
// opacity: theme::kBodyLevelRest at kEnergyMin, rising to theme::kBodyLevelFull at kEnergyMax.
// Energies outside the axis are clamped to it.
//
// At kEnergyMax this is exactly theme::kBodyLevelFull, so Printing renders as it did before
// Phase F and remains the control any change is judged against.
[[nodiscard]] std::int32_t body_level(Energy energy);

} // namespace prd::ui::scene
