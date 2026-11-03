<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# Contributing

Thanks for taking a look. You can help with this project without being an
embedded developer: a confusing instruction, an unclear screen or a missing
assembly picture is useful feedback too.

## Feedback without writing code

Open a GitHub issue in this repository and tell me what you were trying to do,
what you expected and where you got stuck. Include your operating system and
the relevant app or firmware version if you know it. A screenshot can help;
please remove Wi-Fi passwords, printer keys and other private details first.

Have a Mac? The [desktop installer preview](desktop-installer/README.md) is a
good place to help with readability, window layout and setup instructions.
Trying the window still needs Python and Terminal setup; USB access and
installation are disabled.
You can also review the [build guide](docs/get-started.md) without installing
anything or buying parts. If repository issues are not accessible to you yet,
the public feedback route will become available when the project is opened.

## Code and documentation changes

Small fixes and clearer wording are welcome. Describe the problem and the
change in your pull request; for a large redesign, start with an issue so we
can discuss the direction before you invest time in it.

The firmware uses ESP-IDF and keeps normal operation on your local network.
The checks below help keep changes maintainable and safe for the reference
hardware. For the product rather than development instructions, start at the
[project overview](README.md).

## Design boundaries

These decisions shape the project. A change to them needs a design discussion,
not an incidental change in a pull request:

- **Arduino or PlatformIO.** The build is ESP-IDF v5.5.4, pinned (ADR-0001).
- **Cloud dependencies or an external backend.** Local-first is the product.
- **Blocking delays in application code.** Tasks block on the scheduler, not on
  spin loops.
- **C++20 or later.** C++17 is the binding standard.

## Getting set up

[docs/development/macos-setup.md](docs/development/macos-setup.md) — toolchain,
ESP-IDF, builds, tests, checks. macOS is the documented platform.

[docs/development/first-flash.md](docs/development/first-flash.md) — flashing the
board over USB-C, and how to recover it.

## Before you propose a change

Start with the four core code gates:

```bash
./firmware/tools/check-format.sh
```

```bash
./firmware/tools/check-tidy.sh
```

```bash
./firmware/tools/test-host.sh
```

```bash
./firmware/tools/test-host.sh --sanitize
```

Run the repository-wide source and tooling gates as well. The first command below
is a static source check only; it does not contact or configure hardware.

```bash
./firmware/tools/check-efuse-safety.sh
./firmware/tools/check-tokens.sh
./firmware/tools/check-credential-logs.py
./firmware/tools/check-licensing.sh
./firmware/tools/test-tools.sh
python3 -m unittest discover -s firmware/test/configuration -p 'test_*.py' -v
```

If you touched the provisioning page — `firmware/components/provisioning/assets/` —
also run the browser suite, which drives the real page in a real browser at phone
width. It needs Node and an installed Chrome or Chromium; see
[macos-setup.md](docs/development/macos-setup.md) and
[ADR-0006](docs/decisions/0006-browser-regression-suite-toolchain.md).

```bash
./firmware/tools/test-browser.sh
```

And the profiles you touched must build clean:

```bash
./firmware/tools/build.sh debug
```

OTA adapter changes also run against the pinned ESP-IDF headers:

```bash
bash ./firmware/tools/test-ota-adapter.sh
```

Changes to firmware signing or signed packaging require the separate signing
suite inside the activated ESP-IDF Python environment. It uses disposable test
keys and never a publisher key:

```bash
python -m unittest discover -s firmware/test/signing -p 'test_*.py' -v
```

CI runs the applicable gates, all firmware profiles and packaging, and — on tags
or manual dispatch — the reproducibility check. A local pass should mean a CI
pass; if it does not, compare your tool versions and the exact workflow entry
point rather than substituting a similar command.

## Code expectations

**Formatting is mechanical.** `.clang-format` decides; run `--fix` and move on.
Never reformat code you did not otherwise change — it buries the real diff.

**Warnings are errors in spirit.** Project code builds warning-free. A new warning
is fixed, not tolerated. If a suppression is genuinely needed it is local, scoped as
narrowly as possible, and carries its reason — see the ESP-IDF adapter translation
units for the pattern.

