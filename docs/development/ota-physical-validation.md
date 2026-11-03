<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Private physical OTA validation

**Private gate result (2026-09-25):** The nine ordered physical cases passed
across six frozen local releases (`v0.0.19`–`v0.0.24`). The final trusted
`v0.0.24` transferred in full, booted from `ota_0`, was marked VALID by
ESP-IDF, and the owner confirmed the display looked normal. The private
evidence worksheet passes its consistency checker (nine runs, six distinct
releases, minimum OTA worker stack free-low 4,256 bytes); serial captures and
the worksheet remain outside Git. These results validate the private-LAN
matrix only. The official GitHub production delivery channel is still untested.

**Bootstrap gate (2026-09-24):** Signed v0.0.18 passed four-region USB flash,
byte-for-byte app readback, physical startup and the first board-to-private-server
HTTPS discovery. Metadata and manifest each returned HTTP 200 with transport `ok`
and no redirect; the same installed version was correctly rejected as `not-newer`.
Post-operation OTA stack low-water was 4,260/12,288 bytes; steady pre-TLS largest
internal block was 29,684 bytes; 4,789 display flushes had zero failures or missed
deadlines (mean 2.435 ms versus about 4.6 ms with 40-row buffers). Private serial
SHA-256: `b98fcaa14d0dc9b640c501e85069173d57dd93645bd4ded8dc8e276952c21a0c`.
The earlier signed v0.0.17 attempt failed in TLS setup with insufficient internal
heap; do not repeat it. Six clean local version-only tags/builds v0.0.19–v0.0.24
were subsequently frozen and used for the completed private matrix.

This is a private, owner-operated beta gate for signed OTA installation and rollback. It uses the
real updater, streamed image write, compatibility/digest/signature checks and boot health logic,
but substitutes a temporary HTTPS server on the owner's private LAN for GitHub release delivery.
**It does not prove GitHub API behavior, redirects, release immutability or production delivery.**
Those require a separate physical official-channel test when the product is ready for public source
release. A local matrix PASS must never be called production OTA PASS.

No product source, firmware, manifest, key, evidence or release was uploaded to GitHub for the
2026-09-24–25 private session. This document does not itself authorise signing, flashing, power
interruption or any hardware operation. Owner approval and the established signing procedure
remain separate.

## Channel and trust boundary

The tracked defaults keep `PRD_OTA_LAN_ORIGIN` and `PRD_OTA_LAN_CA_CERT_PATH` empty. Thus debug,
release and ordinary hardware-validation builds use the official GitHub path. Only a deliberately
configured `hardware-validation` build may use a local origin, which must be an exact
`https://PRIVATE_IPV4:PORT` address in RFC1918 space. Both the origin and an absolute public CA PEM
path are mandatory. CMake rejects either input in debug/release, a missing certificate, a public
IP, plaintext HTTP or a URL with path/query/userinfo. The firmware pins that origin for metadata
and both assets, disallows redirects, validates the local TLS certificate and continues enforcing
the signed-image path. The fixture key is not embedded in firmware and stays outside Git.

The local fixture advertises the same required release JSON fields and fixed asset names, with a
synthetic positive release ID and `immutable: true`. It snapshots both files into memory before
serving, so a file edit cannot change a running fixture. This is **local frozen-file evidence**,
not GitHub's immutable-release guarantee. Stop and restart with a new candidate identity if any
byte needs changing. Do not port-forward the fixture or expose it outside a trusted private LAN.

## Preparation before hardware

1. Select the computer's actual private LAN IPv4 address and a free port. Generate a dedicated
   temporary TLS certificate whose Subject Alternative Name contains that exact IP, and keep its
   private key outside this repository with restrictive permissions. This TLS key is unrelated to
   the firmware publisher signing key. Verify the certificate/IP match before proceeding.
2. In only the local `firmware/build/hardware-validation/sdkconfig`, set the two Kconfig values to
   the exact origin and **public certificate PEM** path. Reconfigure and build that profile. Check
   the build log and image: the intended `/releases/latest` URL must be embedded, and the official
   GitHub URL must not be embedded. Do not commit the local sdkconfig, certificate key or artifacts.
