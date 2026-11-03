<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0002: Dual OTA slots without a factory application

## Status

Accepted

## Date

2026-08-05

## Context

The board carries 16 MB of flash, confirmed genuine on the physical unit in
Milestone 1 Phase B (JEDEC `0x20`/`0x4018`, full-range read with no aliasing).
The product is a desktop appliance that must survive a failed update without a
USB cable, because the user will not have one to hand and may not be able to open
the enclosure.

ESP-IDF offers two shapes for this. A `factory` app partition plus OTA slots lets
the bootloader fall back to an image flashed at manufacture. Two OTA slots with no
factory partition make every image an update, with the bootloader alternating
between them.

Bootstrap Spec §19.2 requires the partition layout to be validated and §19.3
requires the application to fit both OTA slots. §9 specifies a 16 MB dual-slot
layout with rollback.

The relevant sizes: the largest image built to date is the hardware-validation
profile at 0xecae0 (967 kB), which includes LVGL and a 48 px validation font.
Release is 0x65f00 (417 kB).

## Decision

The partition table provides **`ota_0` and `ota_1`, each 0x680000 (6.8 MB), and no
factory application partition**. `otadata` selects between them and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` allows the bootloader to revert to the
previous slot when a new image fails to confirm itself.

The table also contains `factory_cfg`, which despite its name is a **data**
partition (subtype `0x40`) reserved for factory configuration, not an application
image. It is not a fallback and the bootloader will never boot from it.

Full layout, validating as exactly 16 MB:

| Partition | Type | Subtype | Offset | Size |
|---|---|---|---|---|
| `nvs` | data | nvs | 0x9000 | 0x6000 |
| `nvs_keys` | data | nvs_keys | 0xF000 | 0x1000 |
| `otadata` | data | ota | 0x10000 | 0x2000 |
| `phy_init` | data | phy | 0x12000 | 0x1000 |
| `factory_cfg` | data | 0x40 | 0x13000 | 0xD000 |
| `ota_0` | app | ota_0 | 0x20000 | 0x680000 |
| `ota_1` | app | ota_1 | 0x6A0000 | 0x680000 |
| `coredump` | data | coredump | 0xD20000 | 0x100000 |
| `assets` | data | 0x41 | 0xE20000 | 0x1E0000 |

## Consequences

- Rollback protects against a bad update without a cable, which is the failure
  mode that matters for an appliance.
- Both slots are the same size, so any image that fits one fits the other and
  there is no "this update is too big for the other slot" class of failure.
- 13.6 MB of the 16 MB is committed to application slots. That is generous —
  the largest image uses 14% of a slot — and deliberately so: §21 defers the final
  app-slot size until production firmware sizes are known, and shrinking a
  partition table after units ship is far harder than shrinking it now.
- There is no image of last resort. A user who manages to write a broken image
  into both slots needs USB recovery. Accepted: the rollback mechanism exists to
  make that path unreachable in normal operation, and USB-C recovery is validated
  and documented.
- `assets` (1.9 MB) and `coredump` (1 MB) are reserved now so adding them later
  does not require repartitioning a shipped device.

## Alternatives Considered

**Factory app plus a single OTA slot.** Rejected. It gives a guaranteed fallback,
but every update then overwrites the only updatable slot, so a failed update leaves
the device on factory firmware until someone intervenes. For a product whose whole
value is showing current printer state, silently reverting to an old image is worse
than the dual-slot risk.

**Factory app plus two OTA slots.** Rejected on arithmetic. Three app partitions
of a size that fits a future LVGL-heavy image do not leave room for the data
partitions on 16 MB, and shrinking the slots to fit would reintroduce the
size-limit problem §21 exists to avoid prejudging.

**Asymmetric slot sizes.** Rejected. It buys flash at the cost of an update path
that works in one direction only.

**Smaller slots now, larger later.** Rejected. Repartitioning a device that is
already in a user's hands is exactly the migration this layout is sized to avoid.

## Validation / Follow-up

- The partition table validates as **exactly 16 MB** — verified from
  `partitions.csv` and reported by `package-firmware.py` in `size-report.txt`.
- The application fits both slots with margin: hardware-validation at 0xecae0
  leaves 86% of the smallest slot free; release at 0x65f00 leaves 94%. ESP-IDF's
  own `check_sizes.py` asserts this on every build.
- Booting from `ota_0` with rollback enabled was validated on hardware in
  Milestone 1 Phase B — 25 clean boots and a 31.8-minute soak.

Open, and deliberately deferred to a later milestone: OTA image signing (§21 —
signature format and update transport), and the final slot size once production
firmware size is known. `manifest.json` already emits the compatibility fields
that a signed update will need, with the signing block declared and unsigned.
