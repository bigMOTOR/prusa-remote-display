// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
//
// Orchestrates the browser-flow suite: one headless Chrome run per scenario against the real
// served page, plus iPhone-viewport screenshots.
//
// THE BROWSER IS A DECLARED TOOL REQUIREMENT, NOT AN ASSUMPTION. Nothing is downloaded and no
// npm package is used — the suite runs on Node's built-in modules and a browser that is already
// installed, which is why it adds no dependency to the firmware or to CI's firmware jobs. What
// it does need is stated and checked here before a single scenario runs: an executable browser,
// a version new enough for the flags used below, and proof that it actually starts. A missing or
// unusable browser has to say so in seconds rather than surface as 22 identical timeouts.

import { execFileSync, spawn } from "node:child_process";
import { mkdtemp, mkdir, rm } from "node:fs/promises";
import { accessSync, constants, statSync } from "node:fs";
import { platform, tmpdir } from "node:os";
import { join } from "node:path";
import {
  arm,
  start,
  submissions,
  updateSubmissions,
  awaitResult,
  polls,
} from "./serve.mjs";

// Every launch below uses `--headless=new`, which is available from Chrome 112. This is a FLOOR,
// not a pin: pinning the browser would mean downloading a second one, which is precisely the
// dependency this suite exists without. The resolved version is printed on every run, so the log
// records what actually ran even though the runner image moves on its own schedule.
const CHROME_MIN_MAJOR = 112;

// Preferred order — and on Linux the order is deliberate. Ubuntu 23.10+ restricts unprivileged
// user namespaces through AppArmor, and Chromium's own documentation records that official Chrome
// installations under /opt/google/chrome carry a profile permitting them while a self-downloaded
// build does not. The packaged google-chrome is therefore the one that starts sandboxed on a
// GitHub ubuntu-24.04 runner.
const CHROME_CANDIDATES = {
  darwin: [
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
  ],
  linux: [
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/opt/google/chrome/chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
  ],
};

// Resolved once, at the start of the run, and reported before anything else happens.
let CHROME = "";

const PORT = Number(process.env.PRD_BROWSER_PORT ?? 8231);
const SHOTS = process.env.PRD_SHOT_DIR ?? join(process.cwd(), "firmware", "build", "browser-shots");

// iPhone-sized. The failures that reached the owner were all mobile-only.
const VIEWPORT = "375,812";

// Bounds, and both are BACKSTOPS rather than the normal path. The launch proof finishes as soon
// as the browser prints a document, and a capture finishes as soon as its PNG stops growing;
// reaching either deadline means something is wrong, which is the point — see `capture()`.
const LAUNCH_DEADLINE_MS = 20000;
const CAPTURE_DEADLINE_MS = 20000;
const CAPTURE_POLL_MS = 100;
const CAPTURE_MIN_BYTES = 1024;

const accepted = (stage, extra = {}) => ({ accepted: true, stage, ...extra });

