<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Desktop installation application

## Status

Accepted for local implementation by the owner on 2026-10-05. Publication,
signing credentials and real USB installation are separate authorizations.

## Decision

Provide a shared Qt Widgets application for Mac and Windows, with Python and
the official Espressif esptool bundled using PyInstaller. Users do not install
Python, run commands or use a particular browser. The browser installer remains
an alternative. Use Qt Essentials rather than QtWebEngine or an Electron shell.

Application and firmware versions are independent. Discover official stable
firmware, display and freeze its exact version before installation, and never
use main or silently install. Application-only releases must not replace the
repository's firmware latest release: use `installer-vX.Y.Z` and
`make_latest=false`, with a post-publication check of the latest firmware
endpoint. Firmware discovery requires `immutable: true`, consistent with
the device OTA release contract. A change to the initial installation
format can require an application update.

Require a separately trusted publisher public key, exact supported non-touch
board confirmation, a user-selected port and explicit erase acknowledgement.
Validate all four initial flash regions and the signed application. Hashes
supplied by the downloaded bundle are consistency checks, not independently
trusted publisher identity. TLS delivery of the initial bootloader and data
is distinct from the application's embedded signature. No private signing key
belongs in the application. Without configured trust, installation is disabled.

## Dependencies and packaging

The desktop environment is separate from ESP-IDF. Pin direct and transitive
dependencies with generated hashes. Start with esptool 4.12.0, already used in
the project's offline installation tests; do not implicitly upgrade firmware
tooling to the latest API. Use PySide6 Essentials 6.11.2, cryptography 44.0.3
and PyInstaller 6.22.3. Bundle certifi 2026.7.22 for HTTPS root certificates,
so the application does not rely on development-machine certificate paths.
Review advisories and upstream maintenance before
distribution. These are starting pins, not a security certification.

Prefer app-bundle or directory distributions rather than self-extracting single
files. Build on each supported platform. Mac evidence cannot certify Windows.
Signing, notarization, full third-party notices and corresponding source,
boot/reset/unplug recovery and exact released firmware provenance remain
distribution gates. No publicly install-ready release is established here.

## CI cadence — accepted 2026-10-05

Run the full desktop and disposable-key signing suites automatically on Linux for
relevant changes. Run Mac/Windows suites and native preview builds only through an
owner-requested manual workflow before releases or after platform-sensitive changes.
The project PM explicitly reminds the owner at those checkpoints. Pin managed
Python 3.12.13 through uv 0.11.6 rather than falling back to an older Python patch
when setup-python lacks Mac/Windows binaries. Dependency hashes and no-build-isolation
remain required. Cache dependency downloads and cancel superseded same-ref runs.
Linux success never certifies native packages; require green native jobs for the
exact frozen candidate and repeat if it changes before publication.
Verify the run's head SHA equals that candidate, not merely the selected branch name.
Avoid pushing to the candidate branch during native validation: same-ref automatic
and manual runs share cancellation, so a new push can invalidate the manual run.

Local evidence used Homebrew CPython 3.12.13, not uv-managed standalone CPython.
The first manual Linux/macOS/Windows run must therefore validate the managed
interpreter, Qt and native PyInstaller path itself. No hosted success is established
yet. Investigate failures and review any runtime correction; a silent Python patch
fallback or weakened check is not authorized by this decision.

## Alternatives

Separate Swift and Windows implementations duplicate the installation logic.
A Chromium wrapper adds an unnecessary browser runtime. A custom serial
protocol would duplicate the maintained Espressif implementation. A Python
script alone does not meet the accepted end-user experience.

## References

[Espressif integration](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/scripting.html),
[Qt deployment](https://doc.qt.io/qtforpython-6/deployment/deployment-pyinstaller.html),
[PyInstaller packaging](https://pyinstaller.org/en/stable/operating-mode.html).
