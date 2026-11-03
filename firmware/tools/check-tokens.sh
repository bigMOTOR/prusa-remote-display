#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Production UI Specification §2 Token Rules, as a build check.
#
#   "Literal production values are prohibited outside their token definitions."
#   "Every reusable measurement shall reference a production token."
#
# And §14's first four checklist items:
#
#   [ ] No literal spacing values outside Foundation Tokens.
#   [ ] No literal font sizes outside Typography Tokens.
#   [ ] No literal colours outside Colour Tokens.
#   [ ] No literal geometry outside Geometry Tokens.
#
# Those are review promises unless something checks them, and a review promise about
# four numeric classes across a growing scene library is one that will be broken. This
# fails the build instead.
#
# SCOPE: the ui component's own C++ sources, excluding src/theme/ (which is where the
# tokens are defined) and src/fonts/ (machine-generated glyph data, which is nothing
# but numbers). The Milestone 1 diagnostic scene is also excluded and says why below.
#
# Usage:
#     ./firmware/tools/check-tokens.sh
#
# Exits non-zero on the first category with findings, listing every one.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UI_SRC="${REPO_ROOT}/firmware/components/ui/src"

if [[ ! -d "${UI_SRC}" ]]; then
    echo "check-tokens: ${UI_SRC} not found" >&2
    exit 1
fi

# Files that are checked. Everything under src/ except:
#
#   theme/            the token definitions themselves
#   fonts/            generated glyph data
#   validation_scene  the Milestone 1 Phase F diagnostic scene. It predates the token
#                     layer, is compiled only when it replaces the product scene, and
#                     its own header states that it defines no token and is not a
#                     product screen. Holding bench artwork to the product token rules
#                     would mean editing validated bring-up code to satisfy a rule it
#                     is explicitly outside of.
#   diagnostic_overlay
#                     the owner-paced brightness harness's own surface, on exactly the
#                     same ground. It is bench instrumentation: compiled only when the
#                     diagnostic scene replaces the product scenes, so no product image
#                     can contain it. Its raw grey ramp, its patch geometry and its step
#                     labels are measurement apparatus — the ramp exists so a person can
#                     see whether the dark end of the pixel range survives at each
#                     backlight level, and the labels exist so the panel says which step
#                     is showing. They are not product tokens and not product copy, and
#                     tokenising them would put bench values in the product token layer.
#   lvgl_panel        the LVGL/display seam. Its numbers are buffer geometry and
#                     timeouts, not production design values.
#
# Each exclusion names an exact file. No directory is excluded, no class of token is
# switched off, and ui.cpp is NOT exempt: the harness's queue and task boundary stay
# there and stay checked. What moved out is only the drawing.
#
# Collected with a while-read loop rather than mapfile: macOS ships bash 3.2, which
# has no mapfile, and this script has to run identically on the developer machine and
# on the Linux CI container.
FILES=()
while IFS= read -r file; do
    FILES+=("${file}")
