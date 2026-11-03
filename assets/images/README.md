<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Documentation images

Copyright 2026 Mykola Fiantsev. The original photographs and UI illustrations
in this folder are documentation assets licensed under
[CC-BY-4.0](../../LICENSE-documentation), not designated logos or brand assets.

## Photograph

`display-hero.png` is a **1600 × 900** exact rectangular crop from Mykola
Fiantsev's original photograph version `display-hero-4309`
(1600 × 1200, source SHA-256
`ac877b7c961518eb75ecf6da923e3b0bb19ef8c25ee94eca2048774e78a19b42`).
The crop rectangle is `(0, 150)–(1600, 1050)`, right and bottom exclusive.
JPEG decoding, crop and lossless PNG export are the only operations; the source
ICC profile is preserved. No retouching, screen replacement, text, panel,
compositing or resizing is applied. The complete device and its visible surface
marks remain in the photograph. On 3 November 2026, Mykola Fiantsev confirmed
that the assembled and photographed enclosure is the current version without
the two printed nut keepers. Only those keepers were removed from the print
plate; the remaining geometry is unchanged. This is the project author's
assembly observation, not a new coordinator physical test or a measurement
of fit tolerances, support-removal forces or runtime behavior.

- `printables-cover.png` — full original photo, 1600 × 1200, without added text;
  unchanged SHA-256 `2765b629fa29884b27b59f472cc34513320555ec0223f159862d9d61f51a6479`.
- `display-social.png` — social preview, 1280 × 640; unchanged SHA-256
  `6300a23d5d4e137638bd361d98c84bbf9c501e39b2e9fbc321598f902f277558`.

The README uses the same photo in both colour themes. A future photograph
replacement should be a separate coherent update with recorded provenance.
Adding these files does not publish a Printables listing or change repository
social-preview settings.

## Software-rendered UI examples

The three 320 × 240 PNGs are exact RGB565-to-RGB888 conversions of saved
host-renderer frames dated **4 October 2026**. They show simulated printer
conditions drawn by the project's LVGL UI, not photographs of the LCD or
evidence of a real printer reaching those states. No overlays or composite
images are used.

The source corpus is `firmware/build/controller-tests-1/controller-frames/`.
The same three frame bytes also occur in the second allocator's corpus.

| Documentation image | Saved renderer frame | Source frame SHA-256 |
| --- | --- | --- |
| `ui-paused.png` | `Paused.rgb565` | `d743b5261f36ba5267ee5f1a94a31bac274725819da6abe891209f2d66616e83` |
| `ui-completed.png` | `Completed.rgb565` | `73975459cb1c7ce7efb9ab51ad8cc19f2f208038e0f02c075086a92d29bbd7f9` |
| `ui-error.png` | `Error-Printing-entry-1120.rgb565` | `c0347f184693c751fd662d0ab8cd19b674914cfd69bc98099467e84cca9cc611` |

Conversion uses [render-frame-to-png.py](../../firmware/tools/render-frame-to-png.py),
320 × 240, little-endian RGB565, preserving every decoded pixel. The renderer
source is [controller_motion.cpp](../../firmware/test/render/controller_motion.cpp);
`Paused` and `Completed` are settled operational states, while `Printer error`
is the settled 1120 ms error entry with the last printing context retained.

The 273 declared renderer input files were compared between repository
snapshot `388036f4eea228f0cd25ed4706ff6ebbc5d65f7e` (4 October) and the
documentation baseline `d3e36fcf51516741ef448f5a53a784ed99f7a383`. They match
except for one added header helper, `recession_timings_for`.
The original binary-to-source build receipt is unavailable. These saved examples
are not a new render run or a full current-renderer validation claim.

The UI uses the project's Inter glyph assets; upstream font terms remain in
[third-party notices](../../THIRD_PARTY_NOTICES.md#inter-41). Physical panel
colour, brightness, motion cadence and real-printer behavior require separate
checks.