// Per scenario: what /api/status returns over successive polls, what /api/setup answers, and
// optionally the status script to switch to once a stage is accepted.
const SCENARIOS = [
  ...["printer-unreachable", "printer-unauthorized", "printer-unsupported", "printer-unusable-response"].map((reason) => ({
    name: reason,
    reason,
    statuses: ["editing"],
    setup: { wifi: accepted("wifi"), printer: accepted("printer") },
    after: { wifi: ["validating", "wifi-validated"], printer: ["validating", "failed"] },
    expectPosts: 3,
    shot: true,
  })),
  { name: "wifi-initial", statuses: ["editing"], setup: {} },
  {
    name: "wifi-empty",
    statuses: ["editing"],
    setup: { wifi: { accepted: false, stage: "wifi", wifiSsid: "missing", wifiPassword: "ok" } },
  },
  {
    name: "wifi-invalid",
    statuses: ["editing"],
    setup: {
      wifi: { accepted: false, stage: "wifi", wifiSsid: "ok", wifiPassword: "too-short" },
    },
  },
  {
    name: "wifi-validating",
    statuses: ["editing", "validating", "validating", "validating", "validating"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "validating", "validating", "validating", "validating"] },
  },
  {
    name: "wifi-failure",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-unreachable",
  },
  {
    name: "wifi-retry",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    // Attempt 1 fails; the user edits and resubmits; attempt 2 succeeds with nothing else
    // changed. This is the shape the owner hit on hardware, and D-6 requires it to work.
    after: {
      wifi: [
        ["validating", "failed"],
        ["validating", "wifi-validated"],
      ],
    },
    reason: "wifi-unreachable",
  },
  {
    name: "wifi-success",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "wifi-validated"] },
  },
  {
    name: "printer-initial",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "wifi-validated"] },
    shot: true,
  },
  {
    name: "printer-empty",
    statuses: ["editing"],
    setup: {
      wifi: accepted("wifi"),
      printer: {
        accepted: false,
        stage: "printer",
        printerHost: "missing",
        printerPort: "missing",
        printerApiKey: "missing",
      },
    },
    after: { wifi: ["validating", "wifi-validated"] },
    shot: true,
  },
  {
    name: "printer-missing-key",
    statuses: ["editing"],
    setup: {
      wifi: accepted("wifi"),
      printer: {
        accepted: false,
        stage: "printer",
        printerHost: "ok",
        printerPort: "ok",
        printerApiKey: "missing",
      },
    },
    after: { wifi: ["validating", "wifi-validated"] },
    shot: true,
  },
  {
    // ⛔ CHECKPOINT 3d. A validated body the device could not hand to its application task. The
    // status and the body are byte-for-byte what provisioning::kUnavailableBody and the 503
    // responder send, so the page is driven against the real answer rather than a paraphrase.
    name: "wifi-unavailable-503",
    statuses: ["editing"],
    setup: { wifi: { status: 503, body: { accepted: false, error: "unavailable" } } },
    // Two: the submission that gets the 503, and the retry that proves recovery is possible.
    expectPosts: 2,
    expectNoPollsAfterLastPost: true,
  },
  {
    name: "printer-unavailable-503",
    statuses: ["editing"],
    setup: {
      wifi: accepted("wifi"),
      printer: { status: 503, body: { accepted: false, error: "unavailable" } },
    },
    after: { wifi: ["validating", "wifi-validated"] },
    expectPosts: 2,
    expectNoPollsAfterLastPost: true,
  },
  {
    name: "printer-all-valid",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi"), printer: accepted("printer") },
    after: { wifi: ["validating", "wifi-validated"], printer: ["succeeded"] },
  },
  {
    name: "summary-success-then-error",
    statuses: ["editing"],
    setup: {
      wifi: accepted("wifi"),
      printer: {
        accepted: false,
        stage: "printer",
        printerHost: "ok",
        printerPort: "missing",
        printerApiKey: "missing",
      },
    },
    after: { wifi: ["validating", "wifi-validated"] },
    shot: true,
  },
  {
    name: "edit-clears-only-edited",
    statuses: ["editing"],
    setup: {
      wifi: accepted("wifi"),
      printer: {
        accepted: false,
        stage: "printer",
        printerHost: "missing",
        printerPort: "missing",
        printerApiKey: "missing",
      },
    },
    after: { wifi: ["validating", "wifi-validated"] },
  },
  {
    name: "whitespace",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi"), printer: accepted("printer") },
    after: { wifi: ["validating", "wifi-validated"], printer: ["succeeded"] },
  },
  {
    name: "double-submit",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "validating", "wifi-validated"] },
    expectPosts: 1,
  },
  {
    name: "polling-race",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    // "editing" AFTER the POST is the race: the device has not yet moved the phase.
    after: { wifi: ["editing", "editing", "validating", "wifi-validated"] },
  },
  // --- The Wi-Fi failure / recovery matrix, and the invariant behind it -------
  //
  // Every one of these is a state the owner reached on hardware or asked to be proven. The last
  // is the one that mattered most: on 2026-08-18 the device panicked mid-attempt and rebooted,
  // and the page — which cannot tell "not started yet" from "forgot everything" — sat on
  // Checking… for ever. A restarted server mints a new session token, so that is the signal.
  {
    name: "wifi-wrong-password-bounded",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "validating", "failed"] },
    reason: "wifi-unreachable",
  },
  {
    name: "wifi-wrong-then-corrected",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: {
      wifi: [
        ["validating", "failed"],
        ["validating", "wifi-validated"],
      ],
    },
    reason: "wifi-unreachable",
  },
  {
    name: "wifi-wrong-a-then-wrong-b",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: {
      wifi: [
        ["validating", "failed"],
        ["validating", "failed"],
      ],
    },
    reason: "wifi-unreachable",
    expectPosts: 2,
  },
  // --- The owner's DriverStalled ruling, as the user meets it ----------------
  //
  // ⛔ THE DISPLAY HAS QUARANTINED ITS STATION. It accepted a driver operation whose documented
  // completion never arrived, or it lost the record of one, so it will issue no further station
  // operation until somebody restarts it. The page must say that and must ask for the one action
  // that works — not "check the password", which was never tested, and not "try again", which
  // could not be judged either.
  {
    name: "wifi-driver-stalled",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-driver-stalled",
  },
  {
    name: "wifi-evidence-lost",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-evidence-lost",
  },
  // ⛔ CHECKPOINT 3e's RESTORE FAILURE. A restore WAS attempted against the previous network and it
  // did not come back. The page must name that network and the one action that remains, and must not
  // fall through to the generic "try again" — which would invite a retry of the thing that was not
  // the problem.
  //
  // ⛔ AND IT MAY CLAIM ONLY THE NARROW GUARANTEE. This reason can be published after the printer
  // stage was submitted, by which point both candidate records were already written to and verified
  // in the inactive flash slots — so "nothing was saved" is not true of every route. What holds
  // everywhere is that no activation pointer moved. Run beside the two scenarios below, which are its
  // negative controls: a credential failure and the generic fallback must both read differently.
  {
    name: "wifi-restore-failed",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-restore-failed",
  },
  {
    name: "wifi-restore-failure-is-not-a-credential-failure",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-restore-failed",
  },
  // ⛔ THE ROLLBACK REFUSAL, AND THREE DIFFERENT ROUTES PUBLISH IT. Only the PREFLIGHT one is
  // reported before the radio is ever touched; the staging-disagreement route had already written
  // candidate records to the inactive slots and already joined the candidate, and the
  // post-disturbance load-failure route had already joined the candidate and is taking it down.
  //
  // ⛔ SO THE PAGE MUST NOT GENERALISE THE PREFLIGHT ACCOUNT. It may claim only what holds on all
  // three — that no activation pointer moved, so the saved configuration was not replaced — and it
  // must name a restart rather than a retype or a bare "try again", which would invite a submission
  // refused again for the same reason. Run beside the restore failure below, which is its negative
  // control: the two must not read the same.
  {
    name: "wifi-rollback-unavailable",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-rollback-unavailable",
  },
  {
    name: "wifi-rollback-is-not-a-restore-failure",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-rollback-unavailable",
  },
  // ⛔ THE NEGATIVE CONTROL FOR THE ACTIONABLE OUTCOME, AS A SCENARIO. A generic Wi-Fi failure and a
  // quarantined driver must NOT read the same on the page: the first sends the user to check their
  // password, the second tells them to restart the display and must never mention the credential.
  // Run side by side so a change that collapsed one into the other fails here.
  {
    name: "wifi-stall-is-not-a-credential-failure",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "wifi-driver-stalled",
  },
  {
    name: "wifi-nonexistent-ssid-timeout",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "validating", "failed"] },
    reason: "wifi-timed-out",
  },
  // ⛔ THE THREE REASONS THAT USED TO MAKE UNSAFE PERSISTENCE CLAIMS. A storage failure may already
  // have written a candidate; an interrupted activation may already have moved a pointer; and an
  // unknown reason tells the page nothing at all about flash. Each now has copy bounded by what its
  // own semantics actually guarantee, and the four actions stay distinct: retry, restart, look at
  // the display, review the details.
  {
    name: "storage-commit-failed",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "storage-commit-failed",
  },
  {
    name: "promotion-failed",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "promotion-failed",
  },
  // An enumerator this page has never heard of — the case the fallback exists for, and the reason it
  // may not carry a guarantee about flash.
  {
    name: "unknown-reason-fallback",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "failed"] },
    reason: "some-reason-this-page-has-never-heard-of",
  },
  {
    name: "device-restart-mid-attempt",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    // The device reboots: back to "editing", and a NEW token. The page must not wait for ever.
    after: { wifi: ["validating", "editing"] },
    rotateTokenAfterPosts: 1,
  },
  {
    name: "reveal",
    statuses: ["editing"],
    setup: { wifi: accepted("wifi") },
    after: { wifi: ["validating", "wifi-validated"] },
    // One submission — the Wi-Fi step that reveals the API key field. Four reveal toggles add
    // nothing, which is the point: the control is presentation only.
    expectPosts: 1,
    shot: true,
  },

  // --- Owner ruling A: the page over the LAN ---------------------------------
  //
  // ⬅️ THE PAGE MUST NOT OFFER A CONTROL THE DEVICE WILL REFUSE. Owner ruling D6 kept a Save
  // action off this page while nothing could be saved; the same rule applies to a form whose
  // submission is guaranteed to be refused because it arrived over the local network rather
  // than over the display's own setup network. This is the only test that runs the page, so it
  // is the only place that fact can be checked at all.
  {
    name: "lan-read-only",
    statuses: ["editing"],
    setup: {},
    canConfigure: false,
    // ⛔ ZERO. A page that posted anything here would be posting into a guaranteed refusal.
    expectPosts: 0,
    shot: true,
  },
  {
    name: "lan-read-only-ignores-the-flow",
    // A provisioned device still reports a setup state, and "succeeded" is the one that would
    // otherwise draw the success panel for somebody who did nothing. The read-only surface must
    // not render the flow around whatever state it happens to see.
    statuses: ["succeeded"],
    setup: {},
    canConfigure: false,
    expectPosts: 0,
  },
  {
    name: "update-lan-available",
    statuses: ["succeeded"],
    setup: {},
    canConfigure: false,
    updateVersion: "0.2.0",
    expectPosts: 0,
    expectUpdatePosts: 0,
  },
  {
    name: "update-authorized-install",
    statuses: ["editing"],
    setup: {},
    canInstall: true,
    updateVersion: "0.2.0",
    expectPosts: 0,
    expectUpdatePosts: 1,
  },
];