done < <(
    find "${UI_SRC}" -type f \( -name '*.cpp' -o -name '*.hpp' \) \
        -not -path "${UI_SRC}/theme/*" \
        -not -path "${UI_SRC}/fonts/*" \
        -not -name 'validation_scene.*' \
        -not -name 'diagnostic_overlay.*' \
        -not -name 'lvgl_panel.*' \
        | sort
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "check-tokens: no files to check" >&2
    exit 1
fi

failures=0

report() {
    local category="$1"
    local matches="$2"

    if [[ -n "${matches}" ]]; then
        echo ""
        echo "check-tokens: ${category}"
        while IFS= read -r line; do
            echo "  ${line#"${REPO_ROOT}/"}"
        done <<< "${matches}"
        failures=$((failures + 1))
    fi
}

# --- Literal colours -------------------------------------------------------
#
# Any six-digit hex constant. §5's palette is nine colours and they all live in
# tokens.hpp; a 0xRRGGBB anywhere else is a colour that no token names.
colours="$(grep -nE '0[xX][0-9a-fA-F]{6}\b' "${FILES[@]}" || true)"
report "literal colour outside src/theme/ (§5, §14)" "${colours}"

# --- LVGL colour and opacity constructors ----------------------------------
#
# lv_color_hex() and the LV_OPA_* macros are how a literal colour or opacity reaches
# LVGL. theme.cpp is the only place either belongs: §5's rules are "Components
# reference semantic colour tokens only" and "Literal opacity values are prohibited".
constructors="$(grep -nE 'lv_color_hex|lv_color_make|LV_OPA_[A-Z0-9_]+' "${FILES[@]}" || true)"
report "colour or opacity constructed outside src/theme/ (§5)" "${constructors}"

# --- Literal font selection ------------------------------------------------
#
# A face must be chosen by §4 text role through fonts::for_role(), never by naming a
# font symbol. Naming one picks a size and a weight independently, which is how
# screen-specific typography gets in (§2: "Screen-specific styling is prohibited").
#
# A face symbol ends in its size — lv_font_montserrat_14, prd_font_body_14 — so the
# trailing digit is what distinguishes a face from the font API. lv_font_t and
# lv_font_get_glyph_width() are a type and a metrics query, not a selection, and both
# are legitimate: measuring the digit advance is how the percentage field is sized.
fonts="$(grep -nE '\b(lv|prd)_font_[a-z_]*[0-9]+\b' "${FILES[@]}" || true)"
report "font named directly instead of by text role (§4, §2)" "${fonts}"

# --- Literal geometry ------------------------------------------------------
#
# Bare integers of two or more digits passed to an LVGL geometry or style setter.
# One-digit values are not flagged: LV_PART_*, an index, a count and a boolean are all
# legitimately small, and flagging them would produce noise that gets suppressed rather
# than fixed. Two or more digits in a position or size is a production measurement.
geometry="$(grep -nE 'lv_obj_set_(pos|size|width|height|x|y)\([^)]*[^a-zA-Z_0-9]([0-9]{2,})\b' "${FILES[@]}" || true)"
report "literal geometry passed to a layout call (§3, §6, §14)" "${geometry}"

# --- Literal style values --------------------------------------------------
#
# The same rule for style setters: arc width, radius, padding, spacing. A token name
# reads as an identifier and passes; a number does not.
styles="$(grep -nE 'lv_obj_set_style_[a-z_]*(width|radius|pad_[a-z]+|space|size)\([^,]*,\s*[0-9]{2,}\b' "${FILES[@]}" || true)"
report "literal style measurement (§3, §6, §14)" "${styles}"

# --- User-visible string literals ------------------------------------------
#
# Product scenes and shared components must not own English. Every user-visible string
# comes from the text resource boundary in src/text/, which is the only place in the
# firmware where a word appears (see the localization-readiness note in text/text.hpp).
#
# SCOPED TO THE ACTUAL USER-VISIBLE SURFACE, not to string literals in general. What
# reaches a reader is text handed to LVGL, so the rule is about lv_label_set_text* call
# sites and about prose cached in a constant for one. Diagnostics keep their messages,
# internal object names and log tags are untouched, and a printf-style format string is
# not prose — three exclusions that fall out of the rule rather than being bolted on.
#
# "Two or more consecutive letters" is what distinguishes a word from a format
# specifier: "%.*s", "%u:%02u", "" and "%" all pass; "Printing" and "Nozzle" do not.
SCENE_FILES=()
while IFS= read -r file; do
    SCENE_FILES+=("${file}")
done < <(
    find "${UI_SRC}/scenes" "${UI_SRC}/components" -type f \( -name '*.cpp' -o -name '*.hpp' \) \
        2>/dev/null | sort
)

if [[ ${#SCENE_FILES[@]} -gt 0 ]]; then
    # A literal handed straight to a label.
    label_text="$(grep -nE 'lv_label_set_text(_fmt)?\([^,)]*,\s*"[^"]*[A-Za-z]{2,}' "${SCENE_FILES[@]}" || true)"
    report "user-visible literal passed to a label; use text::lookup(TextId) instead" "${label_text}"

    # Prose cached in a constant, which is how a scene would hold a title.
    cached_text="$(grep -nE 'constexpr[^=]*\b(char|string_view)\b[^=]*=\s*"[^"]*[A-Za-z]{2,}' "${SCENE_FILES[@]}" || true)"
    report "user-visible literal cached in a constant; add a TextId instead" "${cached_text}"
fi

if [[ ${failures} -ne 0 ]]; then
    echo ""
    echo "check-tokens: FAILED — ${failures} categor$([[ ${failures} -eq 1 ]] && echo y || echo ies) with findings"
    echo "check-tokens: production values belong in firmware/components/ui/src/theme/"
    echo "check-tokens: user-visible strings belong in firmware/components/ui/src/text/"
    exit 1
fi

echo "check-tokens: clean over ${#FILES[@]} files in components/ui/src"
