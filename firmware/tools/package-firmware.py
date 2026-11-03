#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Package a built firmware profile into release artifacts.

Produces the artifact set required by Bootstrap Spec §13.4, plus the OTA manifest
fields §14.6 says Milestone 0 shall generate:

    bootloader.bin
    partition-table.bin
    ota_data_initial.bin
    prusa_remote_display.bin
    flash_args.json
    manifest.json
    SHA256SUMS
    build-info.txt
    size-report.txt

Usage, from the repository root or from firmware/:

    ./firmware/tools/package-firmware.py --profile release
    ./firmware/tools/package-firmware.py --profile debug --no-archive

DETERMINISM. Two packaging runs over the same build produce byte-identical
output. Every dictionary is written with sorted keys, SHA256SUMS is sorted by
filename, and the archive is built with fixed member order and with mtime, uid,
gid, uname and gname normalised — the build epoch from the firmware's own
metadata is used as the timestamp, so nothing reads the wall clock.

UNSIGNED MODE: only the Python standard library, and only files inside
the build directory. ESP-IDF need not be activated: the size report is derived
from the partition CSV and the image sizes rather than from `idf.py size`.

SIGNED MODE: --signed-app IMAGE --verification-key PUBLIC.pem verifies an externally
signed copy using ESP-IDF's Python environment, then creates artifacts-signed and
a separate -signed archive. No private key is accepted or used. The source build
and ordinary unsigned packaging are untouched. Signatures use randomized RSA-PSS:
repackaging identical signed bytes is deterministic; re-signing need not be.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import sys
import struct
import tarfile
from pathlib import Path

# §13.5: release packaging shall scan binaries, map files, logs and test fixtures
# for secret markers and private test values. This list is deliberately about
# artifact *content*, which is a different problem from the metadata-field scan in
# print-build-info.py, so the two lists are separate rather than shared.
#
# A FIELD NAME IS NOT A SECRET. An assignment pattern is only evidence of a leak
# when a value follows the separator. Two forms carry no value at all and are
# unavoidable in any firmware that handles credentials or joins a network:
#
#   " ssid="            a label, where the string literal ends at the separator.
#                       secrets::describe() names each field it reports presence
#                       for, which is the redaction working, not failing.
#   "...for ssid:%s"    a log format, where ESP-IDF's wpa_supplicant names the
#                       field and substitutes the value at runtime.
#
# Matching those rejected every build that linked either, while detecting no
# secret. Requiring a value keeps the gate: `password=hunter2` still fails. A
# printf conversion is excluded specifically rather than '%' as a whole, so a
# percent-encoded value such as `password=%41%42` is still caught. `%%` is spelled
# as its own alternative rather than folded into the conversion characters,
# because a '%' there lets the width digits of `%41%` pass as a conversion and
# swallows exactly that percent-encoded case.
VALUE_FOLLOWS = rb"[ \t]*(?!%%|%[-+ #0-9.*hlLqjzt]*[diouxXeEfgGaAcsp])[^\s\x00]"

SECRET_PATTERNS = (
    (rb"/Users/[^/\s\"]{1,64}", "macOS home directory path"),
    (rb"/home/[^/\s\"]{1,64}", "Linux home directory path"),
    (rb"(?i)\bpassword\s*[=:]" + VALUE_FOLLOWS, "password assignment"),
    (rb"(?i)\bapi[_-]?key\s*[=:]" + VALUE_FOLLOWS, "API key assignment"),
    (rb"(?i)\bssid\s*[=:]" + VALUE_FOLLOWS, "Wi-Fi SSID assignment"),
)

# Two classes of scan target, because a home-directory path means different
# things in each:
#
#   SHIPPED   the binaries that reach a user's board. An absolute build path here
#             is a real §14.5 leak and fails packaging.
#   DIAGNOSTIC the linker map and ESP-IDF's build logs, which record toolchain and
#             source paths by design and are scanned but never shipped. A path
#             finding is a note; a credential or key is still an error.
#
# The distinction is by role, not by filename: ESP-IDF's build logs are written as
# `idf_py_stdout_output_<pid>` with no extension at all, so a suffix rule silently
# treats them as shipped artifacts and fails every local build.
PATH_FINDING_MARKER = "directory path"

