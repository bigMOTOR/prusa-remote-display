<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Local application signing

Use the pinned ESP-IDF Python environment. This is an owner-operated release tool,
not an automatic updater or a GitHub signing service. It never generates a key,
changes device security settings, flashes a board or publishes a release.

```sh
python firmware/tools/sign-firmware.py --image /path/to/application.bin \
  --sha256 EXPECTED_UNSIGNED_IMAGE_SHA256 \
  --encrypted-key /path/to/private.encrypted.pem \
  --public-key /path/to/trusted-public.pem --output /path/to/new-signed.bin
```

Choose the exact approved build and its SHA256 first. Choose the public key from
your established publisher identity, not from an untrusted candidate bundle.
Enter the encrypted key password at the hidden terminal prompt. There is no
password command-line argument, environment variable or piped-input mode. If
hidden input is unavailable, signing stops rather than echoing the password.
Only encrypted PKCS8 RSA-3072 private keys are accepted; their public identity
must equal the independently supplied public PEM. Existing output is never replaced.

The original input is captured once. Signing pads and signs those exact bytes
using ESP-IDF's software-compatible first signature block; the result is checked
cryptographically before output creation. RSA-PSS signing is randomized, so signing
the same application twice need not produce identical files.

Plaintext private material exists in Python process memory. It is not written to
a key file; signed public application bytes use a temporary file removed normally.
This is not a secure-memory-erasure, swap-protection or crash-dump-protection claim.
Use a trusted local machine. A disk-write failure can leave an incomplete output;
discard it and choose a new output path. Failure is never reported as success.

Next use `package-firmware.py --signed-app /path/to/new-signed.bin
--verification-key /path/to/trusted-public.pem` with the matching build/profile.
That separate gate checks original build binding and local dual-slot capacity.
Only the application is signed; the manifest, partition table and bootloader are
not signed. A signature alone does not enable device-side enforcement or prove
that an image boots. No production key is used by the automated tests.

## Software verification in device builds

Project defaults enable ESP-IDF's RSA OTA signature verification without hardware
Secure Boot. Automatic build-time signing is disabled: compilation never needs the
private key. A CMake guard checks the effective configuration, including rollback
enabled and hardware Secure Boot, eFuse anti-rollback, flash encryption and NVS
encryption disabled. These signing settings alone do not provide an update
downloader or installation UI; those are separate implemented components.

In this ESP-IDF mode the trusted key comes from the signature of the running
application, not from a downloaded manifest or a key supplied with an update.
The first trusted installation must therefore be a locally verified, signed
application installed through an explicitly authorised USB procedure. An unsigned
development application is not a trust bootstrap for OTA; no missing-key bypass
is provided. Subsequent OTA candidates must use the running application's key.
Changing that identity requires a deliberate separately verified USB installation;
there is no implemented remote key-rotation protocol.

The ESP-IDF OTA finalisation and boot-target-selection APIs perform image signature
checks when this configuration is enabled. The project has connected its downloader,
compatibility/digest checks, confirmation and health/rollback workflow. Its nine-case
physical matrix passed using a private-LAN HTTPS fixture, but the official GitHub
release channel has not been physically validated. Do not mistake configuration or
a successful build for that production-channel evidence.

Existing build directories may retain old Kconfig values. If the guard rejects one,
configure a fresh build directory with the project's base, board and profile defaults;
preserve old artifacts instead of deleting them or bypassing the guard.

Configuration guard tests run separately with Python and CMake, without ESP-IDF:
`python3 -m unittest discover -s firmware/test/configuration -p 'test_*.py' -v`.

## Test entry points

- `./firmware/tools/test-tools.sh`: standard-library-only tooling tests.
- In ESP-IDF's Python environment: `python -m unittest discover -s
  firmware/test/signing -p 'test_*.py' -v` for disposable-key signing/packaging tests.

These suites are deliberately separate so a normal Python installation does not
need ESP-IDF merely to test secret scanning and unsigned packaging.

For the complete owner-operated path from a reviewed commit to the two exact live
OTA assets, including the offline signed-bundle gate, see
[Firmware release procedure](releasing.md).
