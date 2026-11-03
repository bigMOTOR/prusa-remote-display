<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Inter — upstream provenance

The typeface required by Production UI Specification §4. Vendored rather than
fetched at build time, so a build months from now uses the bytes that were
validated on the bench and neither the firmware build nor CI needs network
access.

---

## Upstream

| | |
|---|---|
| Project | Inter — https://github.com/rsms/inter |
| Version | **4.1** (release tag `v4.1`, published 2024-11-16) |
| Release archive | https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip |
| Archive SHA-256 | `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e` |
| Archive size | 33,707,794 bytes |
| Retrieved | 2026-08-06 |

## Files vendored here

Taken from `extras/ttf/` inside the archive. Four static instances, one per
font weight named by Production UI Specification §4 — not the variable font,
and not the 36 files the archive ships.

| File | §4 weight token | SHA-256 |
|---|---|---|
| `Inter-Regular.ttf` | `font.weight.regular` 400 | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |
| `Inter-Medium.ttf` | `font.weight.medium` 500 | `97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872` |
| `Inter-SemiBold.ttf` | `font.weight.semibold` 600 | `78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3` |
| `Inter-Bold.ttf` | `font.weight.bold` 700 | `288316099b1e0a47a4716d159098005eef7c0066921f34e3200393dbdb01947f` |

`LICENSE.txt` is the licence text as shipped in the archive root, copied
unmodified.

### Why the text cuts and not `InterDisplay`

Inter 4 ships a separate optical-size cut (`InterDisplay-*`) intended for large
sizes, which would arguably suit the 36 px primary value. Production UI §4 names
the typeface as **Inter** and distinguishes faces by weight only. Introducing a
second family the specification does not name would be a divergence for a
difference that is not visible at this size on this panel at the 1–2 m viewing
distance §12 specifies. The text cuts are used at every size.

---

## Licence

**SIL Open Font License, Version 1.1.** Full text in `LICENSE.txt`.

Copyright notice as shipped:

> Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)

### Reserved Font Name: none declared

Verified against the vendored `LICENSE.txt`, not assumed. The OFL's Reserved
Font Name mechanism works by naming such fonts **in the copyright statement**
("with Reserved Font Name ..."). Inter 4.1's copyright statement contains no such
clause. The only occurrence of the phrase in the file is the OFL's own
definition of the term in its DEFINITIONS section.

**Consequence:** OFL clause 5 does not restrict the naming of a derivative of
Inter 4.1, so the generated faces may carry the Inter name.

The generated LVGL faces are nonetheless a **Modified Version** under the OFL —
`tnum` is frozen into the character map (see
`firmware/tools/generate-fonts.py`), the glyph set is subsetted, and the outlines
are rasterised to fixed-size bitmaps. The licence's own definition reaches all
three: a Modified Version is any derivative made "by adding to, deleting, or
substituting … any of the components of the Original Version, by changing
formats or by porting the Font Software to a new environment." They are
therefore named for the production role they serve (`prd_font_body_14`, and so
on) rather than presented as the official Inter binary, each generated file
records the derivation and this licence, and this notice and `LICENSE.txt`
accompany them in the repository. That satisfies OFL clause 2 and states the
derivation plainly, independently of clause 5's naming provisions.

### The identifiers those seven files carry

Owner ruling, 2026-08-18, on the licence text and SIL's FAQ. Each generated
face under `firmware/components/ui/src/fonts/prd_font_*.c` carries:

```c
// SPDX-FileCopyrightText: Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
// SPDX-License-Identifier: OFL-1.1-no-RFN
```

Inter's notice, not this project's, and **not** `GPL-3.0-only`. OFL clause 5
requires the Font Software "modified or unmodified, in part or in whole" to be
"distributed entirely under this license, and … not … under any other license",
so a project licence tag on that data would be the one thing the OFL forbids.
`-no-RFN` is the precise identifier because Inter 4.1 declares no Reserved Font
Name, verified above against the vendored licence text.

`firmware/tools/licensing-manifest` classifies these seven paths `foreign` and
`check-licensing.sh` enforces both halves: the OFL identifier must be present,
and this project's copyright line must be absent.

**The firmware as a whole remains GPL-3.0-only.** The OFL and the GPL apply to
different files, and SIL's FAQ 1.3 says where the line falls: "Only the portions
based on the Font Software are required to be released under the OFL." The
fonts are aggregated data with their own licence, retained here in full. The
project-authored wrappers `fonts.cpp` and `fonts.hpp` and the generator
`generate-fonts.py` are original work and stay GPL-3.0-only.

---

## Regenerating

The generated C sources under `firmware/components/ui/src/fonts/` are committed,
so an ordinary build and CI require neither `fontTools`, nor Node, nor network
access. Regenerate only when a font token or the vendored version changes:

```bash
./firmware/tools/generate-fonts.py
```

The tool records its own pinned dependency versions and refuses to run against
different ones, so a regenerated file cannot silently differ from the committed
bytes because a converter moved underneath it.