# §13.5's private-key check, as a whole PEM BLOCK rather than a delimiter.
#
# ⛔ WHAT THE STANDALONE MARKER GOT WRONG. It was `BEGIN (RSA |EC )?PRIVATE KEY`, and it failed every
# release the moment Milestone 4 added esp_http_client: that pulls in esp-tls and mbedTLS, and
# libmbedcrypto's PEM parser carries the delimiters it looks FOR as string literals —
# `-----BEGIN RSA PRIVATE KEY-----`, `-----BEGIN EC PRIVATE KEY-----`,
# `-----BEGIN ENCRYPTED PRIVATE KEY-----` and their END forms, each a NUL-terminated constant in
# `pem.c.obj` and `pkparse.c.obj`. A parser's format string is not a key, and a gate that cannot tell
# the difference is a gate that gets bypassed.
#
# ⬅️ SO THE TEST IS THE THING A LEAK ACTUALLY LOOKS LIKE: a BEGIN delimiter, then a plausible base64
# payload, then the MATCHING END delimiter. A lone header, a lone footer, or a header immediately
# followed by anything that is not encoded key material does not match. The label is captured and
# back-referenced, so a BEGIN of one kind cannot be closed by an END of another.
#
# Deliberately NOT a cryptographic parse. This does not need to decode a key to know that a couple of
# kilobytes of base64 wrapped in matching private-key delimiters has no business in a release
# artifact — and requiring a successful parse would be a second, weaker gate that a malformed but
# still-sensitive dump would slip through.
#
# Every quantifier is bounded, so this cannot backtrack pathologically over a 1.7 MB binary. In
# practice `-----BEGIN ` occurs a handful of times and each attempt dies on the next byte.
PEM_LABEL = rb"(?:[A-Z][A-Z0-9]*[ ])*PRIVATE KEY"

PEM_PRIVATE_KEY_BLOCK = re.compile(
    # The BEGIN delimiter, on its own line.
    rb"-----BEGIN (" + PEM_LABEL + rb")-----[ \t]*\r?\n"
    # RFC 1421 headers, which an encrypted PEM key carries (Proc-Type, DEK-Info). Optional, bounded,
    # and followed by the blank line the format requires.
    rb"(?:(?:[A-Za-z][A-Za-z0-9-]{0,30}:[^\n]{1,200}\r?\n){1,4}\r?\n)?"
    # The payload: base64 lines. At least one, and the total is length-checked below.
    rb"((?:[A-Za-z0-9+/][A-Za-z0-9+/=]{3,75}[ \t]*\r?\n){1,4096})"
    # The matching END delimiter.
    rb"-----END \1-----"
)

# A real private key is hundreds of base64 characters; the smallest useful EC key is over a hundred.
# This floor is what stops a few stray encoded-looking bytes between two delimiters from counting as
# key material, without needing to know which curve or modulus size produced it.
PEM_MINIMUM_PAYLOAD_CHARS = 64


def find_pem_private_key(data: bytes) -> str | None:
    """Return the BEGIN delimiter of a whole PEM private-key block, or None.

    Only the delimiter is returned: the finding must never carry the key material into a log.
    """
    for match in PEM_PRIVATE_KEY_BLOCK.finditer(data):
        payload = re.sub(rb"[^A-Za-z0-9+/=]", b"", match.group(2))

        if len(payload) >= PEM_MINIMUM_PAYLOAD_CHARS:
            return match.group(1).decode("ascii", "replace")

    return None


