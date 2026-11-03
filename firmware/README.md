<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Firmware

This directory contains the ESP-IDF firmware for Prusa Remote Display.

## Target

- ESP-IDF v5.5.4
- ESP32-S3
- Waveshare ESP32-S3-LCD-2.8

## Current status

Pre-beta product firmware. Local onboarding, PrusaLink polling, the application
state model, production UI, recovery/settings flows, power behaviour and the
signed live-OTA software path are implemented and covered by host, browser,
native-render and target-build gates.

There is no public firmware release yet. The nine-case physical OTA rejection,
interruption and rollback matrix passed using a private-LAN HTTPS fixture on
the reference board; official GitHub release delivery remains untested.
Broader hardware soak evidence is also required before the firmware is
presented as beta-ready. See the repository
[README](../README.md) for the user-facing capability and limitation summary.

## Build and test

With ESP-IDF v5.5.4 activated, build a named profile from the repository root:

```sh
./firmware/tools/build.sh debug
./firmware/tools/build.sh hardware-validation
./firmware/tools/build.sh release
```

Run host logic in ordinary and sanitizer configurations:

```sh
./firmware/tools/test-host.sh
./firmware/tools/test-host.sh --sanitize
```

Full setup, tooling and packaging instructions live in
[docs/development/macos-setup.md](../docs/development/macos-setup.md). OTA signing
and trust-bootstrap constraints live in
[docs/ota-signing.md](../docs/ota-signing.md).

## Test artifact retention

Native renderer mutation controls use fresh `build/controller-tests-{1,2}/message-controls-*`
directories. After a control fires its exact expected assertion without a sanitizer
failure, the wrapper keeps `control.log` and `evidence.json` (SHA-256 of every
original file), and removes only that control's copied objects, executable, debug
symbols and raw frames. Ordinary renderer frames are not compacted. Unexpected
failures, unverified runs and unknown artifacts remain intact for investigation.
Only direct `.o` object files, direct `.rgb565` frames and the known compiler
`control.dSYM` layout are disposable. Unknown entries even inside these folders
make compaction fail closed (exit 1), preserving the entire control's evidence.

Before starting another control run, a 4 GiB allocated-space budget per allocator
checks retained mutation-control runs. Exceeding it stops the test with a clear
message; it never deletes old evidence automatically. Inspect or losslessly
archive those runs before retrying. This budget does not govern signed firmware,
physical validation records, ordinary render output or CAD files. Do not remove
the entire `build` directory to reclaim space: it may contain irreplaceable
validation evidence.

## License

Firmware and software tools are licensed under **GPL-3.0-only** — see
[LICENSE](../LICENSE). For the full project licensing model (hardware,
documentation, brand assets and contribution terms) see
[LICENSING.md](../LICENSING.md).
