<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0001: Pin ESP-IDF to v5.5.4

## Status

Accepted

## Date

2026-08-05

## Context

The firmware targets the ESP32-S3 and depends on ESP-IDF for the toolchain, the
build system, FreeRTOS, esp_lcd, LEDC, partition handling and OTA. ESP-IDF's minor
releases change driver APIs and Kconfig option names, and its patch releases carry
toolchain updates that change generated code.

Bootstrap Spec §4 requires a single supported ESP-IDF version and §19.1 makes "a
version mismatch fails clearly" an acceptance criterion. §14.3 additionally
requires reproducible build metadata, which is meaningless if the compiler can
vary between builds.

Two things make an unpinned version actively harmful here rather than merely
untidy. Hardware findings are recorded against a specific build — the panel init
sequence, the LEDC duty resolution at 5 kHz, the LVGL flush timings — and a
toolchain change invalidates the comparison. And §13.2's reproducibility job
compares two release binaries byte for byte, which cannot succeed if the
toolchain floats.

## Decision

ESP-IDF **v5.5.4** is the only supported version. It is enforced at three points,
each of which catches a different mistake:

1. **At CMake configure time**, `firmware/CMakeLists.txt` reads `IDF_VER` through
   `idf_build_get_property()` and fails with `FATAL_ERROR` unless it is exactly
   `v5.5.4`. This catches a wrong version no matter how the build was invoked.
2. **In the build wrapper**, `firmware/tools/build.sh` compares
   `idf.py --version` against `ESP-IDF v5.5.4` before doing anything, so the
   failure is a one-line message rather than a CMake stack.
3. **In CI**, every firmware job runs in an Espressif container pinned to an
   immutable digest, and asserts the version again inside it. §13.3 prohibits a
   floating tag such as `latest`; a digest is stronger still, because `v5.5.4`
   itself is a mutable tag that Espressif can repoint.

The pinned version is also compiled into the build metadata as `idfVersion`
(§14.2), so a running image and its artifacts both state which ESP-IDF produced
them.

## Consequences

- Every measurement in the hardware validation record is attributable to one
  toolchain, so figures from different phases are comparable.
- The reproducibility job is able to succeed at all.
- Upgrading is a deliberate, reviewed change: one version string, one container
  digest, and a re-run of the hardware validation that the new toolchain
  invalidates. That cost is the point.
- A contributor with a different ESP-IDF installed cannot build. The failure names
  the required version, which is better than a confusing compile error deep in a
  driver header.
- ESP-IDF 6.x migration is a deferred decision (§21) and this ADR does not
  prejudge it.

## Alternatives Considered

**A minimum version, `>= 5.5`.** Rejected. It admits exactly the drift the pin
exists to prevent, and it would silently let a contributor's 5.6 toolchain produce
different code from the validated build while both "pass".

**A range pinned only in CI.** Rejected. Hardware bring-up happens on a
developer's machine, not in CI, and that is where the validated measurements come
from. Enforcing the version only in CI would leave the runs that matter
unconstrained.

**A floating `espressif/idf:v5.5.4` container tag.** Rejected, and prohibited in
spirit by §13.3. A tag is a mutable pointer; the digest is the artifact.

**Vendoring ESP-IDF as a submodule.** Rejected for Milestone 0. It pins equally
well but adds a multi-gigabyte checkout and a tools-installation step to every
clone, for a guarantee the digest-pinned container already provides.

## Validation / Follow-up

Verified by construction and in use:

- `idf.py --version` reports `ESP-IDF v5.5.4`; all three profiles build clean.
- The CMake gate fires on a mismatch — this is the mechanism behind the accepted
  deviation D1, which records why the check reads `IDF_VER` rather than the
  `IDF_VERSION` variable the specification's snippet names: ESP-IDF v5.5 does not
  define that variable at the point the check runs.
- `./firmware/tools/print-build-info.py` reports `idfVersion = v5.5.4` from the
  generated metadata of any build directory.

Open: the container digest must be re-resolved whenever the workflow is updated
deliberately. There is no automation that notices Espressif repointing the
`v5.5.4` tag, which is the intended behaviour — the digest is the pin.
