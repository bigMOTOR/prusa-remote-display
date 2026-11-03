<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Licensing

Prusa Remote Display uses an open-source licensing model for its firmware,
hardware design files and documentation. Different parts of the repository use
different licenses, chosen so the project stays genuinely open-source while
allowing commercial kits and finished devices. This document explains which
license applies where and what it permits.

The additional permission below supplements the firmware license. The other
sections are plain-language summaries; the linked license texts govern those
summaries.

---

## License by repository area

| Area | Contents | License | SPDX |
|---|---|---|---|
| Firmware and software tools | `firmware/`, build/packaging scripts, and any other original software | GNU General Public License v3.0 only | `GPL-3.0-only` |
| Hardware design files | `enclosure/source/`, `enclosure/step/`, `enclosure/stl/`, `enclosure/3mf/`, `hardware/` | CERN Open Hardware Licence v2 — Strongly Reciprocal | `CERN-OHL-S-2.0` |
| Documentation | root `README.md`, `docs/`, other public READMEs, assembly/development/troubleshooting guides, original diagrams | Creative Commons Attribution 4.0 International | `CC-BY-4.0` |
| Project name, logo, wordmarks, packaging identity, designated brand assets | Names, logos, wordmarks, packaging identity and other designated brand assets | All rights reserved | — |

Full texts:

- Firmware/software — [LICENSE](LICENSE)
- Hardware — [LICENSE-hardware](LICENSE-hardware)
- Documentation — [LICENSE-documentation](LICENSE-documentation)

Third-party material retains its own license — see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Additional permission for ESP-IDF libraries

### Additional permission under GNU GPL version 3, section 7

The copyright holder of the original Prusa Remote Display firmware grants
additional permission to link or combine that firmware with the precompiled
libraries supplied in ESP-IDF v5.5.4 and listed in the
[ESP-IDF firmware libraries table](THIRD_PARTY_NOTICES.md#esp-idf-firmware-libraries),
and to convey the resulting firmware under GPL-3.0-only.

For this permitted combination, source code need not be provided for a listed
library that its upstream supplier distributes only in precompiled form.
All other Corresponding Source obligations remain in force. This permission
does not replace third-party licenses, waive required notices, or waive
Installation Information required by GPLv3 section 6. It does not authorize
relicensing any third-party contribution.

This permission applies only to the listed ESP-IDF v5.5.4 libraries. Review
the list when changing ESP-IDF versions. It does not change GPL-3.0-only to
“GPL-3.0-or-later”.

See [building and running your own version over USB](docs/development/first-flash.md#build-and-run-your-own-version-over-usb).

---

## Commercial use

All three open-source licenses permit commercial use, subject to their terms:

- **Firmware (GPL-3.0-only).** You may sell devices and services built on the
  firmware. If you distribute a modified version of the firmware, you must
  make the corresponding source of that modified version available under
  GPL-3.0-only. You may not distribute a closed, proprietary firmware derived
  from this project.
- **Hardware (CERN-OHL-S-2.0).** You may manufacture and sell the hardware,
  including commercially. When you distribute a product made from the covered
  design, provide each recipient with its Complete Source or notify them of
  the Source Location. Complete Source includes the design information needed
  to make the product, in the preferred form for modification. Modified covered
  designs, including larger works incorporating them, must be licensed as a
  whole under CERN-OHL-S-2.0. The licence's Available Component exception keeps
  qualifying components under their own terms; include the information needed
  to make or source and use them. Retain the required notices and identify
  modifications as the licence requires.
- **Documentation (CC-BY-4.0).** You may copy, translate, adapt and include
  the documentation in commercial products, provided you give appropriate
  attribution as required by the license.

Commercial use does **not** grant any right to the project's name, logo,
wordmarks or other brand assets (see below).

---

## Generated CAD exports

Neutral and printable exports generated from the editable hardware source —
STEP, STL, 3MF and similar — are covered by the hardware license,
**CERN-OHL-S-2.0**, even when the file format cannot embed an SPDX
identifier. The editable CAD source under `enclosure/source/` is authoritative;
its license flows to the exports derived from it. This covers the project's
original geometry, not third-party material bundled alongside it. No printer, print or filament presets are bundled with the geometry-only 3MF.
The Waveshare board, Inter fonts and dependencies are not relicensed as this
project's hardware.

---

## Third-party material

Third-party code, libraries, fonts, icons, CAD, vendor material and other
assets retain their original licenses. They are not relicensed by this
project. Anything redistributed in the repository is recorded, with its
license, in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Build-time
dependencies that the project fetches or that a developer installs separately
(for example ESP-IDF, or GoogleTest fetched for host tests) are likewise
governed by their own licenses.

---

## Project name, logos and brand assets

The open-source licenses above cover software, hardware and documentation.
They do **not** grant any trademark or branding rights. The following remain
all rights reserved:

- the project and product names;
- logos and wordmarks;
- packaging identity;
- other designated brand assets.

This lets anyone build compatible forks and commercial products from the
openly licensed material, while preventing those products from being
represented as official project products.

**Functional exception.** A functional asset that is genuinely required to
build the firmware is not treated as a reserved brand asset unless an openly
licensed replacement is provided. Build-required assets are licensed with the
firmware (GPL-3.0-only), not withheld as brand assets.

---

## Contributions

The contribution policy is **inbound license = outbound license**:
contributions are accepted under the license that already applies to the
repository area being changed (firmware under GPL-3.0-only, hardware under
CERN-OHL-S-2.0, documentation under CC-BY-4.0).

By submitting a contribution you confirm that you have the right to submit it
under the applicable license. Do not add third-party code, CAD, fonts, icons
or other assets without first confirming license compatibility and recording
them in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
