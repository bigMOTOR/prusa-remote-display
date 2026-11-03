// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
//
// The deterministic stand-in for the device, for browser-flow tests.
//
// WHY THIS EXISTS. Three physical validation cycles were spent finding page defects that a
// browser could have found in a second — `hidden` doing nothing on an SVG, a success class
// surviving into an error, placeholders reading as data. Static assertions over the asset text
// cannot catch any of those, because none of them is visible in the source. This serves the
// REAL index.html, style.css and app.js and lets a real browser execute them.
//
// DETERMINISM IS THE POINT. Every /api/status answer and every /api/setup answer is scripted
// per scenario, so the validating→validated race, the polling interval and the failure paths
// happen the same way every run. Node built-ins only: no npm package, and nothing here is a
// firmware dependency.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const ASSETS = join(HERE, "..", "..", "components", "provisioning", "assets");

const TYPES = {
  "index.html": "text/html; charset=utf-8",
  "style.css": "text/css; charset=utf-8",
  "app.js": "text/javascript; charset=utf-8",
};

// One scenario is active at a time; the orchestrator sets it before launching the browser.
let active = { name: "none", statuses: ["editing"], setup: {}, reason: "" };
let pollIndex = 0;
let token = "harness-token";
let posts = [];
let updatePosts = [];
let result = null;
const waiters = [];

export function arm(scenario) {
  active = scenario;
  pollIndex = 0;
  token = "harness-token";
  posts = [];
  updatePosts = [];
  result = null;
}

export function submissions() {
  return posts;
}

export function updateSubmissions() {
  return updatePosts;
}

// How many /api/status requests the page has made. Each submission below is stamped with the count
// as it stood when the POST arrived, so a scenario can assert that NO status request happened after
// a given submission — which is how "polling never started" is proven rather than assumed.
export function polls() {
  return pollIndex;
}

export function awaitResult(timeoutMs) {
  if (result) return Promise.resolve(result);
  return new Promise((resolve) => {
    const timer = setTimeout(
      () => resolve({ scenario: active.name, failures: ["TIMEOUT: driver never reported"] }),
      timeoutMs,
    );
    waiters.push((r) => {
      clearTimeout(timer);
      resolve(r);
    });
  });
}

// The status the page sees on its Nth poll. The last entry repeats, so a scenario does not
// have to predict how many polls the page performs.
function nextStatus() {
  const list = active.statuses;
  const state = list[Math.min(pollIndex, list.length - 1)];
  pollIndex += 1;
  return state;
}

function send(res, code, body, type) {
  const raw = Buffer.isBuffer(body) ? body : Buffer.from(body, "utf-8");
  res.writeHead(code, {
    "Content-Type": type,
    "Content-Length": raw.length,
    "Cache-Control": "no-store",
  });
  res.end(raw);
}

async function asset(name) {
  return readFile(join(ASSETS, name));
}

