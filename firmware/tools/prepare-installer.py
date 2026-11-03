#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare an offline browser-install candidate from a verified signed release.

No signing, publisher private key, serial access, upload or deployment. Supply the
intended tag/commit and trusted public key independently of the bundle. Real USB
installation and owner approval remain publication gates, not results of this tool.
"""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace


def release_gate():
    spec = importlib.util.spec_from_file_location(
        "prd_browser_release_gate", Path(__file__).with_name("check-release-assets.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def prepare(bundle: Path, tag: str, commit: str, public_key: Path, output: Path) -> dict:
    if re.fullmatch(r"v\d+\.\d+\.\d+", tag) is None:
        raise ValueError("browser installation requires an exact stable release tag")
    gate = release_gate()
    summary = gate.verify(bundle, tag, commit)
    if output.exists():
        raise ValueError("choose a new output directory; existing output is never replaced")
    # espsecure imports only here so stdlib tests can inspect the basic gates.
    import espsecure
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key_data = public_key.read_bytes()
    key = serialization.load_pem_public_key(key_data)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        raise ValueError("expected the trusted RSA-3072 public verification key")
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    if digest(der) != summary["publicKeySpkiSha256"]:
        raise ValueError("release publisher differs from independently supplied public key")
    app = (bundle / gate.IMAGE_NAME).read_bytes()
    with contextlib.redirect_stdout(io.StringIO()):
        espsecure.verify_signature(SimpleNamespace(version="2", datafile=io.BytesIO(app),
                                                   keyfile=io.BytesIO(key_data), hsm=False))
    flash_path = gate._regular_file(bundle, "flash_args.json", 8192)
    checksums = gate._checksums(bundle / "SHA256SUMS")
    if checksums.get("flash_args.json") != digest(flash_path.read_bytes()):
        raise ValueError("SHA256SUMS does not bind flash_args.json")
    flash = json.loads(flash_path.read_text(), object_pairs_hook=gate._unique_object)
    expected = {"0x0": "bootloader.bin", "0x8000": "partition-table.bin",
                "0x10000": "ota_data_initial.bin", "0x20000": gate.IMAGE_NAME}
    if flash.get("flash_files") != expected:
        raise ValueError("unexpected reference-board initial flash regions")
    for region, (offset, name) in zip(
            ("bootloader", "partition-table", "otadata", "app"), expected.items()):
        record = flash.get(region, {})
        if record.get("offset") != offset or record.get("file") != name or \
                record.get("encrypted", "false") != "false":
            raise ValueError("initial flash region records disagree")
    settings = flash.get("flash_settings", {})
    if settings.get("flash_mode") not in ("qio", "dio") or \
       settings.get("flash_freq") != "80m" or settings.get("flash_size") not in ("keep", "16MB"):
        raise ValueError("unexpected reference-board flash parameters")
    if flash.get("extra_esptool_args", {}).get("chip") != "esp32s3":
        raise ValueError("unexpected flasher chip target")
    source = {}
    for offset, name in expected.items():
        path = gate._regular_file(bundle, name, 16 * 1024 * 1024)
        data = path.read_bytes()
        if checksums.get(name) != digest(data):
            raise ValueError(f"SHA256SUMS does not bind initial flash input: {name}")
        source[offset] = data
    if source["0x20000"] != app:
        raise ValueError("application changed during verification")
    # Patch only the bootloader header with esptool. Preserve the signed app
    # byte-for-byte; QIO startup uses DIO as documented by ESP Web Tools.
    with tempfile.TemporaryDirectory(prefix="prd-browser-merge-") as temporary:
        temp = Path(temporary)
        command = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "merge_bin",
                   "--output", str(temp / "merged.bin"), "--flash_mode", "dio",
                   "--flash_freq", "80m", "--flash_size", "16MB"]
        for offset, name in expected.items():
            path = temp / name
            path.write_bytes(source[offset])
            command.extend([offset, str(path)])
        process = subprocess.run(command, check=True, capture_output=True, text=True)
        if "warning" in (process.stdout + process.stderr).lower():
            raise ValueError("esptool warned while merging; candidate not emitted")
        merged = (temp / "merged.bin").read_bytes()
    if merged[0x20000:] != app or len(merged) > 16 * 1024 * 1024:
        raise ValueError("merged image does not preserve the exact signed application")
    for offset in ("0x8000", "0x10000"):
        start = int(offset, 0)
        if merged[start:start + len(source[offset])] != source[offset]:
            raise ValueError("merged image changed initial data")
    manifest = {"name": "Prusa Remote Display", "version": tag[1:],
                "new_install_prompt_erase": False, "new_install_improv_wait_time": 0,
                "builds": [{"chipFamily": "ESP32-S3",
                            "parts": [{"path": "merged-firmware.bin", "offset": 0}]}]}
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    release = {"available": True, "board": gate.BOARD_TARGET, "tag": tag, "commit": commit,
               "imageSha256": digest(merged), "manifestSha256": digest(manifest_bytes),
               "manifest": "firmware/web-manifest.json",
               "publisherSpkiSha256": summary["publicKeySpkiSha256"]}
    output.mkdir(parents=True)
    (output / "merged-firmware.bin").write_bytes(merged)
    (output / "web-manifest.json").write_bytes(manifest_bytes)
    (output / "release.json").write_text(json.dumps(release, indent=2, sort_keys=True) + "\n")
    (output / "SHA256SUMS").write_text("".join(
        f"{digest(path.read_bytes())}  {path.name}\n"
        for path in sorted(output.iterdir())))
    return release


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--verification-key", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = prepare(args.bundle, args.tag, args.commit, args.verification_key, args.output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"installer candidate: FAIL: {error}", file=sys.stderr)
        return 1
    print("installer candidate: PASS (offline preparation only; no USB or publication proof)")
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
