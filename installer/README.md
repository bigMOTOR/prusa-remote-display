<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Browser installation site

This is the browser installation page being prepared for the display. The goal
is to connect the display by USB and install ready-made firmware from Chrome
or Edge, without compiling code.

There is no live installation URL or public firmware download yet. The local
preview keeps installation disabled when no verified release is supplied;
building the page does not publish it or access a board.

Prefer a Mac or Windows application? See the [desktop alternative](../desktop-installer/README.md).
For the complete build, start with [Get started](../docs/get-started.md), or
return to the [project overview](../README.md). The commands below prepare the
site for developers; they are not the installation instructions for end users.

## Build a local preview

Use Node 22.23.2, as pinned in CI. Install with `npm ci --ignore-scripts` in this
directory, then run `npm test` and `node build.mjs /absolute/new/output-directory`.
Serve that directory on localhost to preview it. Existing output is never replaced.
Invalid release inputs are rejected before output is created. An interrupted
copy or missing dependency license may leave partial output; missing release metadata disables
installation. Do not deploy failed builds.

ESP Web Tools 10.4.0 and its dependencies are pinned with npm integrity records.
The build copies the upstream self-contained browser distribution and license
texts into the site. It uses no CDN, remote scripts, analytics or backend. The
upstream npm SSR shim omits its license; the build includes the pinned Lit BSD
license and shim copyright with a checked source notice. This is a browser tool,
not an ESP-IDF firmware dependency. Review its maintenance and advisories before
each public release rather than updating automatically.

## Prepare an installation candidate

In the pinned ESP-IDF 5.5.4 environment, run:

```sh
python -B firmware/tools/prepare-installer.py /absolute/signed-release-bundle \
  --tag v1.0.0 --commit FULL_40_CHARACTER_RELEASE_COMMIT \
  --verification-key /absolute/trusted-public-key.pem \
  --output /absolute/new/prepared-bundle
python3 -B installer/accompaniment.py --tag v1.0.0 \
  --commit FULL_40_CHARACTER_RELEASE_COMMIT \
  --source-archive /absolute/prusa-remote-display-1.0.0-source.tar.gz \
  --dependency-notices /absolute/dependency-notices.tar.gz \
  --build-inventory /absolute/build-dependencies.txt \
  --output /absolute/new/accompaniment
node installer/build.mjs /absolute/new/site /absolute/new/prepared-bundle /absolute/new/accompaniment
```

Supply the intended tag, commit and trusted **public** key independently of the
bundle. This tool never signs or needs a publisher private key. It verifies the
signed release application and four initial flash regions, then uses esptool to
merge them. Only the bootloader's flash header is adapted to DIO, 80 MHz, 16 MiB;
the signed application must remain byte-identical. Initial installation erases
display configuration. No Improv support is claimed; setup uses the display's
existing local Wi-Fi page.

The site builder checks the prepared hashes and manifest contract. It does not
re-prove publisher authenticity: publish only the reviewed preparer output, not
an arbitrary directory with self-declared hashes. ESP32-S3 chip detection cannot
identify the exact board. The user must confirm the supported non-touch model
and the erase warning before the USB control is connected to the installer.

## Publication and evidence

The manual [Pages workflow](../.github/workflows/installer-pages.yml) prepares
an enabled site only from one public, stable, immutable GitHub Release. It
requires an independently reviewed release ID, tag, full firmware commit and
`installer-site.zip` SHA-256. See [the release procedure](../docs/releasing.md).
Merging the workflow does not configure Pages or grant publication authority.

The accompaniment tool copies complete upstream text bytes from the reviewed
ordinary dependency-notice archive, current notices/licensing/GPL text from the
exact frozen Git commit, and that build's dependency/configuration inventory.
It records original member paths and per-file hashes. Every project source archive
member must equal the exact frozen Git tree byte-for-byte, with no extra/missing
files, links or unsafe paths; its canonical version must match the intended tag. The `prusa-remote-display-1.0.0-source.tar.gz` archive contains exactly all tracked project files,
without an added prefix or extra SDK/config members. For other stable tags, its
release asset name is `prusa-remote-display-VERSION-source.tar.gz`, with VERSION
taken from the validated tag without the leading `v`. The separate
`build-dependencies.txt` inventory identifies the exact build configuration, SDK
and submodule revisions, links to their ordinary source inputs, and records each
input SHA-256. That inventory is copied and hashed as a notice; review those
linked inputs separately before upload. The tool preserves and binds inputs, not a new legal/linker audit.
The builder refuses enabled firmware without this accompaniment and copies every
notice into the site and combined third-party notices, with the exact source
archive download URL and SHA-256. Preview builds remain disabled.

Use a **new v1 source archive**, not the older 0.1.1 kit. The final tagged build,
configuration and source archive do not exist during local preparation. The separate linked inventory covers
ESP-IDF 5.5.4 with submodule references, LVGL 9.5.0, mDNS 1.11.3, Inter notices,
full upstream license/notice texts and actual sdkconfig/build details under the
accepted ordinary source route. Reuse unchanged upstream full texts by verified
hash; refresh current project notices and build configuration. Do not substitute
an old archive under a new tag. Firmware upload remains blocked until these exact
inputs and existing owner/physical/publication gates are complete.

First physical browser-install checks must include automatic reset/boot. If the
board stays in download mode, test unplugging/reconnecting USB and document that
recovery for users; it is not yet a verified requirement for this installer.
Optional CSP hardening is deferred until the real upstream dialog/style paths
are browser-tested; no claim of a security audit is made.

Generated site output is limited to 20 MiB, including at most one 16 MiB image.
Dependencies stay in ignored `node_modules`; do not create repeated copies.
Test fixtures are deleted only from their newly allocated temporary directories.
Retain failure logs and candidate hashes; remove successful scratch builds only
after their evidence is saved. Do not touch CAD packets, hardware observations
or signing material as part of site cleanup.