3. Use a reviewed clean product commit/tag for each new candidate version. The first private
   sequence `v0.0.1`–`v0.0.7` is preserved as evidence: its signed `v0.0.1` booted, but the
   live OTA service and button ISR failed before case 1. Do not move those tags or reuse their
   snapshots or signatures. The earlier repaired diagnostic sequence used USB bootstrap
   `v0.0.8` and reserved `v0.0.9` through `v0.0.14`; it did not reach case 1. The
   signed `v0.0.18` bootstrap passed its private HTTPS diagnostic. At the
   2026-09-25 close of that session, the board ran confirmed `v0.0.24`.
   Six local candidate tags/builds
   `v0.0.19` through `v0.0.24` were frozen for the cases below.
   The product tag must match `firmware/version.txt`; an environment version
   override is not a substitute. Because
   hardware-validation images retain the local origin after OTA, all six candidates use the same
   configured origin and CA. No GitHub tag/push/release is needed for this private session.

   The signed `v0.0.8` diagnostic booted with byte-verified USB flash and started both the OTA
   worker and button ISR, but Wi-Fi initialization failed while allocating its 16 static RX
   buffers. No OTA case ran. Preserve `v0.0.8`–`v0.0.14` tags, signatures and evidence; do not
   retag or mix them with the next attempt. The subsequent USB-only memory diagnostic was
   `v0.0.15` with bounded 8/8/8 Wi-Fi buffers and additional DMA-memory measurements. It
   connected to Wi-Fi, but the OTA and button tasks could not start. Signed `v0.0.16` moved
   LVGL's unchanged 64 KB pool to PSRAM; physical startup of Wi-Fi, OTA and button passed,
   but its pinned private origin became unreachable after the Mac changed subnets. Clean
   `v0.0.17` changed only the version and ignored local validation origin/certificate;
   its application startup passed after an owner USB power cycle, but its first
   board-to-private-server HTTPS attempt failed with `mbedtls_ssl_setup(-0x7F00)` /
   TLS memory allocation failure before metadata. The reviewed 16-row internal-DMA
   repair in signed `v0.0.18` passed the private HTTPS, stack and renderer gate stated
   above. No OTA candidate may be served until its own signature/package and
   intended defect have been independently checked.

4. **The session's USB bootstrap `v0.0.18` was signed.** Sign trusted OTA candidate
   bases R1–R4 and R6 with the same established publisher key using
   [Local application signing](../ota-signing.md). R2 and R3 are negative cases,
   but signing their bases ensures that their intended manifest defects are the
   only rejection reasons. **Never sign R5 `v0.0.23` with the trusted key**: its sole
   intended defect is a signature from a disposable, non-trusted key. In this
   no-secure-boot signed-app configuration, the running application's signature supplies the
   trusted key identity for later OTA checks. An unsigned USB bootstrap cannot validate
   trusted-signed OTA candidates. The then-installed v0.0.18 was independently verified against
   the chosen public key, exact unsigned-image SHA-256 and partition limits before flash.
   Never hand the private key or password to the fixture or evidence checker. The owner
   ran the five-image local signer interactively. On any future repeat, if a prompt fails,
   stop and return to root to verify the partial outputs, without rerunning or moving them
   by hand. Package
   each trusted candidate from a **private copy of its matching frozen build snapshot**
   with `package-firmware.py --profile hardware-validation --build-dir ... --signed-app ...
   --verification-key ...`; do not package an older tag from the mutable current build.
   Then run the offline consistency gate with its actual tag, full clean
   commit and `--local-origin https://PRIVATE_IPV4:PORT`:

   ```sh
   ./firmware/tools/check-release-assets.py /path/to/artifacts-signed \
     --tag v0.0.19 --commit FULL_40_CHARACTER_COMMIT_SHA \
     --local-origin https://PRIVATE_IPV4:PORT
   ```

   The tool checks the package and compiled destination; it neither signs nor proves physical
   installation. Deliberately damaged candidates should fail this normal preflight; that failure
   is a control, not a reason to weaken the gate.
5. The completed USB bootstrap used the **verified signed `v0.0.18` application**, not the
   unsigned app in a build directory. Its matching bootloader, partition table and blank
   `ota_data_initial.bin` came from the same preserved build snapshot. The snapshot's
   `flasher_args.json` specifies four writes: bootloader at `0x0`, partition table at `0x8000`,
   OTA data at `0x10000` and signed application at `0x20000`, with DIO, 80 MHz and flash-size
   `keep`. Those exact files and offsets were checked before flashing; resetting OTA data is
   essential because stale selection could boot a different image. **Do not use generic `idf.py flash` in
   this session:** the current build may contain an unsigned later candidate, not the signed
   bootstrap.
   Preserved flash/write verification shows the blank OTA data was written at `0x10000`
   and the bytes at `0x20000` match the independently verified signed-bootstrap SHA-256.
   Private serial proves the bootloader loads `ota_0` at `0x20000`, version `0.0.18`
   runs, the OTA worker and button ISR start, and the private HTTPS release check
   passes with sufficient internal memory and post-operation stack margin. Smaller
   DMA bands have zero renderer flush failures/deadline misses. Preserve these gates
   for any later diagnostic; do not flash v0.0.18 again merely to repeat this step.
   The signing key identity is **not observable at boot** in this configuration: the first live
   trust check occurs when case 1 verifies the signed OTA candidate. Do not start case 1 if the
   write verification, boot partition or version is uncertain. This procedure does not itself
   grant hardware or power-interruption authorisation.
