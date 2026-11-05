<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Guide media

## Enclosure illustrations

Copyright 2026 Mykola Fiantsev. Original documentation illustrations: CC-BY-4.0.
These two existing PNGs were copied without modification from the saved
2 November 2026 clean photo-v2 package. They are CAD concept views, not photographs,
optical simulations, verified insertion paths or evidence of physical fit.
The exploded view omits electronics and fasteners; the cleanup view separates
only the three temporary pads for identification. Follow the assembly text for
movement and preserve all functional button/cassette parts.

Original renderer: CadQuery 2.6.1, tessellation 0.10 mm / 0.16 rad,
orthographic software Z-buffer. No geometry edits. The source commit recorded
by the render receipt is `e1d2dc8c45667b51196cfbeebfb7e5d17290634e`.
All six recorded STEP inputs were byte-compared with the current documentation
base `98028d38b06f7d9f4e4041b37ab2e8a97c13fe33` and match their recorded hashes.
The clean images contain no embedded labels; geometry is unchanged.

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| `assembly-exploded.png` | 21135 | `097a6e4cb7e43b19dc84f92628f93ce7ecce7b9dd18f3b73ec2f9f08ddcdb023` |
| `cleanup-separated.png` | 6732 | `1cfbb66a01b8eec948c41a05d2801e0fcd8edcfd348a717af17d85e07a1f4c74` |

Saved render receipt: project artifact `artifacts/stage6-photo-v2-20261102/render-manifest.json`.
Saved original image paths: `artifacts/stage6-photo-v2-20261102/package/docs/media/`.
The receipt also records an LCD envelope used by other images; these two guide
views omit electronics and do not use that envelope as a physical-fit certificate.
Full documentation licence: [CC-BY-4.0](../../LICENSE-documentation).


## Reference board photograph — 5 November 2026

Copyright 2026 Mykola Fiantsev. Original documentation photograph: CC-BY-4.0;
see the adjacent `reference-board-marking.jpg.license` SPDX sidecar.

`reference-board-marking.jpg` is the exact 1200 × 1600 full-frame JPEG derivative
of the owner's original `IMG_4447` photograph. The original remains outside this
repository. The derivative uses neutral resizing and standard JPEG encoding;
no crop, retouching, AI generation or added content. The reviewed derivative was copied
byte-for-byte into this repository, not exported again for this documentation
change.

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| `reference-board-marking.jpg` | 284620 | `b7741bc0463e16b129d2dbb40fce29764a27ad3e18b04f8073242aef7b54c3fd` |

The QC sticker is lifted, exposing `ESP32-S3-Touch-LCD-2.8`; the small FPC socket
beside the connected display cable is empty. The owner's order lists the
non-touch option, SKU 30223 / ESP32-S3-LCD-2.8. This photograph does not establish
the delivered SKU, the reason for the naming difference, identical PCBs/BOMs
across variants or touch-version compatibility. See the
[dated validation clarification](../../hardware/validation/waveshare-esp32-s3-lcd-2.8.md#clarification--5-november-2026).
