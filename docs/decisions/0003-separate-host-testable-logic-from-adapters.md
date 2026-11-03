<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0003: Separate host-testable product logic from ESP-IDF adapters

## Status

Accepted

## Date

2026-08-05

## Context

Almost nothing about this product's correctness is hardware-specific. Semantic
version parsing, a boot reducer, a perceptual brightness curve, button gesture
classification, DMA buffer-ownership accounting, and later the whole PrusaLink
normalizer and UI projection are decisions about values and state transitions.

Testing them on hardware is slow and weak: a flash-and-observe cycle takes
minutes, cannot easily reach an edge case, and produces evidence a human has to
read. Testing them on the host takes under a second and can assert exactly.

Bootstrap Spec §11 requires a host test target that does not depend on ESP-IDF,
and §19.4 makes "host-tested headers do not depend on ESP-IDF" an acceptance
criterion. Architecture Spec §3.4 makes testability a quality attribute and §5.1
forbids ESP-IDF types in `app_model`.

The constraint that makes this non-trivial: ESP-IDF headers are not clean under
this project's warning policy. They use undefined macros, anonymous structs and
`#include_next`, so a translation unit that includes them cannot compile under
`-Wundef -Wpedantic` without help.

## Decision

Product logic is separated from ESP-IDF adapters at the **translation unit**
boundary, not merely by directory.

**Pure translation units** include no ESP-IDF header, compile under the full
warning policy with no relaxation, and are compiled by both the firmware build and
the host test build from the same source file — not copied, not reimplemented:

| Unit | Component |
|---|---|
| `build_info.cpp`, `boot_state.cpp`, `board_identity.cpp`, `hardware_check.cpp` | `app_model` |
| `boot_reducer.cpp` | `app_store` |
| `redaction.cpp` | `diagnostics` |
| `brightness_curve.cpp`, `transfer_tracker.cpp` | `display` |
| `press_classifier.cpp`, `button_event.cpp` | `button` |

**Adapter translation units** own all ESP-IDF contact — `display.cpp` (esp_lcd),
`backlight.cpp` (LEDC), `button.cpp` (GPIO and ISR), `platform/*.cpp`, `ui/*.cpp`
(LVGL). They carry a documented, file-scoped warning relaxation and nothing more:
`-Wno-undef;-Wno-pedantic` applied per source file in the component's
`CMakeLists.txt`, or a `#pragma GCC diagnostic` block around the offending
includes. Both are scoped and both state their reason, per §12.3 rules 4 and 5.

Where a pure unit is an internal implementation detail rather than a public API, it
lives under the component's `src/` and the host build adds that directory to its
include path — `brightness_curve.hpp`, `transfer_tracker.hpp`,
`press_classifier.hpp`. This keeps them out of the component's public interface
while still testing them directly, rather than testing them through an adapter that
would need hardware.

Time is reached through a seam. `platform::monotonic_time()` is the adapter; the
classifier takes a timestamp as a parameter, and the host tests drive it with
`test/host/support/fake_clock.hpp`. A gesture classifier that called an ESP-IDF
clock could not be tested at all without hardware, and its 8-second threshold could
not be tested in finite patience.

## Consequences

- 71 host tests run in about 0.4 s, so they run on every change rather than at
  review time.
- The button classifier's full behaviour — settle-debounce, three gesture classes,
  the exact emission rule — is pinned by 22 tests that never touched a board. The
  hardware validation then had only to confirm the pipeline, not the logic.
- clang-tidy has a well-defined scope. §12.2 requires host-compatible code only;
  because the pure units are exactly that set, the static-analysis scope falls out
  of the architecture instead of being a hand-maintained list.
- Sanitizers reach the logic. ASan and UBSan cover the same source the firmware
  compiles, which no on-target test arrangement would provide at this stage.
- The cost is indirection: the brightness curve does not call LEDC, and the
  classifier does not read a GPIO. Each needs a small adapter above it. That is a
  real cost in lines and it is accepted.
- A second cost, discovered in practice: a pure unit tested only on the host can
  still be wrong about the hardware. The Phase F colour-format defect passed every
  host test and every counter, and was caught by photographing the panel. Host
  tests bound the logic; they say nothing about whether the logic is the right
  logic for the device.

## Alternatives Considered

**Test everything on target with Unity.** Rejected for Milestone 0. §11.7 reserves
`firmware/test/target/` for it in a later milestone, and it is the right tool for
driver behaviour. It is the wrong tool for a version parser: minutes per cycle, and
it cannot inject a fake clock to test an 8-second threshold.

**Mock ESP-IDF headers for the host build.** Rejected. Maintaining a mock of
esp_lcd, LEDC and FreeRTOS is a large ongoing cost, and the mock would drift from
the real API in exactly the ways that matter. Keeping the logic ESP-IDF-free is
cheaper and gives a stronger guarantee.

**Relax the warning policy repository-wide so ESP-IDF headers compile
everywhere.** Rejected, and it is the alternative worth naming explicitly because
it is the tempting one. It would let any file include any header, and it would
stop checking the project's own conversions and shadowing in precisely the files
that do pixel and duty arithmetic. The per-file relaxation is more work and is the
correct trade.

**Duplicate the logic — one copy for host, one for target.** Rejected outright.
Two copies of a state machine are two state machines.

## Validation / Follow-up

- 71 host tests pass from a clean configure, and pass again under ASan and UBSan.
- `clang-tidy` reports zero findings across the ten pure component translation
  units, with `WarningsAsErrors: '*'`.
- The separation is checked mechanically rather than trusted: no LVGL type appears
  in any public header, `esp_lcd` is called only from `display/src/display.cpp`,
  and `board_pins.h` is included only inside `board`.
- The pure units carry no warning relaxation; only the adapters do, each with its
  reason in the `CMakeLists.txt` beside it.

Open: `firmware/test/target/` is still empty. Driver-level behaviour — esp_lcd
transfer edge cases, LEDC re-timing — is validated today by hardware bring-up flows
and recorded evidence rather than by automated on-target tests. That is adequate
for Milestone 1 and will not stay adequate indefinitely.
