<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0004: Stage security features; do not burn eFuses during bootstrap

## Status

Accepted

## Date

2026-08-05

## Context

The product will eventually store a PrusaLink API key and a Wi-Fi credential on
the device. Both are secrets; ESP-IDF protects them with NVS encryption, whose key
is itself protected by Flash Encryption or by an HMAC key held in eFuse.

eFuse writes are **irreversible**. Burning a key or enabling Secure Boot on a
development board is a one-way operation: get the scheme wrong and the board is
scrap, or is permanently locked to a key that was never meant to be a production
key. There is exactly one prototype unit, and Milestone 1 depended on it.

Bootstrap Spec §9.6 states that Milestone 0 reserves the partitions and keeps the
encrypted-NVS code path buildable but does not initialise encrypted NVS, and that
production key provisioning is not performed in Milestone 0. §21 defers the
production NVS encryption key workflow, Secure Boot and Flash Encryption
enablement.

The ordering argument is decisive: the firmware has no secret to protect yet. Wi-Fi
and provisioning are Milestone 3; PrusaLink is Milestone 4. Committing to a key
scheme now would mean choosing it before knowing what it must protect, and paying
for that choice in hardware that cannot be un-chosen.

## Decision

Security is **staged**. Milestone 0 makes the code path exist and reserves the
storage, and burns nothing.

**Now (compiled and reserved):**

- `CONFIG_NVS_ENCRYPTION=y` — the encrypted-NVS code path compiles, so the API it
  requires cannot drift out of the build unnoticed.
- `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC=y` with
  `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID=0` — a **compile-time placeholder only**. No
  eFuse is read and none is written. Key ID 0 is a syntactically valid value that
  lets the configuration resolve; it is not a decision about which key block
  production will use.
- `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC is not set` — the alternative scheme
  is explicitly off rather than left to a default.
- Partitions reserved: `nvs_keys` (0x1000, marked `encrypted`), `factory_cfg`
  (0xD000, `encrypted`), `coredump` and `assets` likewise.

**Not now:**

- No eFuse is burned, for any purpose.
- Encrypted NVS is not initialised; nothing is stored in it.
- Flash Encryption is disabled, so the `encrypted` flags on those partitions
  currently **document intent rather than activate encryption** (§9.6). This is
  worth stating plainly: a reader who sees `encrypted` in `partitions.csv` today
  must not conclude that anything is encrypted.
- Secure Boot is not enabled.
- No OTA image signing (see ADR-0002; the manifest declares an unsigned signing
  block).

The staging boundary is the arrival of a real secret. The key workflow shall be
decided, written down and validated **before** any credential is persisted — not
before Milestone 3 begins, but before Milestone 3 stores anything.

## Consequences

- The prototype unit stays usable and re-flashable throughout bring-up. Milestone 1
  flashed it many times, erased it once, and recovered it from sticky download mode
  — none of which would have been safe with Secure Boot enabled.
- The encrypted-NVS API stays compiled, so an ESP-IDF upgrade that changed it
  would break the build rather than surface in Milestone 3.
- The partition layout does not need to change when encryption is enabled, because
  the partitions and flags are already in place.
- **The accepted risk, stated so it cannot be forgotten:** for as long as this
  configuration stands, a secret written to NVS would be stored in plaintext in
  flash. Nothing writes one today. Anything that begins to must land after the key
  workflow, not alongside it. This is the single item most likely to be overlooked
  under schedule pressure in Milestone 3.
- Enabling Flash Encryption and Secure Boot later requires a documented
  provisioning procedure and, for development units, a deliberate decision about
  which boards are sacrificed to test it.

## Alternatives Considered

**Enable Flash Encryption and Secure Boot now.** Rejected. It burns eFuses on the
only prototype before the firmware has a secret worth protecting, and it obstructs
exactly the flash-erase-recover cycle that Milestone 1 needed. It also inverts the
order of decisions: the key scheme should follow from what it protects.

**Leave `CONFIG_NVS_ENCRYPTION` off until Milestone 3.** Rejected. The code path
would then compile for the first time at the moment it is needed, which is when a
surprise costs the most. Compiling it now is nearly free and catches API drift
early.

**Choose the key-protection scheme now, and burn later.** Rejected as false
economy. The choice between HMAC-in-eFuse and Flash-Encryption-derived keys depends
on the provisioning process and on whether units are programmed in-house or by a
contract manufacturer — neither of which is decided. Recording a choice that will
be revisited is worse than recording that it is open.

**Use a plaintext credential store and treat secrecy as out of scope.** Rejected.
An API key grants control of a printer; the Product Brief's local-first posture
means the user's trust is the product.

## Validation / Follow-up

- All three profiles build with `CONFIG_NVS_ENCRYPTION=y`, proving the code path
  compiles.
- No eFuse operation exists anywhere in the firmware: no `esp_efuse_*` call is
  present, and no build or tool step writes one.
- The prototype has been flashed, fully erased and recovered repeatedly across
  Milestone 1 with no irreversible operation performed.
- `package-firmware.py` scans shipped binaries for credential markers on every
  packaging run (§13.5). The shipped binaries are clean today, which is the
  baseline that would have to stay true.

Blocking follow-up, before **any** credential is persisted:

1. Decide the production key-protection scheme and write it as an ADR.
2. Define and document the provisioning procedure, including which eFuse blocks are
   used and by whom.
3. Decide whether Secure Boot is enabled for production, and how development units
   are handled if it is.
4. Validate the whole flow on a unit that is expendable, because the first attempt
   may not be recoverable.