// Returns the child so the caller can end it.
//
// ⛔ HEADLESS CHROME DOES NOT EXIT WHEN GIVEN A URL. It loads the page and stays open, so
// awaiting its exit hangs for ever. The driver reporting its result is the completion signal;
// the browser is killed once that arrives. Only --screenshot runs exit by themselves.
// Chrome can still hold its profile directory for a moment after SIGKILL, and a failed rmdir
// must not decide whether the suite passed.
async function discard(profile) {
  for (let attempt = 0; attempt < 5; attempt += 1) {
    try {
      await rm(profile, { recursive: true, force: true, maxRetries: 3 });
      return;
    } catch {
      await new Promise((resolve) => setTimeout(resolve, 300));
    }
  }
}

function sizeOf(file) {
  try {
    return statSync(file).size;
  } catch {
    return -1;
  }
}

// ⛔ A CAPTURE ENDS WHEN THE FILE IS FINISHED, NOT WHEN A TIMER EXPIRES.
//
// Every capture used to wait out the full kill budget, because a --screenshot run does not
// reliably exit on its own: 100 s of a 3 min suite spent watching a browser that had already done
// its work. The file is the deliverable, so the file is what gets watched — once it exists and
// its size has stopped changing, the browser is killed immediately.
//
// Two failures this must not hide, both of which would turn teardown trouble into a silent pass:
//
//   - A PNG left behind by an EARLIER run makes "the file exists" true before this browser has
//     done anything at all. The target is deleted first, so the file proves this run.
//   - Reaching the deadline is now a FAILURE rather than the normal path. That is the whole point
//     of the change, and a quiet return to waiting out the budget would undo it unnoticed.
async function capture(url, file, profile) {
  await rm(file, { force: true });

  const child = spawnChrome(url, profile, [
    `--screenshot=${file}`,
    "--virtual-time-budget=15000",
    "--hide-scrollbars",
  ]);

  let finished = false;
  child.on("exit", () => {
    finished = true;
  });
  child.on("error", () => {
    finished = true;
  });

  const started = Date.now();
  let previous = -1;
  let outcome = "deadline";

  while (Date.now() - started < CAPTURE_DEADLINE_MS) {
    const size = sizeOf(file);

    // Two equal readings, both plausible: the PNG is written, not merely created.
    if (size >= CAPTURE_MIN_BYTES && size === previous) {
      outcome = "settled";
      break;
    }
    if (finished) {
      outcome = size >= CAPTURE_MIN_BYTES ? "exited" : "exited-without-a-file";
      break;
    }

    previous = size;
    await new Promise((resolve) => setTimeout(resolve, CAPTURE_POLL_MS));
  }

  child.kill("SIGKILL");
  await discard(profile);

  return { outcome, elapsed: Date.now() - started, bytes: sizeOf(file) };
}

