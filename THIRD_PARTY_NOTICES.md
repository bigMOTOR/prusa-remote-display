<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Third-Party Notices

This file records third-party code, libraries, fonts, icons, assets and
vendor material **redistributed in this repository**, together with their
copyright and license information.

## Redistributed third-party material

### Inter 4.1

| | |
|---|---|
| Component | [Inter](https://github.com/rsms/inter), release tag `v4.1` |
| Copyright | Copyright (c) 2016 The Inter Project Authors |
| License | SIL Open Font License 1.1 (`OFL-1.1-no-RFN`) |
| License text | [`assets/fonts/inter/LICENSE.txt`](assets/fonts/inter/LICENSE.txt) |
| Provenance | [`assets/fonts/inter/PROVENANCE.md`](assets/fonts/inter/PROVENANCE.md) |

Files redistributed, under `assets/fonts/inter/`:

- `Inter-Regular.ttf`
- `Inter-Medium.ttf`
- `Inter-SemiBold.ttf`
- `Inter-Bold.ttf`

The typeface required by the Production UI specification. Vendored rather than
fetched at build time, so a build months from now uses the bytes that were
validated on the bench and neither the firmware build nor CI needs network
access.

`assets/fonts/inter/LICENSE.txt` is the licence text as shipped in the upstream
release archive, copied unmodified.
[`assets/fonts/inter/PROVENANCE.md`](assets/fonts/inter/PROVENANCE.md) records
the upstream release, the archive and per-file SHA-256 checksums, which four of
the archive's thirty-six files are vendored and why, and the Reserved Font Name
analysis — verified against the vendored licence text rather than assumed.

Inter 4.1 declares no Reserved Font Name, so OFL clause 5 does not restrict the
naming of derivatives.

**Derived font sources are also redistributed, and are also OFL.** The seven
LVGL font sources under `firmware/components/ui/src/fonts/prd_font_*.c` are
generated from the files above by `firmware/tools/generate-fonts.py` and are
committed so that an ordinary build needs neither the converter nor network
access. They are a **Modified Version** of Inter within the meaning of the OFL:
the pipeline freezes the `tnum` feature into the character map, subsets the
glyph set, and rasterises the outlines into C arrays — and the licence's own
definition covers derivatives made "by changing formats or by porting the Font
Software to a new environment".

They therefore carry Inter's copyright notice and
`SPDX-License-Identifier: OFL-1.1-no-RFN`, not this project's licence, because
OFL clause 5 requires the Font Software "modified or unmodified, in part or in
whole" to be distributed entirely under the OFL. **This does not change the
firmware's licence.** SIL's own FAQ 1.3 draws the boundary: "Only the portions
based on the Font Software are required to be released under the OFL." The
project-authored wrappers `fonts.cpp` and `fonts.hpp`, and the generator
`generate-fonts.py`, are original work and remain GPL-3.0-only.

`firmware/tools/licensing-manifest` records this classification, and
`check-licensing.sh` enforces it — including that the seven generated files
must **not** carry this project's copyright line.

### Slicer profiles are not redistributed

The enclosure 3MF contains geometry and build transforms only. Printer, print,
filament and support settings, profile files and macros are not bundled. Select
the official profiles for your printer and material in your own slicer. The
project author's textual recommendations are starting points, not copied presets.

Inter is the third-party material currently recorded as redistributed in this
repository. Record any additional material and its applicable terms before
redistribution.

## Build-time dependencies (not redistributed here)

### ESP-IDF firmware libraries

The reference firmware uses **ESP-IDF v5.5.4**, for **ESP32-S3**.
The following version-scoped list identifies the precompiled libraries covered
by the additional permission in
[LICENSING.md](LICENSING.md#additional-permission-for-esp-idf-libraries).
A library being listed does not mean every build includes it.

| SDK location | Library files | Upstream terms |
|---|---|---|
| `components/esp_wifi/lib/esp32s3/` | `libcore.a`, `libespnow.a`, `libmesh.a`, `libnet80211.a`, `libpp.a`, `libsmartconfig.a`, `libwapi.a` | Espressif Wi-Fi binary-library README and LICENSE, Apache-2.0 Object form |
| `components/esp_phy/lib/esp32s3/` | `libphy.a`, `libbtbb.a` | Espressif PHY binary-library README and LICENSE, Apache-2.0 Object form |
| `components/esp_coex/lib/esp32s3/` | `libcoexist.a` | Espressif coexistence binary-library README and LICENSE, Apache-2.0 Object form |
| `components/xtensa/esp32s3/` | `libxt_hal.a` | Xtensa target-support library supplied with this ESP-IDF version; retain its applicable upstream terms, without treating the RF README as its license |

These libraries are not vendored in this Git repository. A firmware binary may
contain their code, so distributing that binary is separate from distributing
this repository's source. Keep the applicable upstream copyright, license and
notice texts with a firmware distribution; the project's additional permission
does not grant rights owned by Espressif or other third parties.

For the ordinary source route, provide the project repository tag, the pinned
ESP-IDF v5.5.4 with its submodules, the project's `sdkconfig.defaults*`, the
selected build's `sdkconfig`, and this notices file. LVGL, mDNS, Inter and
the other separately licensed dependencies retain the terms recorded below
or in their upstream packages. Changing the SDK requires updating this list.

### Browser installer distribution

The repository tracks the installer source and npm lockfile, not third-party
JavaScript. Building the static site **does redistribute** the precompiled
browser distribution of [ESP Web Tools 10.4.0](https://github.com/esphome/esp-web-tools/releases/tag/10.4.0)
(Apache-2.0, ESPHome contributors) and its bundled code. `installer/build.mjs`
preserves upstream headers and copies pinned packages' license and notice texts
into `licenses/` and `third-party-notices.txt`. Lit packages retain BSD-3-Clause;
esptool-js, Material Web, Improv Wi-Fi SDK and the other pinned packages retain
their respective terms recorded in `installer/package-lock.json` and the copied
full texts. The pinned SSR shim tarball lacks LICENSE; its checked Google source
notice and the upstream Lit workspace BSD text are included explicitly.

No third-party CDN or runtime download is required by the built page. Never
publish the vendor files without those notices. The original installer remains
GPL-3.0-only, with its license text and linked source available alongside the site.

For transparency, the following third-party components are used to build or
test the firmware but are **not** redistributed in this repository. Each
retains its own license; installing or fetching them is the developer's
responsibility.

| Component | Used for | Obtained | License |
|---|---|---|---|
| ESP-IDF v5.5.4 | Firmware framework / toolchain | Installed separately by the developer (not vendored) | Apache-2.0 (with its own bundled third-party components) |
| LVGL 9.5.0 | Display graphics (ADR-0005) | Fetched by the ESP-IDF Component Manager, pinned in `firmware/dependencies.lock`, into the ignored `firmware/managed_components/` tree | MIT (carries Project Nayuki's `qrcodegen`, MIT) |
| espressif/mdns 1.11.3 | Advertising the device on the local network (ADR-0007) | Fetched by the ESP-IDF Component Manager, pinned in `firmware/components/network/idf_component.yml` and `firmware/dependencies.lock`, into the ignored `firmware/managed_components/` tree | Apache-2.0 |
| GoogleTest | Host unit tests | Fetched at configure time via CMake `FetchContent` into the ignored `firmware/build/` tree | BSD-3-Clause |
| Node.js 22.23.2 and Google Chrome | Browser regression suite (ADR-0006) | Installed by the developer, or provided by the CI runner image; neither is downloaded by this repository | Node.js: MIT. Chrome: Google's own terms |

These entries are informational. Because the components are not distributed
from this repository, their license texts are not reproduced here; refer to
each project for the authoritative terms.

One note on `espressif/mdns`, so nobody has to re-derive it: the ESP component
registry's own metadata reports its license as **"Custom"**, and the license
text the registry serves for version 1.11.3 is the **Apache License 2.0**
verbatim — as is the `LICENSE` file in the fetched component. The table records
what the text says rather than what the metadata field says. Apache-2.0 inbound
to a GPL-3.0-only project is compatible.

## Adding third-party material later

If third-party code, CAD, fonts, icons or other assets are ever added to and
redistributed from this repository, they must be:

1. confirmed license-compatible with the applicable repository-area license
   (see [LICENSING.md](LICENSING.md));
2. added to the **Redistributed third-party material** section above with the
   component name, version, origin, copyright holder and license;
3. accompanied by the required license text or notice where the license
   demands it.
