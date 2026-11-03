<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# macOS Development Setup

Everything needed to build, test and check this firmware on macOS, in the order a
fresh machine needs it. Every command here has been run in this repository.

macOS is the only documented platform (Bootstrap Spec §4.1). Apple Silicon is what
the project is developed and validated on.

---

## 1. Prerequisites

**Xcode command-line tools** — the host C++ compiler, `git`, and `clang-format`.

```bash
xcode-select --install
```

**Homebrew packages** — CMake, and LLVM for `clang-tidy`.

```bash
brew install cmake llvm
```

`llvm` is needed for `clang-tidy` specifically: Apple's toolchain ships
`clang-format` but **not** `clang-tidy`. Homebrew installs LLVM keg-only, so it
stays out of your `PATH` and cannot shadow the system compiler — the project's
check scripts look for it under `/opt/homebrew/opt/llvm/bin` on their own.

Python 3 comes with Homebrew or Xcode; the project's tools use only the standard
library.

**Node and a browser** — needed only for the browser suite (§4), not for building
the firmware or running the host tests.

```bash
brew install node
```

Google Chrome or Chromium, installed normally, is the browser. Nothing is
downloaded by the suite and there is no npm dependency: it runs on Node's built-in
modules and drives the browser you already have. CI pins Node to **22.23.2** and
requires **Chrome 112 or newer** (the floor for `--headless=new`); the suite prints
the Node and browser versions it resolved on every run, so a mismatch with CI is
visible in the first three lines of output. See
[ADR-0006](../decisions/0006-browser-regression-suite-toolchain.md).

Verify:

```bash
cc --version
cmake --version
/opt/homebrew/opt/llvm/bin/clang-tidy --version
node --version
```

---

## 2. ESP-IDF v5.5.4

The version is pinned exactly and enforced in three places (ADR-0001). Nothing
else is supported; the build refuses to configure with a different version.

Clone at the tag:

```bash
mkdir -p ~/Developer/esp
git clone -b v5.5.4 --recursive https://github.com/espressif/esp-idf.git ~/Developer/esp/esp-idf
```

Install the toolchain for the ESP32-S3 only:

```bash
cd ~/Developer/esp/esp-idf && ./install.sh esp32s3
```

`~/Developer/esp/esp-idf` is the location Bootstrap Spec §16.1 documents. Any path
works; the commands below assume this one.

### Activating it

ESP-IDF must be activated in **every new shell** before any firmware command:

```bash
. ~/Developer/esp/esp-idf/export.sh
```

Confirm — this must print exactly `ESP-IDF v5.5.4`:

```bash
idf.py --version
```

Activation does not persist; a new terminal tab needs it again. Host tests,
formatting and static analysis do **not** need it. Only firmware builds and
flashing do.

---

## 3. Build the firmware

Three profiles (Bootstrap Spec §10.1). Run the commands shown here from the
repository root.

```bash
./firmware/tools/build.sh debug
```

```bash
./firmware/tools/build.sh hardware-validation
```

```bash
./firmware/tools/build.sh release
```

| Profile | Purpose |
|---|---|
| `debug` | daily development; readable logs, assertions |
| `hardware-validation` | board bring-up; compiles the validation flows and the LVGL validation scene |
| `release` | product image; requires a clean Git tree |

Each profile builds into `firmware/build/<profile>/` with its own `sdkconfig`, so
switching profiles never reconfigures another one.

**`release` fails on a dirty working tree**, by design (§14.4), and the error says
so. Commit or stash first; use `debug` or `hardware-validation` while you work.

Inspect what a build produced:

```bash
./firmware/tools/print-build-info.py --profile debug
```

---

## 4. Run the tests

No ESP-IDF needed.

```bash
./firmware/tools/test-host.sh
```

With AddressSanitizer and UndefinedBehaviorSanitizer (§12.5):

```bash
./firmware/tools/test-host.sh --sanitize
```