export function start(port) {
  const server = createServer(async (req, res) => {
    const url = new URL(req.url, "http://127.0.0.1");

    // ⛔ THE PAGE UNDER TEST RUNS IN A 375-WIDE IFRAME, AND THAT IS NOT A CONVENIENCE.
    // Chrome clamps --window-size to a 500 px minimum on macOS, so a run that asked for 375
    // silently laid out at 500 and the screenshot was cropped rather than phone-width — an
    // instrument reporting a width it did not have. An iframe of exactly 375x812 gives the real
    // page a real phone viewport, and being same-origin the driver can still reach into it.
    if (req.method === "GET" && url.pathname === "/") {
      const wrapper =
        `<!doctype html><html><head><meta charset="utf-8"><title>harness</title>` +
        `<style>html,body{margin:0;background:#000}` +
        `iframe{width:375px;height:812px;border:0;display:block}</style></head><body>` +
        `<iframe id="frame" src="/page"></iframe>` +
        `<script>window.__scenario=${JSON.stringify(active.name)};</script>` +
        `<script src="/__driver.js"></script></body></html>`;
      send(res, 200, wrapper, TYPES["index.html"]);
      return;
    }

    // The real page, unmodified — exactly what the device serves.
    if (req.method === "GET" && (url.pathname === "/page" || url.pathname === "/index.html")) {
      send(res, 200, await asset("index.html"), TYPES["index.html"]);
      return;
    }

    const name = url.pathname.slice(1);
    if (req.method === "GET" && TYPES[name]) {
      send(res, 200, await asset(name), TYPES[name]);
      return;
    }

    if (req.method === "GET" && url.pathname === "/__driver.js") {
      send(res, 200, await readFile(join(HERE, "driver.js")), TYPES["app.js"]);
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/status") {
      const state = nextStatus();
      send(
        res,
        200,
        JSON.stringify({
          product: "Prusa Remote Display",
          version: "0.1.0-dev",

          // ⬅️ WITHHELD ON A SURFACE THAT CANNOT USE IT, exactly as the device does: on the
          // persistent LAN surface no mutation is permitted, so the token there would
          // authorise nothing. A scenario opts into that surface with canConfigure: false.
          csrfToken: active.canConfigure === false ? "" : token,
          setupActive: true,

          // Owner ruling A of Milestone 3.9. Absent from a scenario means the setup access
          // point, which is what every scenario written before 3.9 assumed.
          canConfigure: active.canConfigure !== false,
          canInstall: active.canInstall === true,
          updateVersion: active.updateVersion ?? "",
          updateInstalling: active.updateInstalling === true,

          setupState: state,
          setupReason: state === "failed" ? active.reason || "wifi-unreachable" : "",
        }),
        "application/json",
      );
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/update") {
      let body = "";
      for await (const chunk of req) body += chunk;
      let parsed = null;
      try {
        parsed = JSON.parse(body);
      } catch {
        /* recorded as raw below */
      }
      updatePosts.push({ raw: body, parsed, headers: { ...req.headers } });

      const reply = active.updateReply ?? { accepted: true };
      if (reply.accepted) {
        active = { ...active, canInstall: false, updateInstalling: true };
      }
      send(res, reply.status ?? 200, JSON.stringify(reply.body ?? reply), "application/json");
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/setup") {
      let body = "";
      for await (const chunk of req) body += chunk;
      let parsed = {};
      try {
        parsed = JSON.parse(body);
      } catch {
        /* recorded as raw below */
      }
      posts.push({ raw: body, parsed, headers: { ...req.headers }, pollsAtPost: pollIndex });

      // A restart mints a new session token. This is how the harness reproduces the panic that
      // left the page hanging, without needing to panic anything.
      if (active.rotateTokenAfterPosts && posts.length >= active.rotateTokenAfterPosts) {
        token = "harness-token-after-restart";
      }

      const stage = parsed.stage === "printer" ? "printer" : "wifi";
      const attempt = posts.filter((p) => (p.parsed.stage ?? "wifi") === stage).length;

      // A stage may answer differently on its Nth attempt — `setup.wifi` may be a single reply
      // or an array indexed by attempt, which is what lets "fails once, then succeeds unchanged"
      // be scripted honestly. The last entry repeats.
      const configured = active.setup[stage];
      const reply = Array.isArray(configured)
        ? configured[Math.min(attempt - 1, configured.length - 1)]
        : (configured ?? { accepted: false, stage });

      // A scenario may advance the status script the moment a stage is accepted, which is what
      // the device does — the phase moves on the application task just after the POST returns.
      //
      // ⬅️ PER ATTEMPT, NOT PER STAGE. Re-arming the same script on every accepted POST made a
      // retry replay the previous attempt's failure, so the harness reported a page defect that
      // was its own. `after.wifi` may therefore be an array of scripts, one per attempt.
      const after = active.after && active.after[stage];
      if (reply.accepted && after) {
        const script = Array.isArray(after[0])
          ? after[Math.min(attempt - 1, after.length - 1)]
          : after;
        active = { ...active, statuses: script };
        pollIndex = 0;
      }
      send(res, reply.status ?? 200, JSON.stringify(reply.body ?? reply), "application/json");
      return;
    }

    if (req.method === "POST" && url.pathname === "/__result") {
      let body = "";
      for await (const chunk of req) body += chunk;
      result = JSON.parse(body);
      send(res, 200, "{}", "application/json");
      while (waiters.length) waiters.shift()(result);
      return;
    }

    send(res, 404, "{}", "application/json");
  });

  return new Promise((resolve) => server.listen(port, "127.0.0.1", () => resolve(server)));
}