**Static analysis findings are fixed or suppressed with a reason.** `.clang-tidy`
runs with `WarningsAsErrors: '*'`. If you disagree with a check, argue it in the
config with a documented rationale rather than adding a bare `NOLINT`.

**Logic belongs where it can be tested.** Product logic goes in translation units
that include no ESP-IDF header, so the host suite compiles the same source the
firmware does. ESP-IDF contact is confined to adapters. ADR-0003 explains the
boundary and why it is drawn at the translation unit rather than the directory.

**Comments explain why, not what.** The code says what it does. Record the reason
it does it that way, especially when the obvious alternative was tried and
rejected — a comment saying "the documented approach does not work here, measured"
saves the next person a day.

## Architecture

Start with the [architecture map](docs/development/architecture.md) for component
roles and data flow. For UI changes, run the separate
[LVGL renderer checks and inspect the frames](docs/development/ui-rendering.md);
the host logic tests alone do not render the screen.

The accepted firmware architecture is authoritative. Do not merge components, and
do not add layers without cause. If you believe the architecture must change: stop,
explain why, and get agreement before writing code.

Component boundaries that are checked mechanically and must stay true:

- only `ui` includes LVGL headers, and only its single task calls LVGL;
- only `display/src/display.cpp` calls esp_lcd;
- pin numbers exist only inside `board`;
- no LVGL type appears in any public header.

## Architecture Decision Records

A decision that is expensive to reverse gets an ADR:
[docs/decisions/](docs/decisions/). Copy `0000-template.md`, take the next number,
and record the costs alongside the benefits. If the decision rests on a
measurement, give the measurement.

## Hardware changes

Anything touching the panel, backlight, button or renderer needs validation on real
hardware, recorded in
[hardware/validation/](hardware/validation/) with evidence.

Two lessons from Milestone 1 that are worth inheriting rather than repeating:

- **Look at the screen.** A colour-format defect once passed every counter, every
  timing figure and every log line while rendering all greys and whites blue. For a
  display product, a photograph is evidence and instrumentation is not a substitute.
- **Believe the API note.** Two hardware faults in one phase were both documented in
  headers that were already open.

Evidence rules: redact machine paths, usernames and serial numbers; never commit
credentials, Wi-Fi names or real printer payloads; keep every evidence file
referenced from the validation record.

## Versioning and releases

`firmware/version.txt` is canonical. A release tag is `v` plus that version
exactly; a mismatch fails the release build with a message telling you which to
change.

Release builds refuse a dirty tree. That gate is not to be weakened to make a
build or a CI job pass — a release nobody can trace to a commit is worse than no
release.

The owner-operated signing, signed-bundle preflight and immutable publication
boundary are documented in [docs/releasing.md](docs/releasing.md). A normal CI
artifact is unsigned and is not the live OTA image.

## Licensing

The licence depends on which area of the repository you are changing:

| Area | Licence |
|---|---|
| Firmware and software tools | `GPL-3.0-only` ([LICENSE](LICENSE)) |
| Hardware design files | `CERN-OHL-S-2.0` ([LICENSE-hardware](LICENSE-hardware)) |
| Documentation | `CC-BY-4.0` ([LICENSE-documentation](LICENSE-documentation)) |

[LICENSING.md](LICENSING.md) has the full scope. New project-owned files carry
two tags at the top, in the comment syntax of the file:

```c
// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
```

The copyright line is not decoration. GPL-3.0's own *How to Apply These Terms*
appendix asks for it in each source file, and CC-BY-4.0's attribution term is
unsatisfiable if no creator is named anywhere.

**Never add these tags to third-party material.** A vendored typeface, an
upstream licence text and a serial capture that is evidence all keep exactly the
notices they came with. `firmware/tools/licensing-manifest` records which paths
are project-owned and which are exceptions, with a reason for each, and
`check-licensing.sh` enforces it:

```bash
./firmware/tools/check-licensing.sh
```

A file matching no rule in the manifest fails the check. That is deliberate:
adding a file type or third-party material should be a decision recorded there,
not an accident.

Contribution licensing is **inbound = outbound**: your contribution is licensed
under whichever of the three licences already applies to the area you changed.
By contributing you confirm you have the right to submit it under that licence.
