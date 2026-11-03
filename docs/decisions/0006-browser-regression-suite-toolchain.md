<!-- SPDX-FileCopyrightText: 2026 Mykola Fiantsev -->
<!-- SPDX-License-Identifier: CC-BY-4.0 -->

# ADR-0006: Run the browser regression suite in CI on system Chrome and a pinned Node

## Status

Accepted

## Date

2026-08-18

## Context

The provisioning page is a product surface: the device serves `index.html`, `style.css` and
`app.js` from flash, and the user's phone executes them. Four defects in that page each survived
review and reached physical acceptance before being found — `hidden` doing nothing on an SVG
element, a success CSS class surviving into an error summary, three POSTs sent where one was
intended, and a page left polling for ever after the device restarted mid-attempt. None of them is
visible in the asset text. Assertions over the source read what the code *intended*; only a
browser reports what it *did*.

`firmware/test/host/provisioning/served_page_tests.cpp` reads the three real assets and asserts
that each correction is still present in them. Its own header states the limit: there is no
JavaScript engine in that build, so it proves the page code still contains the fix, not that the
fix works. The behavioural half is `firmware/test/browser/` — a deterministic suite that serves
the real assets over Node's built-in HTTP server, drives them in headless Chrome at a genuine
375×812 viewport, and asserts both DOM values and computed styles.

That suite was committed but referenced by no workflow, so it ran only when a developer typed the
command. That is the shape of finding F2 — a gate that runs nowhere but a terminal is not an
enforced invariant — which the `source-gates` job was added to correct for two other checks.

Firmware Architecture Specification §22 requires every dependency to be pinned, license-compatible,
actively maintained, justified and upgrade-reviewed, and lists "general web frameworks" among the
libraries to avoid. Bootstrap Spec §13.2 defines six CI jobs and does not contemplate a browser
one; §12.4 nonetheless places static and behavioural checking in CI scope, and `source-gates`
already set the precedent for a job that is documented as outside the numbering.

What was not known when this was queued: whether the GitHub-hosted runners provide a browser at
all. They do. `ubuntu-24.04` ships Google Chrome and Chromium; `macos-15` ships Google Chrome. Both
ship Node 22.

## Decision

The browser suite runs in CI as a **dedicated `browser-tests` job on `ubuntu-24.04`**, documented in
the workflow as not numbered in §13.2, following the `source-gates` convention.

1. **A separate job, not a step in `host-tests`.** A page defect reported under a job named "Host
   Tests (macOS)" names the wrong thing; the two suites share no toolchain, and separate jobs run in
   parallel.

2. **Ubuntu is sufficient.** Every assertion the suite makes is font-independent: computed colours,
   `display`, declared `font-size`, two comparisons between CSS-positioned rectangles, `innerWidth`,
   and horizontal overflow. The 375×812 viewport comes from a same-origin iframe, not from
   `--window-size`, so it is exact on any platform — Chrome clamps `--window-size` to 500 px on
   macOS, and the iframe exists because that clamp once cropped a screenshot and was misread as a
   page overflow. This is behavioural coverage, not pixel-level visual regression, so matching iOS
   San Francisco font metrics buys nothing, and macOS runners bill at ten times the rate.

3. **Node is pinned exactly, to 22.23.2**, via `actions/setup-node` and a `NODE_VERSION` workflow
   variable, beside the existing `CLANG_TOOLS_VERSION` pin. 22.23.2 is the current Node 22 LTS
   ("Jod") and is also what `ubuntu-24.04` preinstalls, so it resolves from the runner's tool cache
   rather than being downloaded. **No npm dependency is added**; the suite imports only `node:http`,
   `node:fs`, `node:os`, `node:path` and `node:child_process`, so there is no `package.json`, no
   lockfile and nothing to audit.

4. **The browser is the one the runner image provides, with a declared minimum version rather than
   a pin.** `firmware/test/browser/run.mjs` owns the whole requirement, so the binary that is
   checked is always the binary that runs: it resolves an executable from a per-platform candidate
   list or from `PRD_CHROME`, refuses anything below **Chrome 112** (the floor for `--headless=new`,
   which every launch uses), proves the browser actually starts by watching for a complete document
   on stdout, and prints the resolved path and full version string into the log on every run. On
   Linux the candidate order prefers the packaged `google-chrome`: Chromium's documentation records
   that the official installation under `/opt/google/chrome` carries the AppArmor profile permitting
   the unprivileged user namespaces its sandbox needs on Ubuntu 23.10+, which a self-downloaded
   build does not.

