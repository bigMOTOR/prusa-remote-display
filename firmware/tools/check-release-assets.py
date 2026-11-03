#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Verify that a signed release artifact directory matches an intended tag and commit.

This is an offline consistency gate. It never signs, uploads, publishes or contacts hardware, and
it does not replace the cryptographic verification performed by package-firmware.py in signed mode.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

MANIFEST_NAME = "manifest.json"
IMAGE_NAME = "prusa_remote_display.bin"
CHECKSUMS_NAME = "SHA256SUMS"
MANIFEST_LIMIT = 8192
IMAGE_LIMIT = 0x680000
BOARD_TARGET = "waveshare_esp32_s3_lcd_2_8"
CHIP_TARGET = "esp32s3"
HEX64 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
MANIFEST_COMMIT = re.compile(r"[0-9a-f]{12,40}")
OFFICIAL_REPOSITORY = "bigMOTOR/prusa-remote-display"


class ReleaseAssetError(ValueError):
    """A release directory is inconsistent or not safe to publish."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseAssetError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def _regular_file(directory: Path, name: str, maximum: int | None = None) -> Path:
    path = directory / name
    if path.is_symlink() or not path.is_file():
        raise ReleaseAssetError(f"missing regular file: {name}")
    size = path.stat().st_size
    if size == 0:
        raise ReleaseAssetError(f"empty release file: {name}")
    if maximum is not None and size > maximum:
        raise ReleaseAssetError(f"{name} exceeds the device limit ({size} > {maximum})")
    return path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _member(value: Any, name: str, expected: type) -> Any:
    if not isinstance(value, dict) or name not in value or type(value[name]) is not expected:
        raise ReleaseAssetError(f"manifest member {name!r} has the wrong type or is missing")
    return value[name]


def _private_origin(origin: str) -> bool:
    try:
        parsed = urlsplit(origin)
        address = ipaddress.ip_address(parsed.hostname or "")
        return (
            parsed.scheme == "https"
            and parsed.port is not None
            and parsed.port > 0
            and origin == f"https://{address}:{parsed.port}"
            and isinstance(address, ipaddress.IPv4Address)
            and (
                address in ipaddress.ip_network("10.0.0.0/8")
                or address in ipaddress.ip_network("172.16.0.0/12")
                or address in ipaddress.ip_network("192.168.0.0/16")
            )
        )
    except ValueError:
        return False


def _checksums(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/\\]+)", line)
        if match is None:
            raise ReleaseAssetError(f"invalid SHA256SUMS line {line_number}")
        digest, name = match.groups()
        if name in entries:
            raise ReleaseAssetError(f"duplicate SHA256SUMS entry: {name}")
        entries[name] = digest
    return entries


def verify(
    directory: Path,
    expected_tag: str,
    expected_commit: str,
    local_origin: str | None = None,
) -> dict[str, str | int]:
    if not directory.is_dir():
        raise ReleaseAssetError(f"not an artifact directory: {directory}")
    if not expected_tag.startswith("v") or len(expected_tag) == 1:
        raise ReleaseAssetError("expected tag must start with v and include a version")
    if COMMIT.fullmatch(expected_commit) is None:
        raise ReleaseAssetError("expected commit must be a full lowercase 40-character SHA")
    if local_origin is not None and not _private_origin(local_origin):
        raise ReleaseAssetError("local origin must be an exact private IPv4 HTTPS origin")

    manifest_path = _regular_file(directory, MANIFEST_NAME, MANIFEST_LIMIT)
    image_path = _regular_file(directory, IMAGE_NAME, IMAGE_LIMIT)
    checksums_path = _regular_file(directory, CHECKSUMS_NAME)
    try:
        manifest = json.loads(
            manifest_path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ReleaseAssetError(f"invalid manifest JSON: {error}") from error
    if not isinstance(manifest, dict):
        raise ReleaseAssetError("manifest root must be an object")

    if _member(manifest, "schemaVersion", int) != 1:
        raise ReleaseAssetError("unsupported manifest schema")
    if _member(manifest, "productVersion", str) != expected_tag[1:]:
        raise ReleaseAssetError("productVersion does not match the intended tag")
    if _member(manifest, "releaseTag", str) != expected_tag:
        raise ReleaseAssetError("releaseTag does not match the intended tag")
    manifest_commit = _member(manifest, "gitCommit", str)
    if MANIFEST_COMMIT.fullmatch(manifest_commit) is None:
        raise ReleaseAssetError("manifest gitCommit must be a lowercase SHA prefix of 12 to 40 characters")
    if not expected_commit.startswith(manifest_commit):
        raise ReleaseAssetError("gitCommit does not match the intended commit")
    required_profile = "hardware-validation" if local_origin is not None else "release"
    if _member(manifest, "buildProfile", str) != required_profile:
        raise ReleaseAssetError(f"only the {required_profile} profile is permitted for this channel")
    if _member(manifest, "dirtyTree", bool):
        raise ReleaseAssetError("dirty-tree artifacts may not be published")
    if _member(manifest, "boardTarget", str) != BOARD_TARGET:
        raise ReleaseAssetError("unexpected board target")
    if _member(manifest, "chipTarget", str) != CHIP_TARGET:
        raise ReleaseAssetError("unexpected chip target")

    image = _member(manifest, "image", dict)
    declared_name = _member(image, "name", str)
    declared_size = _member(image, "sizeBytes", int)
    declared_digest = _member(image, "sha256", str)
    actual_size = image_path.stat().st_size
    actual_digest = _sha256(image_path)
    if declared_name != IMAGE_NAME:
        raise ReleaseAssetError("manifest image name is not the live OTA asset name")
    if declared_size != actual_size:
        raise ReleaseAssetError("manifest image size does not match the release image")
    if HEX64.fullmatch(declared_digest) is None or declared_digest != actual_digest:
        raise ReleaseAssetError("manifest image digest does not match the release image")
    if local_origin is not None:
        expected_url = f"{local_origin}/releases/latest".encode("ascii")
        official_url = (
            f"https://api.github.com/repos/{OFFICIAL_REPOSITORY}/releases/latest".encode("ascii")
        )
        image_bytes = image_path.read_bytes()
        if expected_url not in image_bytes or official_url in image_bytes:
            raise ReleaseAssetError("validation image is not pinned exclusively to its local origin")

    compatibility = _member(manifest, "compatibility", dict)
    minimum_schema = _member(compatibility, "minBootloaderSchema", int)
    slot_size = _member(compatibility, "otaSlotSizeBytes", int)
    slot_count = _member(compatibility, "otaSlotCount", int)
    if minimum_schema < 1:
        raise ReleaseAssetError("minimum bootloader schema must be positive")
    if slot_count != 2 or slot_size < actual_size:
        raise ReleaseAssetError("release image does not fit the declared dual-slot layout")

    signing = _member(manifest, "signing", dict)
    if _member(signing, "algorithm", str) != "RSA-PSS-SHA256":
        raise ReleaseAssetError("release manifest does not describe the required signature algorithm")
    if _member(signing, "signature", str) != "embedded-esp-secure-boot-v2":
        raise ReleaseAssetError("release manifest does not describe an embedded verified signature")
    key_identity = _member(signing, "publicKeySpkiSha256", str)
    if HEX64.fullmatch(key_identity) is None:
        raise ReleaseAssetError("publisher public-key identity is not a lowercase SHA-256 digest")

    checksums = _checksums(checksums_path)
    for name, path in ((MANIFEST_NAME, manifest_path), (IMAGE_NAME, image_path)):
        if checksums.get(name) != _sha256(path):
            raise ReleaseAssetError(f"SHA256SUMS does not bind {name}")

    return {
        "tag": expected_tag,
        "commit": expected_commit,
        "imageBytes": actual_size,
        "imageSha256": actual_digest,
        "publicKeySpkiSha256": key_identity,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("artifact_directory", type=Path)
    parser.add_argument("--tag", required=True, help="intended full release tag, for example v0.1.0")
    parser.add_argument("--commit", required=True, help="intended full 40-character release commit")
    parser.add_argument(
        "--local-origin",
        help="explicit private HTTPS origin for a hardware-validation-only bundle",
    )
    args = parser.parse_args()
    try:
        summary = verify(args.artifact_directory, args.tag, args.commit, args.local_origin)
    except (OSError, ReleaseAssetError) as error:
        print(f"release assets: FAIL: {error}", file=sys.stderr)
        return 1
    print("release assets: PASS")
    for name, value in summary.items():
        print(f"  {name}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
