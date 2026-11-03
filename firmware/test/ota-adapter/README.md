<!--
SPDX-FileCopyrightText: 2026 Mykola Fiantsev
SPDX-License-Identifier: CC-BY-4.0
-->

# OTA staging adapter tests

Run from the repository root with the existing ESP-IDF v5.5.4 checkout:

```sh
IDF_PATH=/path/to/esp-idf bash ./firmware/tools/test-ota-adapter.sh
clang-tidy -p firmware/build/ota-adapter-tests firmware/components/ota/src/device_ingestion.cpp firmware/components/ota/src/image_identity.cpp
```

Use the project's pinned clang-tidy version (`CLANG_TOOLS_VERSION` in CI).
The script checks IDF's tag, the unchanged app-update sources, and the clean mbedTLS revision
`ffb280bb63c78bfec1e1ab55040671768c85c923`. It downloads nothing. The separate
native target compiles that SDK's actual SHA-256 implementation, not a substitute
or canned digest. Only the adapter's three hashing calls are redirected through
failure-injection wrappers; successful calls execute the real library. SHA known
vectors are separate from app-shaped streaming payloads. Sanitizers are enabled
by default and fail hard; the CTest timeout is 30 seconds.

The flash shim records bytes and checks handle ownership and sequential mode.
It does not compile or validate the real SDK flash driver. Payloads start with
the ESP application magic byte but are not valid signed firmware. A passing
digest is neither signature authentication nor installation approval.

The adapter independently checks the actual next application's partition type,
subtype, address against the running partition, and capacity. Image size is not
passed to SDK begin: sequential erase mode is deliberate. In pinned IDF 5.5.4,
for an application partition every failing sequential-begin return precedes
handle allocation, whereas upfront erase can fail after allocation. Handle zero
is valid; abort runs only for an owned successful session. Write failures include
possible sequential erase failures and are cleaned up by ImageIngestion.
The begin result distinguishes resource-free refusal from a failure needing
cleanup. A second borrower is refused without aborting the incumbent, which the
native test continues to a correct digest. Each ingestion/installation object
requires serialized calls. The shared device adapter now serializes its entry
points with a mutex and independently refuses competing reservations/writers.

No image-size conversion to SDK `size_t` occurs: begin always receives the
sequential-mode sentinel, while declared length remains uint64_t and is bounded
by actual partition capacity. Native huge-size rejection is not evidence of a
32-bit conversion path; there is no such conversion in this adapter.

Under rollback configuration, successful SDK begin can invalidate inactive OTA
metadata even without boot selection. Abort releases the handle; it does not
restore erased/written slot bytes or OTA metadata. The verification-only API
still never selects a boot target. The separate installation API described below
does select, but has no reboot or production caller. Hardware/backend SHA,
physical flash/metadata behavior, actual signature rejection and activation,
and the bad-OTA physical acceptance gate remain unproven.
These tests do not replace those delivery gates.

Ordinary host CI stays SDK-independent. The dedicated test and adapter static
analysis run in the existing SDK debug job. Its workflow calls the script through
`bash`, so it does not depend on an executable bit.

## Authenticated identity handoff

The explicit `verify()` step is available only after `finish()` matched length
and digest. The adapter invokes `esp_ota_end` exactly once, consumes the handle on
success and failure, then reads identity only on SDK success. The pinned target
configuration enforces signed updates. Native `esp_ota_end` is a shim, not a test
of actual RSA verification; synthetic fixtures prove call order, identity binding
and consumed-handle lifetime only. Neither historical `ImageVerified` status nor
inactive bytes authorize activation. Any reboot or replacement requires fresh
verification. No durable verification token is created.

As defense in depth, the device translation unit also refuses an ESP_PLATFORM
build with missing or disabled `CONFIG_SECURE_SIGNED_ON_UPDATE`; the existing
global CMake policy already rejects those configurations. The configuration
tests preprocess the actual adapter with enabled, disabled and missing settings.
Their empty SDK headers test only the guard, not C++ linkage or cryptography.
The non-ESP native harness intentionally does not claim target signature policy.