5. **No Playwright, Puppeteer, Selenium, browser-download action or general web framework.** The
   suite drives Chrome through command-line flags and a script served into the page; that is
   sufficient for the assertions it makes, and §22 rules out the alternative.

## Consequences

- The four classes of defect above cannot return silently. A change to the served page is now
  behaviourally checked on every pull request rather than at the next physical acceptance.
- CI gains an external tool requirement it did not have. The Node half is pinned and reproducible.
  The browser half is **not** reproducible across time: GitHub updates the runner image's Chrome on
  its own schedule, so the suite is verified against a moving browser. That cost is accepted
  deliberately — the alternative is downloading and pinning a second browser on every run, which
  §22's "avoid general web frameworks" and the suite's no-dependency premise both argue against —
  and it is mitigated by enforcing a minimum version and recording the actual one in every log.
- A browser regression that is genuinely Chrome's, not ours, will surface as a red CI run on a
  commit that did not touch the page. The printed version string is what makes that diagnosable.
- The job is Linux-only, so a macOS-specific rendering defect would not be caught by CI. It would
  still be caught by the same suite run locally, which is the documented developer command.
- `PRD_CHROME` keeps working, so a developer can point the suite at any browser without editing it.

## Alternatives Considered

**`macos-15`, alongside `host-tests`.** Rejected on cost and on evidence. It was the natural
candidate while it was still an open question whether the Ubuntu image had a browser; it does. The
argument for macOS was that the page's `system-ui, -apple-system, …` stack resolves to the face
iOS Safari uses, so the layout under test resembles the real device. That argument does not reach
this suite's assertions, all of which were checked and none of which depends on font metrics. Ten
times the billing rate for coverage that is identical is not a trade worth making.

**A step inside the existing `host-tests` job.** Rejected. It would save one runner start-up and
misattribute every failure, which is exactly the reasoning already recorded for `source-gates`.

**Hard-pinning Chrome** with a third-party setup action or a Chrome for Testing download. Rejected.
It buys genuine reproducibility, and it costs a third-party action in the supply chain, a browser
download on every run, and — per Chromium's own documentation — a binary *outside* the AppArmor
profile that lets the sandbox start on Ubuntu 24.04, which is how projects using downloaded
Chromium end up passing `--no-sandbox`. A minimum version plus a recorded actual version is the
proportionate reading of §22 for a test-only tool.

**Playwright or Puppeteer.** Rejected. Both would give better ergonomics than command-line flags
and both bring an npm dependency tree and a managed browser download into a repository that has
neither. §22 lists general web frameworks among the libraries to avoid, and the existing suite
already produces the coverage.

**Leaving the suite developer-run and documenting it.** Rejected — that is the status quo this ADR
exists to end, and finding F2 already recorded why documentation is not enforcement.

## Validation / Follow-up

- The suite passes locally on macOS: **22 scenarios, 0 failed**, every scenario reporting
  `viewport 375x812` with `scrollWidth == clientWidth == 375`, plus five screenshots.
- The tool requirement is self-checking, and each check was verified by observing its failure
  rather than by reading it: a `PRD_CHROME` that is not executable, a browser reporting a version
  below the floor, and a browser that never produces a document each stop the run with a named
  reason, before any scenario starts.
- Screenshot teardown is deterministic. Each capture ends when the PNG's size stops changing:
  measured at **1.93 s** per capture against a 20 s budget, taking the suite from 3 min 00 s to
  **1 min 32 s**. Reaching the deadline is a **failure**, not the normal path, and the log prints
  the outcome, elapsed time and byte count for every capture.
- That failure path is negative-controlled. With the deadline forced to 50 ms and a stale PNG
  pre-placed at all five capture paths — which the previous "does the file exist" rule would have
  accepted — the run reports five `deadline` failures at `-1B` and exits non-zero. The target is
  deleted before each capture, so a leftover image cannot pass for a fresh one, and teardown
  trouble cannot convert into a silent pass.
- Only the pushed workflow run can confirm the runner-side facts: that `ubuntu-24.04` resolves
  `/usr/bin/google-chrome`, that its sandbox starts under AppArmor without `--no-sandbox`, that
  `actions/setup-node` supplies 22.23.2 from the tool cache, and the job's wall-clock cost.
- Open: the minimum Chrome version is a floor chosen for `--headless=new`. If a future scenario
  needs a newer capability, raise `CHROME_MIN_MAJOR` in `run.mjs` and say why here.
