// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// LVGL headers are not clean under the project warning policy. Suppressed around
// the include only (per architecture spec §12.3); see lvgl_panel.hpp for why the
// suppression is scoped this tightly rather than applied to the whole TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "lvgl.h"
#pragma GCC diagnostic pop

#include "theme/layout.hpp"
#include "theme/tokens.hpp"

// The binding from Production UI tokens to LVGL types — Architecture Spec §5.12
// `ui_theme`.
//
// INTERNAL HEADER of the ui component; it names LVGL types.
//
// THE ONLY FILE THAT CONVERTS A TOKEN INTO AN LVGL VALUE. tokens.hpp holds the
// specification's numbers in the specification's own units and knows nothing about
// LVGL; components and scenes name tokens and never construct an lv_color_t or pick
// a font pointer. That leaves exactly one place where a colour becomes a colour,
// which is what makes §2's "One canonical token source" checkable rather than
// aspirational.

namespace prd::ui::theme {

// --- §5 Colour -------------------------------------------------------------

// `Colour` itself lives in tokens.hpp, which carries no LVGL include — it is a semantic
// name, not an LVGL type, and keeping it there lets pure code (the per-state content table,
// for one) name a colour without pulling in the renderer.
[[nodiscard]] lv_color_t colour(Colour token);

// A colour already resolved to 0xRRGGBB, converted to LVGL's type.
//
// Naming a token is the normal way to reach a colour and stays the rule. This exists for values
// that arrive already RESOLVED to 0xRRGGBB rather than as a token name: scene::accent() answers in
// that form, and ui::OperationalFormReport and scene::UnavailablePhase both carry the colour the
// renderer actually handed to LVGL. This is where such a value becomes an lv_color_t — so §5's
// "the only place a colour becomes a colour" still holds exactly.
//
// ⬅️ IT IS NO LONGER A CONTINUUM. Until 2026-09-01 this existed because §5.1 required the accent to
// vary continuously with energy, which no token can state. That ramp is rejected; the accent is one
// token now, and what still passes through here is a resolved value, not an interpolated one.
//
// IT OPENS NO HOLE IN THE TOKEN DISCIPLINE. tools/check-tokens.sh rejects any six-digit hex
// literal outside src/theme/, so a caller cannot reach this with a colour it invented; the only
// values that can arrive here are ones derived from the palette.
[[nodiscard]] lv_color_t colour_from_rgb(std::uint32_t rgb);

// §5's opacity scale, converted to LVGL's 0...255. The percentages stay in
// tokens.hpp so they can be read against the document; the conversion happens here
// once rather than at each use site.
enum class Opacity {
    Full,
    High,
    Medium,
    Low,
    Disabled,

    // Fully transparent. Not one of §5's five steps — §5's scale describes how visible
    // something is, and this describes something that is not drawn. It exists because a part
    // of a widget sometimes has nothing to show: the progress indicator in the horizon form
    // carries no progress, and a zero-extent arc with a round cap still paints a dot.
    //
    // Kept in the token layer rather than reaching for LV_OPA_TRANSP at a use site, so §5's
    // rule that literal opacity values are prohibited holds without an exception.
    Transparent,
};

[[nodiscard]] lv_opa_t opacity(Opacity token);

// --- §4 Typography ---------------------------------------------------------

// Applies a §4 text role and a §5 colour to a label in one call: font, colour,
// opacity, line height and tracking together. One call rather than five, because a
// label that received four of the five would be a screen-specific style that looked
// almost right.
void apply_text_style(lv_obj_t* label, TextRole role, Colour token, Opacity alpha = Opacity::Full);

// Re-applies a label's §5 opacity scaled by a 0...255 body level — UI Design Decision 004 §4's
// "the composition recedes as one body as energy falls".
//
// THE TOKEN IS THE ELEMENT'S OWN, NOT A NEW ONE. The caller passes the same Opacity the label
// was built with, and this multiplies it by the level. That is what makes the recession one
// body: every participating element is scaled by the same factor, so §5's internal hierarchy
// survives intact instead of being flattened toward a single value.
//
// Opacity alone. Font, colour, line height and tracking are §4 and §5 properties of the role and
// are constant across all six states (§13.1) — this must not disturb them, so it sets one style
// property and nothing else.
void apply_text_level(lv_obj_t* label, Opacity token, std::int32_t level);

// --- Geometry helpers ------------------------------------------------------

// Places an object at a token-derived rectangle. Takes the Rect rather than four
// integers so a caller cannot transpose width and height, and so every placement in
// the codebase traces back to a named region in layout.hpp.
void place(lv_obj_t* object, const Rect& rect);

// Creates a child with LVGL's default theme styling removed.
//
// The default theme applies radii, borders, background opacity and padding that no
// token in §3 to §6 specifies. Removing it means every pixel drawn is one this
// codebase asked for, which is also what makes the token check meaningful: an
// unremoved theme would put untokened geometry on screen that no source file
// mentions.
[[nodiscard]] lv_obj_t* bare_object(lv_obj_t* parent);
[[nodiscard]] lv_obj_t* bare_label(lv_obj_t* parent);

} // namespace prd::ui::theme