Owner-approved identity format 1 occupies exactly 88 bytes at offset 288: ESP
image header (24), first segment header (8), SDK app descriptor (256), followed
by `PRDOTA01` (8), little-endian format (4), minimum bootloader schema (4),
zero-padded board (32), zero-padded SemVer (32), zero reserved bytes (8).
The custom descriptor uses IDF's `.rodata_custom_desc` and `-u` retention.
Its board/version come from the same generated build inputs as packaging;
compile-time array bounds reject truncation. Packaging requires the actual
descriptor and obtains minimum schema from it. The SDK app descriptor's version
and project are checked too, and custom bytes must lie inside the first segment.
Full-array equality to the owned, already compatibility-checked candidate binds
board/schema/version (including downgrade policy) without borrowed caller strings.
Old images with no descriptor are deliberately rejected by this future OTA path;
USB self-build installation remains available.

The native generated-header fixture exercises producer compilation and bounds,
not actual ELF retention. The Mach-O host omits the ELF section attribute;
inspection of a target-built image remains a separate mandatory gate.

## Offline installation transaction

`Installation` owns decoded metadata and accepts explicitly authorized requests
and injected bytes. It does not fetch, schedule, ask for consent, dispatch events,
create a production worker or reboot. Ordered `finish`, `verify` and `activate`
steps expose existing Downloading/Verifying/Installing/RebootRequired observations
without inventing a display delay. Receiving 100% does not mean installed.
Internal version carriers increase from 24 to 32 characters to preserve supported
31-character versions; the current projection does not display that field. No
font, layout, motion or new visible error-copy acceptance is claimed.

The device lease has a private generation, never exported by the transaction and
never persisted. It survives `esp_ota_end` consuming the SDK handle. All legacy
and leased entry points share the mutex; stale leases cannot write, abort,
finalize, release or select another reservation. Selection compares the actual
retained digest and authenticated identity to the owned approved candidate and
uses the exact snapshotted target. A successful SDK selection latches the slot
until reboot: cancellation, object destruction and legacy abort do not unlock it.
Selection returns a typed Rejected/Selected/Uncertain result. Pre-call rejection
and SDK `ESP_ERR_OTA_VALIDATE_FAILED` precede metadata writes; other SDK errors
may occur after otadata erase/write and conservatively retain both slot and
admission until restart. ActivationUncertain is a failure, never a successful
selection or RebootRequired claim, and cannot retry. A separate test process
models metadata changing before the SDK returns an error and proves the retained
barriers. The ordinary Failed phase does not become globally exclusive: the
application must use its independent admission latch for those mutation guards.
Generation exhaustion refuses acquisition instead of wrapping.

The native SDK shim records the real `esp_ota_set_boot_partition` call shape;
its synthetic ESP-prefix fixtures and mocked `esp_ota_end` do not prove real RSA
validation. Tests cover competing threads/reservations, consumed handles,
identity/digest/target mismatch, selection failure, cancellation and the permanent
selected latch. Successful selection is deliberately last in the process because
there is no production escape hatch to clear that latch.

Admission extends the existing app-core exclusive-operation owner. A shared
`FirmwareUpdateAdmission` synchronously reads live application-owned conditions,
including reset confirmation, in-flight configuration and pending first-boot
validation. The serialized application task must refresh them before acquire and
feed held ownership into its guards, independently of queued OTA observations.
No production request path is instantiated in this batch. RebootRequired now
retains the existing FirmwareUpdating mutation guards. FirmwareUpdate keeps
station connectivity while blocking printer/provisioning mutation; reset/fault
station teardown is preserved.

Observations are historical snapshots, not capabilities. A future worker binding
must filter them against its live session before dispatch, especially a terminal
failure from an old request after admission has been released. Refused requests
produce no observation. Physical bad-OTA rejection, rollback, communicated reboot
and end-to-end network/consent integration remain separate delivery gates.
