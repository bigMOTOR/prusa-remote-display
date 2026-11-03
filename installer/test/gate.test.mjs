// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
import test from "node:test";
import assert from "node:assert/strict";
import { BOARD, validRelease, mayInstall } from "../gate.mjs";
const release = { available: true, board: BOARD, tag: "v0.1.0", commit: "a".repeat(40),
  imageSha256: "b".repeat(64), manifestSha256: "c".repeat(64),
  publisherSpkiSha256: "d".repeat(64), manifest: "firmware/web-manifest.json" };
test("only a pinned reference-board release can enable the USB control", () => {
  assert.equal(mayInstall(release, true, true, true, true), true);
  for (const key of Object.keys(release)) {
    assert.equal(validRelease({ ...release, [key]: "invalid" }), false, key);
  }
  for (let i = 0; i < 4; i++) {
    const conditions = [true, true, true, true];
    conditions[i] = false;
    assert.equal(mayInstall(release, ...conditions), false);
  }
  assert.equal(validRelease({ ...release, tag: "main" }), false);
  assert.equal(validRelease({ ...release, tag: "v0.1.0-dev" }), false);
  assert.equal(mayInstall(undefined, true, true, true, true), false);
});
