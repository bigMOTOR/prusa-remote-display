// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
import test from "node:test";
import assert from "node:assert/strict";
import { mkdtemp, readFile, writeFile, mkdir, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { build } from "../build.mjs";
import { BOARD } from "../gate.mjs";
const hash = (b) => createHash("sha256").update(b).digest("hex");
test("preview is disabled, self-hosted and bounded; output cannot be replaced", async () => {
  const root = await mkdtemp(join(tmpdir(), "prd-site-test-"));
  try {
    const out = join(root, "site");
    const result = await build(out);
    assert.equal(result.release, false);
    assert.ok(result.bytes < 1024 * 1024);
    assert.deepEqual(JSON.parse(await readFile(join(out, "release.json"))), { available: false });
    assert.match(await readFile(join(out, "index.html"), "utf8"), /id="install" disabled/);
    assert.ok((await readFile(join(out, "vendor/install-button.js"))).length > 0);
    assert.match(await readFile(join(out, "third-party-notices.txt"), "utf8"), /Apache License/);
    assert.match(await readFile(join(out, "LICENSE.txt"), "utf8"), /GNU GENERAL PUBLIC LICENSE/);
    await assert.rejects(build(out), /EEXIST/);
  } finally { await rm(root, { recursive: true }); }
});
test("release copies only bound bytes; malformed or substituted contracts fail closed", async () => {
  const root = await mkdtemp(join(tmpdir(), "prd-site-test-"));
  try {
    const bundle = join(root, "bundle");
    await mkdir(bundle);
    // Test fixture only: these bytes are never offered as real firmware.
    const image = Buffer.from("test-fixture-not-firmware");
    const manifest = { version: "0.1.0", new_install_prompt_erase: false,
      new_install_improv_wait_time: 0, builds: [{ chipFamily: "ESP32-S3",
        parts: [{ path: "merged-firmware.bin", offset: 0 }] }] };
    const bytes = JSON.stringify(manifest);
    const release = { available: true, board: BOARD, tag: "v0.1.0", commit: "a".repeat(40),
      imageSha256: hash(image), manifestSha256: hash(bytes), publisherSpkiSha256: "c".repeat(64),
      manifest: "firmware/web-manifest.json" };
    await writeFile(join(bundle, "merged-firmware.bin"), image);
    await writeFile(join(bundle, "web-manifest.json"), bytes);
    await writeFile(join(bundle, "release.json"), JSON.stringify(release));
    await assert.rejects(build(join(root, "missing-notices"), bundle), /accompaniment required/);
    const source = { tag: release.tag, commit: release.commit, sha256: "d".repeat(64),
      url: `https://github.com/bigMOTOR/prusa-remote-display/releases/download/${release.tag}/prusa-remote-display-${release.tag.slice(1)}-source.tar.gz`,
      notices: [] };
    for (const name of ["THIRD_PARTY_NOTICES.md", "LICENSE.txt", "LICENSING.md", "build-dependencies.txt"]) {
      const bytes = Buffer.from(`fixture-only ${name}`);
      await writeFile(join(bundle, name), bytes);
      source.notices.push({ name, sha256: hash(bytes) });
    }
    const upstreamName = `upstream-${hash("dependency/LICENSE")}.txt`;
    const upstreamBytes = Buffer.from("full upstream fixture terms");
    const originsBytes = Buffer.from(JSON.stringify([{ file: upstreamName,
      original: "dependency/LICENSE", sha256: hash(upstreamBytes) }]));
    for (const [name, bytes] of [[upstreamName, upstreamBytes], ["notice-origins.json", originsBytes]]) {
      await writeFile(join(bundle, name), bytes);
      source.notices.push({ name, sha256: hash(bytes) });
    }
    await writeFile(join(bundle, "source.json"), JSON.stringify(source));
    assert.equal((await build(join(root, "valid"), bundle, bundle)).release, true);
    assert.deepEqual(JSON.parse(await readFile(join(root, "valid/source.json"))), source);
    const validUrl = source.url;
    for (const wrongName of ["candidate-source.tar.gz", "prusa-remote-display-9.9.9-source.tar.gz"]) {
      source.url = `https://github.com/bigMOTOR/prusa-remote-display/releases/download/${release.tag}/${wrongName}`;
      await writeFile(join(bundle, "source.json"), JSON.stringify(source));
      const rejectedOutput = join(root, wrongName);
      await assert.rejects(build(rejectedOutput, bundle, bundle), /Firmware source identity differs/);
      await assert.rejects(stat(rejectedOutput), /ENOENT/);
    }
    source.url = validUrl;
    await writeFile(join(bundle, "source.json"), JSON.stringify(source));
    const validNotices = [...source.notices];
    for (const missing of ["notice-origins.json", upstreamName]) {
      source.notices = validNotices.filter((item) => item.name !== missing);
      await writeFile(join(bundle, "source.json"), JSON.stringify(source));
      await assert.rejects(build(join(root, `missing-${missing}`), bundle, bundle), /Missing firmware accompaniment|Upstream notice/);
    }
    source.notices = validNotices;
    const badOrigins = Buffer.from(JSON.stringify([{ file: upstreamName,
      original: "dependency/LICENSE", sha256: "0".repeat(64) }]));
    await writeFile(join(bundle, "notice-origins.json"), badOrigins);
    source.notices = validNotices.map((item) => item.name === "notice-origins.json" ? { ...item, sha256: hash(badOrigins) } : item);
    await writeFile(join(bundle, "source.json"), JSON.stringify(source));
    await assert.rejects(build(join(root, "bad-origin"), bundle, bundle), /origin bytes differ/);
    source.notices = validNotices;
    await writeFile(join(bundle, "notice-origins.json"), originsBytes);
    await writeFile(join(bundle, "source.json"), JSON.stringify(source));
    await writeFile(join(bundle, "LICENSE.txt"), "tampered");
    await assert.rejects(build(join(root, "bad-notice"), bundle, bundle), /notice bytes differ/);
    await writeFile(join(bundle, "LICENSE.txt"), "fixture-only LICENSE.txt");
    assert.deepEqual(await readFile(join(root, "valid/firmware/merged-firmware.bin")), image);
    await writeFile(join(bundle, "merged-firmware.bin"), "tampered");
    await assert.rejects(build(join(root, "bad-hash"), bundle, bundle));
    await assert.rejects(stat(join(root, "bad-hash")), /ENOENT/);
    await writeFile(join(bundle, "merged-firmware.bin"), image);
    manifest.builds[0].parts[0].path = "../../other.bin";
    const bad = JSON.stringify(manifest);
    await writeFile(join(bundle, "web-manifest.json"), bad);
    await writeFile(join(bundle, "release.json"), JSON.stringify({ ...release, manifestSha256: hash(bad) }));
    await assert.rejects(build(join(root, "bad-path"), bundle, bundle));
    await assert.rejects(stat(join(root, "bad-path")), /ENOENT/);
  } finally { await rm(root, { recursive: true }); }
});
