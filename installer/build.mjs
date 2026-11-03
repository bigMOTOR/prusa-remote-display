// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
import { cp, mkdir, readFile, readdir, writeFile, stat } from "node:fs/promises";
import { createHash } from "node:crypto";
import { fileURLToPath } from "node:url";
import { resolve, join } from "node:path";
import { validRelease } from "./gate.mjs";

const root = fileURLToPath(new URL(".", import.meta.url));
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

export async function build(output, bundle, accompaniment) {
  // Reject malformed candidate bytes before creating even a partial site.
  let release = { available: false };
  let manifestBytes;
  let merged;
  let source;
  const firmwareNotices = [];
  if (bundle) {
    if (!accompaniment) throw new Error("Firmware notices and source accompaniment required");
    source = JSON.parse(await readFile(join(accompaniment, "source.json")));
    release = JSON.parse(await readFile(join(bundle, "release.json")));
    if (!validRelease(release)) throw new Error("Invalid prepared release identity");
    if (source.tag !== release.tag || source.commit !== release.commit ||
        !/^[a-f0-9]{64}$/.test(source.sha256) ||
        source.url !== `https://github.com/bigMOTOR/prusa-remote-display/releases/download/${release.tag}/prusa-remote-display-${release.tag.slice(1)}-source.tar.gz` ||
        !Array.isArray(source.notices) || source.notices.length < 1 || source.notices.length > 200) {
      throw new Error("Firmware source identity differs");
    }
    const seen = new Set();
    for (const item of source.notices) {
      if (!/^[a-zA-Z0-9][a-zA-Z0-9._-]{0,120}$/.test(item.name) || seen.has(item.name)) {
        throw new Error("Invalid firmware notice filename");
      }
      seen.add(item.name);
      const bytes = await readFile(join(accompaniment, item.name));
      if (!bytes.length || bytes.length > 1024 * 1024 || sha256(bytes) !== item.sha256) {
        throw new Error("Firmware notice bytes differ");
      }
      firmwareNotices.push([item.name, bytes]);
      if (firmwareNotices.reduce((n, [, b]) => n + b.length, 0) > 4 * 1024 * 1024) {
        throw new Error("Firmware notices exceed 4 MiB budget");
      }
    }
    for (const required of ["THIRD_PARTY_NOTICES.md", "LICENSE.txt", "LICENSING.md", "build-dependencies.txt", "notice-origins.json"]) {
      if (!seen.has(required)) throw new Error(`Missing firmware accompaniment: ${required}`);
    }
    const noticeMap = new Map(firmwareNotices);
    const upstream = new Set([...seen].filter((name) => name.startsWith("upstream-")));
    const origins = JSON.parse(noticeMap.get("notice-origins.json"));
    if (!Array.isArray(origins) || !origins.length || !upstream.size) {
      throw new Error("Upstream notice origins missing");
    }
    const originFiles = new Set();
    const originalPaths = new Set();
    for (const item of origins) {
      if (typeof item.original !== "string" || !item.original ||
          item.original.startsWith("/") || item.original.includes("\\") ||
          item.original.split("/").some((p) => !p || p === "." || p === "..") ||
          item.file !== `upstream-${sha256(item.original)}.txt` ||
          !upstream.has(item.file) || originFiles.has(item.file) || originalPaths.has(item.original) ||
          sha256(noticeMap.get(item.file)) !== item.sha256) {
        throw new Error("Upstream notice origin bytes differ");
      }
      originFiles.add(item.file);
      originalPaths.add(item.original);
    }
    if (originFiles.size !== upstream.size) throw new Error("Upstream notice origins incomplete");
    manifestBytes = await readFile(join(bundle, "web-manifest.json"));
    const manifest = JSON.parse(manifestBytes);
    merged = await readFile(join(bundle, "merged-firmware.bin"));
    if (sha256(merged) !== release.imageSha256 ||
        sha256(manifestBytes) !== release.manifestSha256 ||
        merged.length === 0 || merged.length > 16 * 1024 * 1024 ||
        manifest.version !== release.tag.slice(1) || manifest.builds?.length !== 1 ||
        manifest.builds[0].chipFamily !== "ESP32-S3" ||
        manifest.builds[0].parts?.length !== 1 ||
        manifest.builds[0].parts[0].path !== "merged-firmware.bin" ||
        manifest.builds[0].parts[0].offset !== 0 ||
        manifest.new_install_prompt_erase !== false ||
        manifest.new_install_improv_wait_time !== 0) {
      throw new Error("Prepared release bytes or installation contract differ");
    }
  }
  const pkg = JSON.parse(await readFile(join(root, "node_modules/esp-web-tools/package.json")));
  const lock = JSON.parse(await readFile(join(root, "package-lock.json")));
  if (pkg.version !== "10.4.0" || lock.packages["node_modules/esp-web-tools"].version !== pkg.version) {
    throw new Error("ESP Web Tools version differs from the reviewed pin");
  }
  // Never replace an existing output silently; use a fresh, bounded directory.
  await mkdir(output);
  for (const name of ["index.html", "style.css", "app.js", "gate.mjs"]) {
    await cp(join(root, name), join(output, name));
  }
  await cp(join(root, "node_modules/esp-web-tools/dist/web"), join(output, "vendor"), { recursive: true });
  await mkdir(join(output, "licenses"));
  let notices = "Third-party licenses for this browser installer\n\n";
  for (const [path, entry] of Object.entries(lock.packages)) {
    if (!path) continue;
    const files = await readdir(join(root, path));
    const texts = files.filter((name) => /^(LICENSE|LICENCE|NOTICE)(\.|$)/i.test(name));
    const name = path.replace(/^node_modules\//, "");
    if (!texts.length && name === "@lit-labs/ssr-dom-shim" && entry.version === "1.6.0") {
      // Its npm tarball omits LICENSE. The pinned upstream monorepo tag uses
      // the same BSD text shipped by lit. Keep the shim's source copyright too.
      // https://github.com/lit/lit/blob/%40lit-labs%2Fssr-dom-shim%401.6.0/LICENSE
      const license = await readFile(join(root, "node_modules/lit/LICENSE"));
      const shim = await readFile(join(root, path, "index.js"), "utf8");
      if (!shim.includes("Copyright 2019 Google LLC") || !shim.includes("BSD-3-Clause")) {
        throw new Error("SSR shim provenance differs");
      }
      const notice = `@lit-labs/ssr-dom-shim 1.6.0 · Copyright 2019 Google LLC\n${license}`;
      await writeFile(join(output, "licenses/ssr-dom-shim-LICENSE.txt"), notice);
      notices += notice + "\n\n";
      continue;
    }
    if (!texts.length) throw new Error(`Missing upstream license text: ${path}`);
    notices += `${name} ${entry.version} · ${entry.license}\n`;
    for (const text of texts) {
      const bytes = await readFile(join(root, path, text));
      await writeFile(join(output, "licenses", `${name.replaceAll("/", "-")}-${text}`), bytes);
      notices += `${text}\n${bytes.toString("utf8")}\n\n`;
    }
  }
  if (bundle) {
    await mkdir(join(output, "firmware-notices"));
    for (const [name, bytes] of firmwareNotices) {
      await writeFile(join(output, "firmware-notices", name), bytes);
      notices += `\nFirmware: ${name}\n${bytes.toString("utf8")}\n`;
    }
    await writeFile(join(output, "source.json"), JSON.stringify(source, null, 2) + "\n");
    notices += `\nExact corresponding-source archive: ${source.url}\nSHA-256: ${source.sha256}\n`;
  }
  await writeFile(join(output, "third-party-notices.txt"), notices);
  await cp(join(root, "../LICENSE"), join(output, "LICENSE.txt"));
  if (bundle) {
    await mkdir(join(output, "firmware"));
    await writeFile(join(output, "firmware/merged-firmware.bin"), merged);
    await writeFile(join(output, "firmware/web-manifest.json"), manifestBytes);
  }
  await writeFile(join(output, "release.json"), JSON.stringify(release, null, 2) + "\n");
  await writeFile(join(output, ".nojekyll"), "");
  let total = 0;
  async function measure(path) {
    for (const entry of await readdir(path, { withFileTypes: true })) {
      const child = join(path, entry.name);
      if (entry.isDirectory()) await measure(child);
      else total += (await stat(child)).size;
    }
  }
  await measure(output);
  if (total > 20 * 1024 * 1024) throw new Error("Installer output exceeds 20 MiB budget");
  return { output, bytes: total, release: release.available === true };
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [output = join(root, "dist"), bundle, accompaniment] = process.argv.slice(2);
  console.log(JSON.stringify(await build(resolve(output), bundle && resolve(bundle), accompaniment && resolve(accompaniment))));
}