function spawnChrome(url, profile, extra = []) {
  const child = spawn(
    CHROME,
    [
      "--headless=new",
      "--disable-gpu",
      "--no-first-run",
      "--no-default-browser-check",
      "--disable-extensions",
      "--disable-background-networking",
      `--user-data-dir=${profile}`,
      `--window-size=${VIEWPORT}`,
      ...extra,
      url,
    ],
    { stdio: "ignore" },
  );

  // A ChildProcess that emits 'error' with no listener takes the whole run down with an uncaught
  // exception, which would report a crash instead of a browser that failed to spawn. The launch
  // proof makes that nearly impossible; this keeps it legible if it happens anyway.
  child.on("error", (error) => {
    console.log(`        browser failed to spawn: ${error.message}`);
  });

  return child;
}

// --- The declared tool requirement, resolved and proven ---------------------

function resolveBrowser() {
  const declared = process.env.PRD_CHROME;
  const candidates = declared ? [declared] : (CHROME_CANDIDATES[platform()] ?? []);

  for (const candidate of candidates) {
    try {
      accessSync(candidate, constants.X_OK);
      return candidate;
    } catch {
      /* not this one */
    }
  }

  const tried = candidates.length
    ? candidates.map((c) => `  ${c}`).join("\n")
    : `  (no candidates known for platform ${platform()})`;
  throw new Error(
    `no executable browser found. Tried:\n${tried}\n` +
      "Set PRD_CHROME to a Chrome or Chromium binary.",
  );
}

