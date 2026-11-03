// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
import { validRelease, mayInstall } from "./gate.mjs";
const board = document.getElementById("board-confirm");
const erase = document.getElementById("erase-confirm");
const button = document.getElementById("install");
const status = document.getElementById("release-status");
const browser = document.getElementById("browser-status");
const source = document.getElementById("source-link");
let release;
let installer;
let toolReady = false;

function update() {
  const enabled = toolReady && mayInstall(release, board.checked, erase.checked,
    "serial" in navigator, window.isSecureContext);
  button.disabled = !enabled;
  // Do not attach upstream click handling to a disabled control: the tool's
  // slot listener handles clicks independently of the button's disabled state.
  const host = document.getElementById("install-host");
  if (installer && enabled) {
    installer.append(button);
    host.replaceChildren(installer);
  } else {
    // Detach the entire element too: upstream otherwise exposes its default
    // Connect button when its activation slot is empty.
    host.replaceChildren(button);
  }
}
board.addEventListener("change", update);
erase.addEventListener("change", update);
browser.textContent = !window.isSecureContext
  ? "USB installation requires HTTPS (or localhost for development)."
  : !("serial" in navigator)
    ? "This browser cannot install over USB. Use desktop Chrome or Edge with Web Serial."
    : "Your browser supports USB installation. No USB port is opened until you click and choose one.";

try {
  const response = await fetch("./release.json", { cache: "no-store" });
  if (!response.ok) throw new Error("release unavailable");
  release = await response.json();
  if (!validRelease(release)) throw new Error("no installation-ready release");
  status.textContent = `Release ${release.tag} · Waveshare ESP32-S3-LCD-2.8 non-touch`;
  source.href = `https://github.com/bigMOTOR/prusa-remote-display/tree/${release.commit}`;
  source.textContent = `Source for ${release.tag}`;
  if ("serial" in navigator && window.isSecureContext) {
    try {
      await import("./vendor/install-button.js");
      installer = document.createElement("esp-web-install-button");
      installer.manifest = release.manifest;
      button.slot = "activate";
      toolReady = true;
    } catch {
      status.textContent = `Release ${release.tag} is available, but the USB installation tool could not load. Reload this page; installation is disabled.`;
    }
  }
} catch {
  release = undefined;
  status.textContent = "No installation-ready release is available yet. This is a preview; installation is disabled.";
}
update();