The two modes use separate build directories, so switching between them does not
reconfigure in place. LeakSanitizer is not used — Apple Clang does not implement it
on macOS.

The first run downloads GoogleTest and needs network access. Later runs do not.

### The display renderer

UI changes also need the [LVGL renderer suite and frame inspection](ui-rendering.md).
It renders the production screen code on the host, separately from the logic
tests and from the browser-based setup page below.

### The browser suite

The provisioning page is a product surface, and the host tests can only read its
source — they prove a correction is still written down, not that it works. This
serves the real `index.html`, `style.css` and `app.js` and drives them in headless
Chrome at a genuine 375×812 viewport, asserting DOM values *and* computed styles.

```bash
./firmware/tools/test-browser.sh
```

22 scenarios, about a minute and a half, plus five iPhone-width screenshots under
`firmware/build/browser-shots/`. The run begins by resolving the browser, refusing
one older than Chrome 112, proving it actually starts, and printing what it found;
if that fails it says so in seconds rather than timing out 22 times. To use a
different browser:

```bash
PRD_CHROME="/Applications/Chromium.app/Contents/MacOS/Chromium" ./firmware/tools/test-browser.sh
```

Run it whenever you change anything under
`firmware/components/provisioning/assets/`. CI runs it too, on Ubuntu, in its own
`Browser Tests` job.

---

## 5. Run the checks

Both must pass before a change is proposed.

```bash
./firmware/tools/check-format.sh
```

```bash
./firmware/tools/check-tidy.sh
```

`check-format.sh` prints a diff for anything that has drifted. To apply it:

```bash
./firmware/tools/check-format.sh --fix
```

CI pins both tools to **22.x**. A different major version of clang-format produces
different output, so if the check disagrees with CI, compare versions first. To
match CI exactly:

```bash
pipx install "clang-format==22.1.8" && pipx install "clang-tidy==22.1.8"
```

Both scripts honour `CLANG_FORMAT` and `CLANG_TIDY` if you need to point them at a
specific binary.

---

## 6. Package artifacts

```bash
./firmware/tools/package-firmware.py --profile release
```

Writes the artifact set to `firmware/build/release/artifacts/` — binaries,
`manifest.json`, `SHA256SUMS`, `build-info.txt`, `size-report.txt` — plus a
`.tar.gz` named with the profile and version. Output is deterministic: the same
build packaged twice produces an identical archive.

Packaging also scans the shipped binaries for credential markers and absolute paths,
and fails if it finds any (§13.5). Findings in the linker map and build logs are
reported as notes rather than failures — those files record build paths by design
and are never shipped.

---

## 7. Common problems

**`idf.py: command not found`** — ESP-IDF is not activated in this shell. Run the
`export.sh` line from §2.

**`ESP-IDF v5.5.4 required; found ...`** — a different ESP-IDF is active. Check
`idf.py --version`, then activate the pinned one in a clean shell.

**`Release builds require a clean Git working tree`** — expected; see §3.

**`Release tag does not match firmware/version.txt`** — HEAD carries a version tag
that disagrees with `version.txt`. `version.txt` is canonical (§14.1): update it, or
retag the commit.

**`clang-tidy not found`** — install Homebrew LLVM (§1). Apple's toolchain does not
include it.

**clang-tidy reports missing standard headers** — the check script passes Apple's
SDK path automatically. If you invoke clang-tidy by hand, add
`--extra-arg=-isysroot --extra-arg="$(xcrun --show-sdk-path)"`; without it,
clang-tidy misreads types and reports confident nonsense.

**Host tests fail to configure on a first run** — GoogleTest is fetched over the
network; check connectivity.

**A stale `firmware/sdkconfig` at the firmware root** — left by an older
`set-target` run. Harmless and Git-ignored; delete it.

---

## Next

To flash a physical board, see [first-flash.md](first-flash.md).
