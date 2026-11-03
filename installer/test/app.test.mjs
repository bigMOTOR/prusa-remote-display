// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { runInNewContext } from "node:vm";
import { validRelease, mayInstall, BOARD } from "../gate.mjs";

class Node {
  constructor(name) { this.name = name; this.children = []; this.listeners = {}; this.checked = false; }
  append(child) {
    if (child.parent) child.parent.children = child.parent.children.filter((n) => n !== child);
    child.parent = this;
    this.children.push(child);
  }
  replaceChildren(...children) {
    for (const child of this.children) child.parent = undefined;
    this.children = [];
    for (const child of children) this.append(child);
  }
  addEventListener(name, callback) { this.listeners[name] = callback; }
}

async function page(release, serial = true, secure = true, failImport = false) {
  const ids = Object.fromEntries(["board-confirm", "erase-confirm", "install", "release-status",
    "browser-status", "install-host", "source-link"].map((id) => [id, new Node(id)]));
  ids["install-host"].append(ids.install);
  let source = await readFile(new URL("../app.js", import.meta.url), "utf8");
  const staticImport = 'import { validRelease, mayInstall } from "./gate.mjs";';
  const dynamicImport = 'await import("./vendor/install-button.js")';
  assert.ok(source.includes(staticImport) && source.includes(dynamicImport));
  // Stub only the dependency-loading seam; execute the actual application's
  // remaining DOM logic, including attachment/removal and failure handling.
  source = source.replace(staticImport, "").replace(dynamicImport, "await loadTool()");
  await runInNewContext(`(async () => {${source}})()`, {
    validRelease, mayInstall, navigator: serial ? {serial: {}} : {}, window: { isSecureContext: secure },
    document: {getElementById: (id) => ids[id], createElement: (tag) => new Node(tag)},
    fetch: async () => ({ok: true, json: async () => release}),
    loadTool: async () => { if (failImport) throw new Error("missing vendor"); },
  });
  return ids;
}
const release = {available:true,board:BOARD,tag:"v0.1.0",commit:"a".repeat(40),
  imageSha256:"b".repeat(64),manifestSha256:"c".repeat(64),publisherSpkiSha256:"d".repeat(64),
  manifest:"firmware/web-manifest.json"};
test("guided document preserves gate bindings, visible confirmations and setup order", async () => {
  const html = await readFile(new URL("../index.html", import.meta.url), "utf8");
  for (const id of ["board-confirm", "erase-confirm", "install", "install-host",
    "release-status", "browser-status", "source-link"]) {
    assert.equal(html.split(`id="${id}"`).length - 1, 1, `unique application binding: ${id}`);
  }
  const confirmations = html.match(/<fieldset>([\s\S]*?)<\/fieldset>/)?.[1];
  assert.ok(confirmations);
  assert.match(confirmations, /Waveshare <span class="board-id">ESP32-S3-LCD-2\.8<\/span>,\s*<span class="board-id">non-touch\.<\/span>/);
  assert.match(confirmations, /erases saved display settings/);
  assert.doesNotMatch(confirmations, /<details|\shidden(?:\s|=|>)/);
  assert.match(html, /<button id="install" disabled>/);
  assert.ok(html.indexOf('id="install-title"') < html.indexOf('id="setup-title"'));
  assert.match(html, /PRD-Setup-…/);
  assert.match(html, /http:\/\/192\.168\.4\.1/);
  assert.match(html, /not on this website/);
  assert.match(html, /href="third-party-notices\.txt"/);
});
test("community attribution and all local setup steps stay visible without disclosures", async () => {
  const html = await readFile(new URL("../index.html", import.meta.url), "utf8");
  const visibleContent = html.replace(/<details\b[^>]*>[\s\S]*?<\/details>/g, "");
  assert.match(visibleContent, /Independent community project; not affiliated with or endorsed by Prusa Research/);
  const setup = visibleContent.match(/<section class="setup"[^>]*>([\s\S]*?)<\/section>/)?.[1];
  assert.ok(setup);
  assert.equal(setup.match(/<li>/g)?.length, 4);
  assert.match(setup, /PRD-Setup-…/);
  assert.match(setup, /http:\/\/192\.168\.4\.1/);
  assert.match(setup, /not on this website/);
  assert.match(html, /class="visually-hidden">Step 1: /);
  assert.match(html, /class="visually-hidden">Step 2: /);
});
test("no upstream fallback button is connected until both confirmations, and removal re-locks", async () => {
  const ids = await page(release);
  const host = ids["install-host"], button = ids.install;
  assert.equal(button.disabled, true);
  assert.deepEqual(host.children.map((n) => n.name), ["install"]);
  assert.equal(ids["source-link"].href,
    `https://github.com/bigMOTOR/prusa-remote-display/tree/${release.commit}`);
  ids["board-confirm"].checked = true;
  ids["board-confirm"].listeners.change();
  assert.deepEqual(host.children.map((n) => n.name), ["install"]);
  ids["erase-confirm"].checked = true;
  ids["erase-confirm"].listeners.change();
  assert.equal(button.disabled, false);
  assert.equal(host.children[0].name, "esp-web-install-button");
  assert.deepEqual(host.children[0].children, [button]);
  ids["board-confirm"].checked = false;
  ids["board-confirm"].listeners.change();
  assert.equal(button.disabled, true);
  assert.deepEqual(host.children.map((n) => n.name), ["install"]);
});
test("missing vendor is distinct from missing release and remains locked", async () => {
  const vendor = await page(release, true, true, true);
  assert.match(vendor["release-status"].textContent, /USB installation tool could not load/);
  const preview = await page({ available: false });
  assert.match(preview["release-status"].textContent, /No installation-ready release/);
  assert.equal(vendor.install.disabled, true);
  assert.deepEqual(vendor["install-host"].children.map((n) => n.name), ["install"]);
});
test("preview, unsupported browser, insecure context and missing vendor never attach installer", async () => {
  for (const args of [[{available:false}], [release,false], [release,true,false], [release,true,true,true]]) {
    const ids = await page(...args);
    for (const id of ["board-confirm", "erase-confirm"]) { ids[id].checked = true; ids[id].listeners.change(); }
    assert.equal(ids.install.disabled, true);
    assert.deepEqual(ids["install-host"].children.map((n) => n.name), ["install"]);
  }
});
