<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Release validation notes

These notes preserve the technical evidence and open checks behind the user
guides. A release-day wording pass does not close a product or hardware gate.
Before publishing the guides, supply the exact signed firmware, enabled browser
installer and matching release/model links, then verify the complete route.

## Firmware and updates

Firmware updates use signed-on-update verification, first-boot health checks and
rollback handling. A nine-case physical matrix passed on the reference board
using a **private-LAN HTTPS fixture**. That does not establish delivery through
the official GitHub release channel. See [update signing](ota-signing.md) and
the [release procedure](releasing.md) for the technical contract.

The controlled 0.1.1-dev USB maintenance session confirmed boot, saved-settings
loading and Wi-Fi joining. It preserved settings with a maintenance procedure;
normal initial browser installation erases settings. It does not prove the
public browser route, automatic restart, unplug/reconnect recovery or a complete
new-user setup. Check these using the exact public release files.

## Mechanical and user checks

On 3 November 2026, the project author confirmed that the assembled and
photographed enclosure is the current version without the two printed nut
keepers. Only those keepers were removed from the print plate; the remaining
geometry is unchanged. This is the author's assembly confirmation, not a new
coordinator physical test or a measurement of button travel, nut access,
fit tolerances or removal forces. The complete beginner assembly route and
runtime checks below remain separate gates. Source and export details,
including the five required STEP bases and lack of a native Fusion timeline,
are in the [CAD source guide](../enclosure/source/README.md).

Check CORE One firmware 8.0.0 compatibility, the full real-device state sequence,
completion and sleep, setup/button/Wi-Fi behavior, official updates and a novice
build without coaching before accepting v1. Software renderer images establish
UI examples only; they do not establish LCD appearance or these hardware checks.

## Publication checklist

- Exact release, installer and model URLs; no source tag substituted for firmware.
- Signed reproducible release, source/dependency availability, retained notices
  and full redistribution package.
- Complete public browser-install, setup, update and recovery route on hardware.
- Final mechanical/button checks and beginner walkthrough from both entry pages.
- Final independent review and owner appearance/product acceptance.

The Mac application is deferred until after v1. It has no public app download,
enabled USB installation, distribution signature or notarization evidence; the
v1 user build route does not depend on it.

## Provisional setup timing and source delivery

The 0.1.1-dev idle setup timeout is about ten minutes. This is a provisional
implementation value, not an accepted release promise; user instructions refer
to an idle period until release validation fixes the final value. Reading the
page does not extend that timeout.

Before distribution, identify the exact project source, reproducible dependency
inputs, retained upstream terms and notices, and how applicable source obligations
are satisfied. A source-commit link alone does not establish completion. This
does not mean source is available for every upstream binary component. The
normal installation route must not require users to compile source.
