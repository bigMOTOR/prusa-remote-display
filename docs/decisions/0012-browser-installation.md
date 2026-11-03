<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Browser installation preparation

## Status

Accepted direction for local preparation by the owner, 2026-10-04. Static hosting
choice is root's implementation decision within that scope, not publication approval.

## Date

2026-10-04

## Context

The owner accepted a ready-firmware browser installation path, without compiling.
Root selects a static GitHub Pages site for local preparation: the installer needs
HTTPS and static assets, not accounts, a database or a new server. Publication
and real USB verification are not authorized or established by this decision.
Firmware Architecture Specification section 22 requires justified, pinned and
maintainable dependencies. Bootstrap section 7.7 governs firmware components;
the browser tool is a separate npm project and does not bypass that policy.

## Decision

Use ESP Web Tools 10.4.0, Apache-2.0, locked with npm integrity records and served
from the site's own files. This widely used installation tool avoids maintaining
a new serial protocol implementation. Its maintenance, dependencies and security
status must be checked before release. It is not linked into firmware and does
not change the ESP-IDF Component Manager dependency policy.

Only an exact clean, signed, reference-board release can be prepared. A separately
trusted public key verifies the application. A merged initial image contains
bootloader, partitions, OTA data and the unchanged signed application. The
bootloader header is adapted for browser flashing; this is an offline software
check, not evidence that the board boots or recovers successfully.

Without a reviewed release, the page is a disabled preview. Require board and
erase acknowledgement before attaching the USB control. Never use latest main
as a fallback. No Improv support, preserved initial configuration, automatic board
SKU recognition or cloud setup is promised. Site hosting may keep access logs;
credentials are entered only on the display's local page.

## Consequences

The user does not need a firmware toolchain. The site must maintain browser-tool
licenses, pins and security updates; USB browser compatibility remains limited.
Initial installation erases display settings and requires explicit acknowledgement.
Changing host later is possible because the site is static and self-contained.

## Alternatives Considered

A developer command-line flasher remains a recovery route but is not the beginner
path. A new backend or hosted database adds no necessary function here. A remote
CDN makes the running installer depend on separately served code; self-hosted
locked files make the delivered candidate inspectable.

## Validation and follow up

Test disabled and board-confirmation gates, prepared hashes, four-region merging
and unchanged signed application bytes using disposable keys. Real initial USB
installation, boot, recovery and OTA remain unverified until a separately
authorized hardware run. No release asset is supplied by this preparation batch.

References: [ESP Web Tools](https://esphome.github.io/esp-web-tools/),
[release 10.4.0](https://github.com/esphome/esp-web-tools/releases/tag/10.4.0),
[GitHub Pages](https://docs.github.com/en/pages/getting-started-with-github-pages/about-github-pages).