def firmware_root() -> Path:
    return Path(__file__).resolve().parent.parent


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def portable_flash_args(build_dir: Path) -> dict:
    """Validate all initial-flash inputs and make their paths bundle-relative.

    An app-only OTA image is not an initial-install bundle. In particular the
    dual-slot layout needs its initial OTA selection data. Fail before creating
    delivery artifacts if a file or a duplicated IDF region record is missing.
    Offsets and flash settings remain those emitted by the exact build.
    """
    args = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    expected = {
        "bootloader": "bootloader/bootloader.bin",
        "partition-table": "partition_table/partition-table.bin",
        "otadata": "ota_data_initial.bin",
        "app": "prusa_remote_display.bin",
    }
    files = args.get("flash_files")
    if not isinstance(files, dict) or len(files) != len(expected):
        raise ValueError("initial flash requires exactly four build regions")
    seen = set()
    portable = {}
    ranges = []
    for region, source_name in expected.items():
        basename = Path(source_name).name
        record = args.get(region)
        if not isinstance(record, dict) or not isinstance(record.get("file"), str):
            raise ValueError(f"missing initial flash region: {region}")
        offset = record.get("offset")
        name = record["file"]
        if offset not in files or files[offset] != name or offset in seen:
            raise ValueError(f"inconsistent initial flash region: {region}")
        if name != source_name:
            raise ValueError(f"invalid initial flash path: {region}")
        path = (build_dir / name).resolve()
        if not path.is_relative_to(build_dir.resolve()) or not path.is_file():
            raise ValueError(f"missing initial flash input: {region}")
        address = int(offset, 0)
        if address < 0 or path.stat().st_size == 0:
            raise ValueError(f"invalid initial flash range: {region}")
        ranges.append((address, address + path.stat().st_size))
        seen.add(offset)
        portable[offset] = basename
        record["file"] = basename
    ranges.sort()
    if any(end > start for (_, end), (start, _) in zip(ranges, ranges[1:])):
        raise ValueError("overlapping initial flash regions")
    args["flash_files"] = portable
    return args


