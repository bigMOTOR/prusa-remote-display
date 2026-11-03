<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0009: The warning colour and the recovery heading, decided on glass

## Status

Accepted

## Date

2026-08-22

## Context

Milestone 3.11's validation checkpoint reserves one item for the physical panel: T20, "the three new
presentations on the physical panel, at 1–2 m: legibility of the recovery instruction, the reset
question, and `color.warning` on real glass." Everything else in the milestone was settled
deterministically; these three are properties of glass and eyes.

Two accepted sources bear on them. Production UI Specification v1.0 §5 pins `color.warning` to
`#F2B84B`, and §4 fixes the type scale. §8 pins the Status Message to Body Typography but leaves the
State Header's role open — "Uses Typography Tokens only". Product Brief v.0.2 asks for "comfortable
readability from approximately 1–2 metres".

Hardware validation exists to test accepted design decisions against the real product, and on this
panel two of them did not survive contact.

**The warning colour did not read as a warning.** On the bench `#F2B84B` appeared as a bright
yellow with a lime cast rather than amber, and the owner questioned whether it belonged to the
product's visual language at all.

**The recovery heading was the smallest text in the product.** Both its lines rendered at Body,
because `provisioning_scene.cpp` applied Body to the shared title object for every phase. Recovery
is entered *automatically* — ADR-0008 decision 3 — so unlike the reset confirmation the reader is
not necessarily within arm's reach when it appears.

## Decision

**1. The colour path was audited before the token was blamed, and it is correct.** `#F2B84B`
quantises to RGB565 `0xF5C9` — RGB(246,186,74) — and a host render of the real scene showed the
framebuffer carrying exactly that, with the background at `0x1082`. The Wi-Fi-style pitfalls were
each checked: byte order is handled deliberately by `lv_draw_sw_rgb565_swap()` against the panel's
big-endian COLMOD/RAMCTRL path; colour order, inversion and the Phase C1 vendor gamma table are the
validated M1 baseline. **No byte order, RGB/BGR setting, panel tuning or gamma value was changed.**
The token was rendered faithfully and was simply wrong for this glass.

**2. The photographic blue cast was a camera artifact, and was controlled rather than assumed.**
Numerical sampling of the bench photographs put the reset screen's background at RGB(17,49,108) —
strongly blue — against RGB(27,30,42) for the recovery screen in the same firmware. Since a
pipeline fault would corrupt both identically, the two readings could not both be the panel. The
owner resolved it by direct observation: **the background is black and the neutral reference patch
is grey.** The blue is the camera and the viewing angle. This matters because an amber judged
against a blue-shifted ground is pushed toward lime by simultaneous contrast, which would have made
the colour verdict unsafe.

**3. `color.warning` becomes `#ED9A2E`.** Chosen on the panel from four neutrally-labelled
candidates (A–D) rendered against the production background, with `color.accent` shown beside them
as a family reference, at 20 px text and as solid swatches. The current token read lime; the next
step was still too yellow; the warmest candidate was rejected *because* it sat too close to
`color.accent` `#F77F2F` and risked reading as an inaccurate reproduction of the accent rather than
as a separate semantic. `#ED9A2E` reads as a deliberate warm amber: hue 34.2° against accent's
24.5°, contrast **8.45:1** on `color.background`, **ΔE 19.3** from accent, ≥50 from error and
success.

**4. The recovery heading becomes `StateTitle` at 20 px; the instruction stays Body at 14 px.**
§8's State Header role is unpinned, so this is a choice inside the accepted source rather than a
divergence from it. The Status Message remains Body, which §8 does pin. The owner confirms 20 px is
readable at approximately 1 m and not comfortably readable at 2 m, and accepts that: the product is
a compact desktop accessory, not a room-scale information panel, and the user must approach it to
perform the recovery action.

**5. The recovery title copy becomes "Settings cannot be used".** The previous "Saved settings
cannot be used" needed ~297 px at 20 px against exactly 296 px of `canvas.content`, wrapping the
single word "used" onto a second line and tripping the scene's overflow guard. "Saved" is redundant
in this context, the meaning is preserved, and the shorter copy holds the approved 20 px on one line
— measured at **233 px, 63 px spare**. Shortening the copy was preferred over accepting an orphan
line or dropping the heading to 16 px.

**6. The layout and its guard were remeasured against the approved composition.** The instruction's
baseline is now one `line.title` plus the region gap below the heading rather than the session row's
Body budget, and the runtime overflow guard's budget is `line.title + 3 × line.body` instead of
`4 × line.body`. The three-Body-line instruction budget is unchanged. Left alone, that guard would
have warned on every recovery draw, and a guard that always warns is one nobody reads.

## Consequences

- The warning colour now reads as its own semantic on the delivered hardware, which is what §8's
  Attention Indicator depends on.
- **Production UI Specification v1.0 §5 and the implementation are now diverged.** The specification
  says `#F2B84B`; the firmware says `#ED9A2E`. This is recorded, not silent: **Production UI
  Specification v1.1 must update §5's warning token.** No historical accepted source was rewritten.
- The recovery heading is legible at the distance the recovery action is actually performed from,
  and the composition costs one more line of vertical space, which it had.
- User-facing copy is now pinned by a test. Any future change to either recovery string fails that
  test, which is deliberate: the width budget is a proxy and a copy change must force a real
  re-measurement.
- **The colour decision is provisional in one respect.** It was judged on a bare module with no
  enclosure and no cover glass. Both change perceived contrast and hue.

## Alternatives Considered

**Compensate in the driver — gamma, colour order or a channel trim.** Rejected on evidence: the
audit showed the data path delivering the correct value, so any compensation would have been a
correction applied to something that was not wrong, and it would have shifted every other token
with it.

**Filter or special-case the warning colour in the scene.** Rejected: it puts a literal outside the
token definitions, which `check-tokens.sh` exists to prevent, and it hides a palette decision inside
one composition.

**Keep `#F2B84B` and accept the appearance.** Rejected by direct observation. The token's purpose is
to mark attention; one that reads as lime on the only panel the product ships with does not.

**The warmest candidate, closest to accent.** Rejected on the reasoning in decision 3: attractive in
isolation, but semantic separation from the accent is the property that makes a warning legible as a
warning.

**Accept a two-line recovery heading.** Rejected: an orphaned "used" on a 2.8-inch panel is poor
typography, and the existing composition comments record an earlier fight to avoid exactly that.

**Drop the heading to Section (16 px).** Rejected: single-line but smaller than the size the owner
approved after seeing 20 px on glass.

## Validation / Follow-up

Deterministic: the host suite covers the token's value, its ΔE separation from accent, error and
success, its contrast on the production background, the approved copy for both recovery strings, and
a width budget that rejects the copy which actually wrapped. Compile-time assertions describe the
approved composition exactly — one `line.title`, the region gap, three Body lines, inside the canvas
— and a negative control confirms that restoring the Body-based budget fails the build. The real
face is measured twice more: by the host LVGL renderer, and by the scene's own runtime guard on
every recovery draw.

On glass: the integrated compositions require physical confirmation — heading on one line at 20 px,
unclipped; instruction complete and not overlapping; the reset warning reading as intentional amber;
the setup composition unchanged. **T20 is not closed until that check passes.**

**Open and owed: revalidate `color.warning` against the final enclosure and cover glass.** The value
in this decision was chosen without them.
