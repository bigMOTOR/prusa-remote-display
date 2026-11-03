<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Desktop display installer

Prefer an application to installing through Chrome or Edge? This is the Mac
and Windows alternative being built for the display. The packaged release will
include everything it needs — no Python installation or particular browser.

Right now it is a developer preview, not a public app download. You can try the
window and its themes, but USB access and firmware installation are disabled.
The preview has no trusted publisher key and is not signed for public distribution.

For the whole build, see [Build and set up your display](../docs/get-started.md).
Have feedback on the Mac preview? See [how to help](../CONTRIBUTING.md), or return
to the [project overview](../README.md). The steps below are for developers;
they will not be required of someone installing a packaged release.

Appearance follows the system by default. The header also offers **Dark** and
**Light** overrides without changing the computer's settings. The artwork in
`assets/app-icon.png` is bundled as a native Mac/Windows application icon.

## Local development

Use Python 3.12 in a separate environment, not ESP-IDF's environment. Install
`bootstrap.lock` with hash enforcement first, then install `requirements.lock`
with both hash enforcement and `--no-build-isolation`. This pins the setuptools
backend used to build esptool instead of downloading a floating build dependency.
For example, use `python -m pip install --require-hashes -r desktop-installer/bootstrap.lock`,
then `python -m pip install --no-build-isolation --require-hashes -r desktop-installer/requirements.lock`.
The universal lock includes Windows and Mac platform markers. Regenerate it only
as a reviewed dependency change using the command recorded in its header.

Run `python -B desktop-installer/app.py --preview` for the real Qt window without
any serial enumeration. Run deterministic tests with
`QT_QPA_PLATFORM=offscreen python -B -m unittest discover -s desktop-installer -v`.
The separate `firmware/test/signing` suite includes the desktop signature and
initial-image tests with disposable keys and no executable firmware fixtures.

Build on the target OS using `python -B desktop-installer/build.py /fresh/output`.
The generated Mac app or Windows folder includes the runtime; it is not a Python
script for end users. Windows must be built and tested on Windows. The build
records dependencies and supplies no distribution signing, notarization or publisher key.
PyInstaller's local Mac ad-hoc signature is not a Developer ID signature.
The dedicated CI workflow runs the complete desktop and signing suites on Linux
for relevant pull requests and main pushes. Native Mac/Windows tests and preview
builds run only when the owner starts **Desktop Installer → Run workflow** in
GitHub Actions; that manual run also repeats Linux checks. It does not access USB,
upload artifacts or publish a release. Superseded runs on the same ref are cancelled.
The workflow pins uv 0.11.6 and managed Python 3.12.13, activates an isolated
runner-temp environment and installs the same hashed locks without build isolation.
It does not rely on setup-python having a binary for that Python patch version.
A workflow definition or Linux PASS is not proof that native jobs have succeeded.
Local checks so far used Homebrew CPython 3.12.13, including the fresh uv-installed
dependency environment; they did not use uv-managed standalone CPython. The first
manual three-platform run must validate that CI interpreter with Qt and native
PyInstaller packaging before this CI path can be relied on for a release. If it
fails, investigate the actual failure and review the correction; do not silently
substitute another Python patch version or weaken the checks.

## Maintainer release checklist

Before every application release, and after changes to packaging, runtime or
dependencies, native icons/resources, OS-specific paths, serial-port handling,
or the desktop CI workflow, request a manual native run on the exact candidate.
The project PM must remind the owner at that checkpoint, not leave this step to
memory. Ordinary platform-neutral edits use automatic Linux checks; when in doubt,
request native validation before calling the affected delivery verified.

1. Freeze and push the reviewed candidate commit, with the owner's authorization.
2. Open **Actions → Desktop Installer → Run workflow** and select its branch.
   Do not push to the candidate branch while native validation runs: push and manual
   runs share a concurrency group, so a new push can cancel the manual run.
3. Require successful Linux, macOS and Windows jobs. Record the run URL and exact
   full commit SHA and verify that the run's head SHA equals the frozen candidate SHA;
   an older green run or a cancelled/skipped native job is insufficient.
4. If that candidate changes afterward, review the change and repeat native
   validation before publication. Native CI is necessary, not sufficient: it does
   not establish distribution signing, trusted publisher identity or real USB behavior.

The PM will give the owner this concrete reminder when needed:
“Run Desktop Installer → Run workflow for the candidate branch; wait for all three
jobs to pass before the release.” No scheduled or manual native run is dispatched
automatically on the owner's behalf.

## Installation behavior

An installation-enabled distribution must ship an independently trusted public
verification key, not a key obtained from the downloaded firmware. The current
builder deliberately does not supply one. Discovery requires official stable
immutable `vX.Y.Z` firmware assets for all four initial regions, manifest,
checksums and flash arguments, then resolves the exact release commit.

Application releases use the separate tag prefix `installer-vX.Y.Z` (for
example, `installer-v0.1.0`). When creating an application release in the same
repository, set GitHub's `make_latest` to `false` and verify that the latest
release endpoint still returns the intended immutable firmware `vX.Y.Z`.
Do not use firmware tags for app-only assets. Publication remains an owner action.

The app checks board, layout, hashes and the embedded application signature,
preserves the signed application when merging, and requires board, USB port and
erase confirmation before calling esptool. It never requests credentials or
writes eFuses or bypasses esptool security checks. Initial installation replaces
display settings; normal OTA updates are a separate device feature. Successful
writing is not proof that the display has booted or been physically validated.

GitHub HTTPS authenticates bootloader/data delivery separately from the signed
application. Bundle checksums are not themselves publisher signatures. The
original release verifier is bundled unchanged, not copied and maintained twice.

## Distribution gates and storage

Before publication, review dependencies/advisories, provide full dependency
licenses and corresponding source, complete Mac signing/notarization and
Windows packaging verification, configure independently trusted publisher
identity, and verify initial USB boot/unplug/recovery on the real board with
explicit authorization. None is established by a GUI screenshot or mocked write.

Use one temporary environment and one fresh build directory, not repeated full
repository copies. Each build is limited to 512 MiB; initial development scratch
budget is 2 GiB including the dependency cache. Preserve logs and provenance on
unexpected failures. Remove only specifically verified disposable outputs after
saving required evidence; never purge CAD, firmware builds or signing material.