6. For one candidate at a time, place exactly `manifest.json` and
   `prusa_remote_display.bin` in a local asset directory. Start the server with its reviewed tag
   and a unique positive synthetic release ID. Stop it before switching candidates:

   ```sh
   ./firmware/tools/serve-ota-validation.py \
     --origin https://PRIVATE_IPV4:PORT \
     --certificate /path/to/local-cert.pem --private-key /path/to/local-key.pem \
     --assets /path/to/one-candidate --tag v0.0.19 --release-id 2002
   ```

   Record the printed manifest and image SHA-256 digests. Keep the computer awake. Do not treat a
   server start or software test as evidence that the board can reach or trust it.

## Nine ordered physical cases, six local candidates — private matrix passed

The first repaired sequence `v0.0.9`–`v0.0.14` was **not run** because `v0.0.8` failed
Wi-Fi initialization. Do not serve those old candidates after flashing `v0.0.18`. The
completed mapping used six newer local tags/builds after the signed v0.0.18
diagnostic. The bootstrap's HTTPS discovery gate and all nine private cases passed.
The table records the planned observation for each case; the case evidence is kept
privately outside this repository.

| Order | Candidate | Case | Required observation | Boot-selection invariant |
| ---: | --- | --- | --- | --- |
| 1 | R1 `v0.0.19` | `valid-signed-install` | `reboot-required`, then `confirmed` | selected candidate remains active |
| 2 | R2 `v0.0.20` | `incompatible-candidate` | `incompatible` | selection unchanged |
| 3 | R3 `v0.0.21` | `digest-mismatch` | `digest-mismatch` | selection unchanged |
| 4 | R4 `v0.0.22` | `signature-rejection` | `verification-failed` | selection unchanged |
| 5 | R5 `v0.0.23` | `wrong-key-rejection` | `verification-failed` | selection unchanged |
| 6 | R6 `v0.0.24` | `interrupted-download` | `truncated` | selection unchanged |
| 7 | R6 unchanged | `interrupted-write-power-loss` | `current-image-recovered` | selection unchanged |
| 8 | R6 unchanged | `pending-boot-rollback` | `reboot-required`, then `rolled-back` | previous image active afterward |
| 9 | R6 unchanged | `final-recovery-install` | `reboot-required`, then `confirmed` | same good candidate remains active |

Prepare each negative case with just its intended defect. For R4, start with a
trusted-signed v0.0.22 image, corrupt one byte inside the RSA signature field
(bytes 812–1195 of the first 1,216-byte block), repair only the signature
block's CRC, and set the manifest's image digest to the bytes actually served;
otherwise a digest mismatch would mask signature rejection. For R5, sign the
unsigned v0.0.23 image with a new disposable non-trusted key and package it with
that key's public half; never use or publish the owner's trusted signature for R5.
The final four attempts must use the
same unchanged R6 tag, synthetic ID and candidate digest. After rollback, verify the running image
is again older than R6 before the final retry. If the board cannot retry R6, stop and preserve
evidence; do not claim recovery or silently add a new candidate.

For every attempt, independently capture before/selected/after boot partition, previous/current
image usability, the real worker stack low-water value and a distinct serial log. A power loss may
prevent the stack line only for case 7. Keep credentials, private key paths and passwords out of
logs. The local fixture's release metadata claim is not a GitHub proof.

Create an unfilled worksheet with `./firmware/tools/check-ota-physical-evidence.py --init
/path/to/evidence.json`. Enter the actual `validationOrigin`, clean baseline commit, case/tag,
synthetic `releaseId`, `fixtureFrozen`, image digest, observations, partitions, stack value and
serial-log byte digest only after each physical capture. Then run the checker without `--init`.
Its PASS proves record consistency only. Root must independently inspect captures, and the owner
must judge physical results. Keep all captures even if a case fails.

Stop on unexpected OTA classification, changed boot selection for a rejected candidate, unusable
previous image, absent TLS verification, uncertain signing identity or insufficient stack headroom.
Do not install an unreviewed image to force a green matrix.

After the session, clear both local `CONFIG_PRD_OTA_LAN_*` values in the ignored
`firmware/build/hardware-validation/sdkconfig` (or use a fresh profile build directory) and
reconfigure. `build.sh` reuses an existing sdkconfig, so leaving those values set would keep later
hardware-validation images on the test server. Preserve the signed artifacts and physical captures
separately; do not erase evidence while cleaning the local build setting.