function browserVersion(path) {
  let raw = "";
  try {
    raw = execFileSync(path, ["--version"], { encoding: "utf-8", timeout: 30000 }).trim();
  } catch (error) {
    throw new Error(`${path} did not answer --version: ${error.message}`);
  }
  const major = Number(/\b(\d+)\.\d+\.\d+/.exec(raw)?.[1]);
  if (!Number.isInteger(major)) {
    throw new Error(`cannot read a version from ${path}: ${JSON.stringify(raw)}`);
  }
  return { raw, major };
}

// Chrome's own diagnostics are verbose enough to bury the line that matters, and the useful one
// comes first — a sandbox or namespace refusal is printed before any background chatter.
function firstLines(text, limit = 12) {
  const all = text
    .trim()
    .split("\n")
    .filter((line) => line.length);
  if (all.length <= limit) return all.join("\n") || "(no output)";
  return `${all.slice(0, limit).join("\n")}\n… ${all.length - limit} further line(s) omitted`;
}

// ⛔ PROVE IT STARTS. A browser that is present, executable and new enough can still fail to
// launch — the AppArmor user-namespace restriction noted above is exactly that failure — and
// without this check the symptom is 22 scenarios each reporting "driver never reported" 25 s
// apart, which names the harness instead of the cause.
//
// ⬅️ THE PROOF IS THE DOM, NOT THE EXIT, and that correction was paid for here. The first
// version of this check waited for `--dump-dom` to exit. It does not reliably exit: on macOS a
// fresh profile wakes GoogleUpdater and GCM registration, and it sat past its deadline while
// printing perfectly healthy DOM. The check reported a working browser as broken — the same
// mistake as judging a screenshot by a timer instead of by the file. So this watches stdout for a
// complete document and stops the moment it has one. The suppression flags are the same ones every
// other launch uses, for the same reason: without them the browser goes looking for the network.
function proveItLaunches(path, profile) {
  return new Promise((resolve) => {
    const child = spawn(
      path,
      [
        "--headless=new",
        "--disable-gpu",
        "--no-first-run",
        "--no-default-browser-check",
        "--disable-extensions",
        "--disable-background-networking",
        `--user-data-dir=${profile}`,
        "--dump-dom",
        "about:blank",
      ],
      { stdio: ["ignore", "pipe", "pipe"] },
    );

    let out = "";
    let err = "";
    let settled = false;
    let deadline = null;

    function finish(result) {
      if (settled) return;
      settled = true;
      if (deadline) clearTimeout(deadline);
      child.kill("SIGKILL");
      resolve(result);
    }

    child.stdout.on("data", (chunk) => {
      out += chunk;
      if (out.includes("</html>")) finish({ ok: true, detail: "" });
    });
    child.stderr.on("data", (chunk) => {
      err += chunk;
    });

    child.on("error", (error) => finish({ ok: false, detail: error.message }));
    child.on("exit", (code) =>
      finish({ ok: false, detail: `exited ${code} without a document\n${firstLines(err)}` }),
    );

    deadline = setTimeout(() => {
      const detail = `no document within ${LAUNCH_DEADLINE_MS}ms\n${firstLines(err)}`;
      finish({ ok: false, detail });
    }, LAUNCH_DEADLINE_MS);
  });
}