def load_build_metadata(build_dir: Path) -> dict[str, str]:
    """Reuse print-build-info.py so the two tools cannot disagree."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        import importlib.util

        spec = importlib.util.spec_from_file_location(
            "prd_print_build_info", Path(__file__).resolve().parent / "print-build-info.py"
        )
        if spec is None or spec.loader is None:
            raise SystemExit("error: cannot load print-build-info.py")

        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module.collect(build_dir)
    finally:
        sys.path.pop(0)


def partition_layout(firmware_dir: Path) -> list[dict[str, object]]:
    """Parse partitions.csv without ESP-IDF."""
    table = firmware_dir / "partitions.csv"
    partitions: list[dict[str, object]] = []

    with table.open(encoding="utf-8") as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#"):
                continue
            if len(row) < 5:
                continue

            name, kind, subtype, offset, size = (cell.strip() for cell in row[:5])
            partitions.append(
                {
                    "name": name,
                    "type": kind,
                    "subtype": subtype,
                    "offset": int(offset, 0),
                    "size": int(size, 0),
                }
            )

    return partitions


def size_report(app_bin: Path, partitions: list[dict[str, object]]) -> str:
    app_size = app_bin.stat().st_size
    app_slots = [p for p in partitions if str(p["subtype"]).startswith("ota_")]
    smallest = min((int(p["size"]) for p in app_slots), default=0)
    flash_end = max((int(p["offset"]) + int(p["size"]) for p in partitions), default=0)

    lines = [
        "Prusa Remote Display size report",
        "",
        f"application image      {app_size} bytes (0x{app_size:x})",
    ]

    if smallest:
        free = smallest - app_size
        percent = (free * 100) // smallest
        lines += [
            f"smallest app partition {smallest} bytes (0x{smallest:x})",
            f"free in that partition {free} bytes ({percent}%)",
            f"fits both OTA slots    {'yes' if app_size <= smallest else 'NO'}",
        ]

    lines += [
        f"partition table extent {flash_end} bytes (0x{flash_end:x})",
        f"exactly 16 MB          {'yes' if flash_end == 16 * 1024 * 1024 else 'NO'}",
        "",
        "partitions:",
    ]

    for part in partitions:
        lines.append(
            f"  {str(part['name']):14} {str(part['type']):6} {str(part['subtype']):10}"
            f" offset=0x{int(part['offset']):06x} size=0x{int(part['size']):06x}"
        )

    return "\n".join(lines) + "\n"


def scan_for_secrets(targets: list[tuple[Path, bool]]) -> tuple[list[str], list[str]]:
    """Scan per §13.5. `targets` pairs each path with whether it is shipped.

    Returns (errors, notes). Errors fail packaging; notes are printed.
    """
    errors: list[str] = []
    notes: list[str] = []

    for path, shipped in targets:
        if not path.is_file():
            continue

        data = path.read_bytes()

        # The PEM block check first, so a finding names the block rather than whatever assignment
        # pattern happens to also match inside a key file.
        label = find_pem_private_key(data)

        if label is not None:
            errors.append(f"{path.name}: private key block: '-----BEGIN {label}-----'")

        for pattern, description in SECRET_PATTERNS:
            match = re.search(pattern, data)
            if match is None:
                continue

            excerpt = match.group(0)[:60].decode("utf-8", "replace")
            message = f"{path.name}: {description}: {excerpt!r}"

            if not shipped and PATH_FINDING_MARKER in description:
                notes.append(message + "  (diagnostic file, not shipped)")
            else:
                errors.append(message)

    return errors, notes


def image_identity(app_bin: Path, metadata: dict[str, str]) -> int:
    """Check placement and bind build metadata; this is NOT signature verification."""
    data = app_bin.read_bytes()
    if len(data) < 376 or data[0] != 0xe9 or data[1] == 0:
        raise ValueError("missing OTA image identity")
    segment_size = struct.unpack_from("<I", data, 28)[0]
    if not 344 <= segment_size <= len(data) - 32:
        raise ValueError("OTA identity outside first image segment")
    magic, version, schema, board, product, reserved = struct.unpack_from("<8sII32s32s8s", data, 288)
    if magic != b"PRDOTA01" or version != 1 or schema == 0 or reserved != bytes(8):
        raise ValueError("unsupported OTA image identity")
    def field(value: str) -> bytes:
        encoded = value.encode("ascii")
        if not encoded or len(encoded) > 31 or b"\0" in encoded:
            raise ValueError("OTA identity field exceeds its canonical bounds")
        return encoded.ljust(32, b"\0")
    if (board != field(metadata["boardTarget"]) or product != field(metadata["productVersion"])
            or data[48:80] != product or data[80:112] != field("prusa_remote_display")
            or struct.unpack_from("<I", data, 32)[0] != 0xabcd5432):
        raise ValueError("image identity differs from build metadata")
    return schema


def build_manifest(
    metadata: dict[str, str],
    app_bin: Path,
    partitions: list[dict[str, object]],
) -> dict[str, object]:
    """OTA manifest, §14.6. Fields that are knowable now, and no more."""
    app_slots = [p for p in partitions if str(p["subtype"]).startswith("ota_")]

    return {
        "schemaVersion": 1,
        "productVersion": metadata["productVersion"],
        "boardTarget": metadata["boardTarget"],
        "chipTarget": metadata["target"],
        "buildProfile": metadata["buildProfile"],
        "gitCommit": metadata["gitCommit"],
        "gitDescribe": metadata["gitDescribe"],
        "idfVersion": metadata["idfVersion"],
        "buildEpoch": metadata["buildEpoch"],
        "releaseTag": metadata["releaseTag"],
        "dirtyTree": metadata["dirtyTree"] == "true",
        "image": {
            "name": app_bin.name,
            "sizeBytes": app_bin.stat().st_size,
            "sha256": sha256_of(app_bin),
        },
        "compatibility": {
            "minBootloaderSchema": image_identity(app_bin, metadata),
            "otaSlotSizeBytes": min((int(p["size"]) for p in app_slots), default=0),
            "otaSlotCount": len(app_slots),
        },
        # §14.6 lists signing metadata among the eventual fields. Signing is a
        # deferred decision (§21: "OTA signature format and update transport"), so
        # the shape is declared and left unsigned rather than invented.
        "signing": {
            "algorithm": None,
            "signature": None,
            "note": "unsigned; OTA signature format is a deferred decision",
        },
    }


def verify_signed_app(app_bin: Path, signed_app: Path, public_key: Path,
                      partitions: list[dict[str, object]]) -> tuple[bytes, dict[str, str]]:
    """Verify an externally signed copy; never load a private key or sign here.

    Optional ESP-IDF tooling dependency, absent from ordinary unsigned packaging.
    Exact original bytes plus IDF padding bind the signature to this build.
    This does not sign the JSON manifest or establish device-side OTA enforcement.
    """
    import contextlib
    from types import SimpleNamespace

    import espsecure
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    original = app_bin.read_bytes()
    signed = signed_app.read_bytes()
    padded_size = ((len(original) + 4095) // 4096) * 4096
    expected_prefix = original + b"\xff" * (padded_size - len(original))
    if not original or len(signed) != padded_size + 4096 or signed[:padded_size] != expected_prefix:
        raise ValueError("signed image is not the exact build plus signature-sector padding")
    if signed[padded_size + espsecure.SIG_BLOCK_SIZE:] != b'\xff' * (4096 - espsecure.SIG_BLOCK_SIZE):
        raise ValueError("expected exactly one signature block in the first position")
    slots = [int(p['size']) for p in partitions
             if p.get('type') == 'app' and str(p.get('subtype', '')).startswith('ota_')]
    if len(slots) < 2 or min(slots) < len(signed):
        raise ValueError("signed image does not fit the local dual-slot partition layout")
    key_data = public_key.read_bytes()
    key = serialization.load_pem_public_key(key_data)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        raise ValueError("expected an RSA-3072 public verification key")
    # Only one native software-verification key is supported by our pinned IDF.
    # espsecure verification supplies the cryptographic check, not just a digest.
    with contextlib.redirect_stdout(io.StringIO()):
        espsecure.verify_signature(SimpleNamespace(version='2', datafile=io.BytesIO(signed),
                                                   keyfile=io.BytesIO(key_data), hsm=False))
    der = key.public_bytes(serialization.Encoding.DER,
                           serialization.PublicFormat.SubjectPublicKeyInfo)
    return signed, {
        'algorithm': 'RSA-PSS-SHA256',
        'signature': 'embedded-esp-secure-boot-v2',
        'publicKeySpkiSha256': hashlib.sha256(der).hexdigest(),
        'note': 'application image signature verified; bootloader, partition table and JSON manifest are unsigned',
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--profile",
        default="release",
        choices=("debug", "hardware-validation", "release"),
        help="build profile to package (default: release)",
    )
    parser.add_argument("--build-dir", type=Path, help="explicit build directory")
    parser.add_argument("--signed-app", type=Path, help="externally signed copy of this exact build")
    parser.add_argument("--verification-key", type=Path, help="trusted RSA-3072 PUBLIC key for --signed-app")
    parser.add_argument(
        "--no-archive", action="store_true", help="write the artifact directory only"
    )
    parser.add_argument(
        "--allow-secrets",
        action="store_true",
        help="report the §13.5 scan but do not fail on it (diagnostic use only)",
    )
    args = parser.parse_args()
    if bool(args.signed_app) != bool(args.verification_key):
        parser.error("--signed-app and --verification-key must be supplied together")

    firmware_dir = firmware_root()
    build_dir = args.build_dir or (firmware_dir / "build" / args.profile)

    if not build_dir.is_dir():
        raise SystemExit(
            f"error: no build directory at {build_dir}\n"
            f"  Build it first: ./firmware/tools/build.sh {args.profile}"
        )

    app_bin = build_dir / "prusa_remote_display.bin"
    bootloader_bin = build_dir / "bootloader" / "bootloader.bin"
    partition_bin = build_dir / "partition_table" / "partition-table.bin"
    flasher_args = build_dir / "flasher_args.json"

    for required in (app_bin, bootloader_bin, partition_bin, flasher_args):
        if not required.is_file():
            raise SystemExit(
                f"error: missing build output {required}\n"
                f"  The build is incomplete; run ./firmware/tools/build.sh {args.profile}"
            )

    metadata = load_build_metadata(build_dir)
    image_identity(app_bin, metadata) # Fail before creating any delivery artifacts.
    flash_args = portable_flash_args(build_dir)

    if metadata["buildProfile"] != args.profile and args.build_dir is None:
        raise SystemExit(
            f"error: build directory holds profile '{metadata['buildProfile']}' "
            f"but --profile is '{args.profile}'\n"
            f"  Rebuild, or pass --build-dir explicitly."
        )

    partitions = partition_layout(firmware_dir)

    signing = None
    signed_bytes = None
    if args.signed_app:
        signed_bytes, signing = verify_signed_app(app_bin, args.signed_app, args.verification_key, partitions)
    artifacts_dir = build_dir / ("artifacts-signed" if signing else "artifacts")
    # Signed delivery never overwrites an earlier bundle or unsigned artifacts.
    artifacts_dir.mkdir(parents=True, exist_ok=signing is None)

    # 1-3. Binaries, copied rather than linked so the artifact directory stands
    #      alone once the build tree is discarded.
    (artifacts_dir / "bootloader.bin").write_bytes(bootloader_bin.read_bytes())
    (artifacts_dir / "partition-table.bin").write_bytes(partition_bin.read_bytes())
    (artifacts_dir / "ota_data_initial.bin").write_bytes(
        (build_dir / "ota_data_initial.bin").read_bytes())
    packaged_app = artifacts_dir / app_bin.name
    packaged_app.write_bytes(signed_bytes if signed_bytes is not None else app_bin.read_bytes())

    # 4. flash_args.json. §13.4 names it with a .json suffix; ESP-IDF writes the
    #    paths refer to this standalone bundle, not the original build tree.
    (artifacts_dir / "flash_args.json").write_text(
        json.dumps(flash_args, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    # 5. build-info.txt
    build_info_lines = [f"{name} = {metadata[name]}" for name in metadata]
    (artifacts_dir / "build-info.txt").write_text(
        "\n".join(build_info_lines) + "\n", encoding="utf-8"
    )

    # 6. size-report.txt
    (artifacts_dir / "size-report.txt").write_text(
        size_report(packaged_app, partitions), encoding="utf-8"
    )

    # 7. manifest.json
    manifest = build_manifest(metadata, artifacts_dir / app_bin.name, partitions)
    if signing:
        manifest['signing'] = signing
    (artifacts_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    # §13.5 secret scan, before checksums so a failure stops packaging.
    scan_targets: list[tuple[Path, bool]] = [
        (packaged_app, True),
        (bootloader_bin, True),
        (partition_bin, True),
        (artifacts_dir / "ota_data_initial.bin", True),
        (build_dir / "prusa_remote_display.map", False),
    ]

    if (build_dir / "log").is_dir():
        scan_targets += [(path, False) for path in sorted((build_dir / "log").glob("*"))]

    errors, notes = scan_for_secrets(scan_targets)

    print(f"secret scan: {len(scan_targets)} target(s), "
          f"{sum(1 for _, shipped in scan_targets if shipped)} shipped")
    for note in notes:
        print(f"  note: {note}")

    if errors:
        print("§13.5 secret scan FAILED:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        if not args.allow_secrets:
            return 1

    # 8. SHA256SUMS, sorted by filename, in the sha256sum(1) format.
    checksum_targets = sorted(
        path for path in artifacts_dir.iterdir() if path.name != "SHA256SUMS"
    )
    checksum_lines = [f"{sha256_of(path)}  {path.name}" for path in checksum_targets]
    (artifacts_dir / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="utf-8"
    )

    print(f"artifacts: {artifacts_dir}")
    for path in sorted(artifacts_dir.iterdir()):
        print(f"  {path.name:32} {path.stat().st_size:>9} bytes")

    if args.no_archive:
        return 0

    # Archive name carries profile and version (§13.4).
    archive_name = (
        f"prusa-remote-display-{metadata['productVersion']}-{metadata['buildProfile']}"
        f"{'-signed' if signing else ''}.tar.gz"
    )
    archive_path = build_dir / archive_name

    # Deterministic archive: fixed member order, and every varying field
    # normalised. mtime comes from the firmware's own build epoch, so the archive
    # is a function of the build and not of when packaging ran.
    try:
        epoch = int(metadata["buildEpoch"])
    except ValueError:
        epoch = 0

    def normalise(info: tarfile.TarInfo) -> tarfile.TarInfo:
        info.mtime = epoch
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mode = 0o644
        return info

    # gzip's own header embeds an mtime; writing through a GzipFile with mtime=0
    # keeps the compressed stream reproducible too.
    import gzip

    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as archive:
        for path in sorted(artifacts_dir.iterdir()):
            archive.add(path, arcname=f"{archive_name[: -len('.tar.gz')]}/{path.name}",
                        filter=normalise)

    with archive_path.open("wb") as handle:
        with gzip.GzipFile(filename="", mode="wb", fileobj=handle, mtime=0) as compressed:
            compressed.write(raw.getvalue())

    print(f"archive:   {archive_path.name}  {archive_path.stat().st_size} bytes")
    print(f"           sha256 {sha256_of(archive_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