async function main() {
  // The tool requirement is settled before the suite starts, and reported either way: a run whose
  // log does not say which browser it used cannot be compared with the next one.
  let version = null;
  try {
    CHROME = resolveBrowser();
    version = browserVersion(CHROME);
  } catch (error) {
    console.log(`test-browser: ${error.message}`);
    process.exit(1);
  }

  console.log(`node       ${process.version}`);
  console.log(`browser    ${version.raw}`);
  console.log(`           ${CHROME}${process.env.PRD_CHROME ? "   (PRD_CHROME)" : ""}`);

  if (version.major < CHROME_MIN_MAJOR) {
    console.log(
      `test-browser: --headless=new needs Chrome/Chromium ${CHROME_MIN_MAJOR} or newer;` +
        ` this is ${version.major}`,
    );
    process.exit(1);
  }

  const probe = await mkdtemp(join(tmpdir(), "prd-probe-"));
  const launched = await proveItLaunches(CHROME, probe);
  await discard(probe);

  if (!launched.ok) {
    console.log("test-browser: that browser did not start headless.");
    for (const line of launched.detail.split("\n")) console.log(`  ${line}`);
    if (platform() === "linux") {
      console.log("");
      console.log("  On Ubuntu 23.10+ this is usually AppArmor's unprivileged user-namespace");
      console.log("  restriction. Official /opt/google/chrome installations are exempt from it;");
      console.log("  a self-downloaded build is not. Point PRD_CHROME at the packaged browser.");
    }
    process.exit(1);
  }
  console.log("");

  const server = await start(PORT);
  await mkdir(SHOTS, { recursive: true });

  let failed = 0;
  const shots = [];

  for (const scenario of SCENARIOS) {
    arm(scenario);
    const profile = await mkdtemp(join(tmpdir(), "prd-chrome-"));
    const chrome = spawnChrome(`http://127.0.0.1:${PORT}/`, profile);
    const result = await awaitResult(25000);
    chrome.kill("SIGKILL");
    await discard(profile);

    const posts = submissions();
    const updatePosts = updateSubmissions();
    const problems = [...result.failures];

    if (scenario.expectPosts !== undefined && posts.length !== scenario.expectPosts) {
      problems.push(
        `submission count: expected ${scenario.expectPosts}, got ${posts.length}`,
      );
    }
    if (
      scenario.expectUpdatePosts !== undefined &&
      updatePosts.length !== scenario.expectUpdatePosts
    ) {
      problems.push(
        `update submission count: expected ${scenario.expectUpdatePosts}, got ${updatePosts.length}`,
      );
    }

    for (const post of updatePosts) {
      if (post.raw !== "{}" || JSON.stringify(post.parsed) !== "{}") {
        problems.push(`update submission body: expected {}, got ${JSON.stringify(post.raw)}`);
      }
      if (post.headers["content-type"] !== "application/json") {
        problems.push(`update content type: got ${JSON.stringify(post.headers["content-type"])}`);
      }
      if (post.headers["x-prd-setup-token"] !== "harness-token") {
        problems.push(`update CSRF token: got ${JSON.stringify(post.headers["x-prd-setup-token"])}`);
      }
    }

    // ⛔ PROVES POLLING NEVER STARTED, RATHER THAN ASSUMING IT. Nothing was handed over, so no
    // validation was ever going to begin and the page must not ask for its outcome. Compared
    // against the count as it stood when the POST arrived, so a scenario that legitimately polled
    // for an EARLIER stage (the printer case polls through step 1) is still held to "not one
    // status request after this submission".
    if (scenario.expectNoPollsAfterLastPost && posts.length > 0) {
      const at = posts[posts.length - 1].pollsAtPost;
      if (polls() !== at) {
        problems.push(
          `status polling continued after the 503: ${at} polls at the POST, ${polls()} now`,
        );
      }
    }

    // No credential may appear in a submission for a scenario that makes none, and no
    // submission may ever carry a field the page was not asked to send.
    for (const post of posts) {
      for (const key of Object.keys(post.parsed)) {
        if (!["stage", "wifiSsid", "wifiPassword", "printerHost", "printerPort", "printerApiKey"].includes(key)) {
          problems.push(`unexpected field in submission: ${key}`);
        }
      }
    }

    if (problems.length) {
      failed += 1;
      console.log(`FAIL  ${scenario.name}`);
      for (const problem of problems) console.log(`        ${problem}`);
    } else {
      console.log(
        `ok    ${scenario.name}  [viewport ${result.notes?.viewport ?? "?"}` +
          ` scroll ${result.notes?.scrollWidth ?? "?"}/${result.notes?.clientWidth ?? "?"}]`,
      );
    }
  }

  // Screenshots of the states worth looking at, at iPhone size.
  let captureMs = 0;

  for (const scenario of SCENARIOS.filter((s) => s.shot)) {
    arm(scenario);
    const profile = await mkdtemp(join(tmpdir(), "prd-shot-"));
    const file = join(SHOTS, `${scenario.name}.png`);

    const suffix = scenario.name.startsWith("printer-") && scenario.reason ? "?failure-shot=1" : "";
    const shot = await capture(`http://127.0.0.1:${PORT}/${suffix}`, file, profile);
    captureMs += shot.elapsed;
    const measured = `[${shot.outcome} ${shot.elapsed}ms ${shot.bytes}B]`;

    if (shot.outcome === "settled" || shot.outcome === "exited") {
      shots.push(file);
      console.log(`ok    screenshot:${scenario.name}  ${measured}`);
    } else {
      failed += 1;
      console.log(`FAIL  screenshot:${scenario.name}  ${measured}`);
      if (shot.outcome === "deadline") {
        console.log(
          `        teardown did not settle within ${CAPTURE_DEADLINE_MS}ms — the browser never`,
        );
        console.log("        finished the capture, or never stopped writing it.");
      } else {
        console.log("        the browser exited without leaving a usable PNG.");
      }
    }
  }

  server.close();

  console.log("");
  console.log(`scenarios: ${SCENARIOS.length}   failed: ${failed}`);
  console.log(`screenshots: ${shots.length} in ${SHOTS}   teardown: ${captureMs}ms total`);
  process.exit(failed === 0 ? 0 : 1);
}

main();
