# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Release discovery and explicit initial-install contracts. No startup USB access."""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import re
import ssl
import tempfile
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlsplit
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, HTTPSHandler, Request, build_opener

REPOSITORY = "bigMOTOR/prusa-remote-display"
API = f"https://api.github.com/repos/{REPOSITORY}"
BOARD = "waveshare_esp32_s3_lcd_2_8"
APP_VERSION = "0.1.0-dev"
MAX_TOTAL = 20 * 1024 * 1024
REGIONS = {"0x0": "bootloader.bin", "0x8000": "partition-table.bin",
           "0x10000": "ota_data_initial.bin", "0x20000": "prusa_remote_display.bin"}
LIMITS = {"manifest.json": 8192, "SHA256SUMS": 8192, "flash_args.json": 8192,
          "bootloader.bin": 0x8000, "partition-table.bin": 0x8000,
          "ota_data_initial.bin": 0x10000, "prusa_remote_display.bin": 0x680000}


class InstallError(ValueError):
    """A readable, fail-closed installation error."""


def unique_json(data: bytes):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise InstallError(f"Duplicate metadata field: {key}")
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=unique)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise InstallError("Release metadata is unreadable.") from error


def validate_url(url: str, *, download=False):
    value = urlsplit(url)
    hosts = {"api.github.com"}
    if download:
        hosts |= {"github.com", "release-assets.githubusercontent.com"}
    if value.scheme != "https" or value.hostname not in hosts or value.port not in (None, 443) \
            or value.username or value.password or value.fragment:
        raise InstallError("Release download must use the official HTTPS service.")
    return url


class SafeRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        validate_url(newurl, download=True)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def get_bytes(url: str, limit: int, *, download=False) -> bytes:
    import certifi
    validate_url(url, download=download)
    request = Request(url, headers={"User-Agent": f"PRD-Desktop/{APP_VERSION}",
                                   "Accept": "application/octet-stream" if download else
                                   "application/vnd.github+json"})
    # Bundle the CA store; a packaged Mac app must not depend on Homebrew's paths.
    context = ssl.create_default_context(cafile=certifi.where())
    try:
        with build_opener(SafeRedirect(), HTTPSHandler(context=context)).open(request, timeout=20) as response:
            data = response.read(limit + 1)
    except HTTPError as error:
        if error.code == 404:
            if download:
                message = "A required firmware file is unavailable. Recheck the release before retrying."
            elif url == f"{API}/releases/latest":
                message = "No firmware release is published yet. Try again after the first release."
            else:
                message = "The firmware release reference is unavailable. Recheck the release."
        elif error.code in (403, 429):
            message = "GitHub refused this request or reached a request limit. Try again later."
        else:
            message = "GitHub could not provide the firmware package. Try again later."
        raise InstallError(message) from error
    except (URLError, TimeoutError, ConnectionError) as error:
        raise InstallError("The connection to GitHub failed or timed out. Check your connection and try again.") from error
    if not data or len(data) > limit:
        raise InstallError("Release file is empty or exceeds the permitted size.")
    return data


@dataclass(frozen=True)
class Release:
    tag: str
    commit: str
    assets: tuple[tuple[str, str], ...]


def discover(fetch=get_bytes) -> Release:
    """Latest must be an official firmware release, never an application-only tag."""
    value = unique_json(fetch(f"{API}/releases/latest", 256 * 1024))
    if not isinstance(value, dict):
        raise InstallError("Unexpected release response.")
    tag = value.get("tag_name", "")
    if type(tag) is not str or re.fullmatch(r"v\d+\.\d+\.\d+", tag) is None or \
            value.get("draft") is not False or value.get("prerelease") is not False or \
            value.get("immutable") is not True:
        raise InstallError("No compatible stable firmware release is available.")
    assets = {}
    records = value.get("assets")
    if not isinstance(records, list):
        raise InstallError("Release assets are missing.")
    for item in records:
        if not isinstance(item, dict):
            raise InstallError("Invalid release asset.")
        name = item.get("name")
        if name not in LIMITS:
            continue
        if name in assets:
            raise InstallError("Duplicate release asset.")
        expected = f"https://github.com/{REPOSITORY}/releases/download/{tag}/{name}"
        if item.get("browser_download_url") != expected or item.get("state") != "uploaded" or \
                type(item.get("size")) is not int or not 0 < item["size"] <= LIMITS[name]:
            raise InstallError("Release asset identity or size is invalid.")
        assets[name] = expected
    if set(assets) != set(LIMITS):
        raise InstallError("This release has no complete initial-installation package.")
    resolved = unique_json(fetch(f"{API}/commits/{tag}", 512 * 1024))
    commit = resolved.get("sha", "") if isinstance(resolved, dict) else ""
    if type(commit) is not str or re.fullmatch(r"[0-9a-f]{40}", commit) is None:
        raise InstallError("The release commit could not be verified.")
    return Release(tag, commit, tuple(sorted(assets.items())))


def load_gate(resource_root: Path):
    path = resource_root / "firmware/tools/check-release-assets.py"
    spec = importlib.util.spec_from_file_location("prd_desktop_release_gate", path)
    if spec is None or spec.loader is None:
        raise InstallError("Application release verifier is missing.")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_inputs(directory: Path, release: Release, gate):
    summary = gate.verify(directory, release.tag, release.commit)
    checksums = gate._checksums(directory / "SHA256SUMS")
    source = {}
    for name, limit in LIMITS.items():
        path = gate._regular_file(directory, name, limit)
        data = path.read_bytes()
        if name != "SHA256SUMS" and checksums.get(name) != hashlib.sha256(data).hexdigest():
            raise InstallError(f"Release checksum mismatch: {name}")
        source[name] = data
    flash = unique_json(source["flash_args.json"])
    if not isinstance(flash, dict) or flash.get("flash_files") != REGIONS:
        raise InstallError("Unsupported initial flash layout.")
    for region, (offset, name) in zip(("bootloader", "partition-table", "otadata", "app"),
                                    REGIONS.items()):
        record = flash.get(region)
        if not isinstance(record, dict) or record.get("offset") != offset or \
                record.get("file") != name or record.get("encrypted", "false") != "false":
            raise InstallError("Initial flash region records disagree.")
    settings = flash.get("flash_settings", {})
    extra = flash.get("extra_esptool_args", {})
    if not isinstance(settings, dict) or not isinstance(extra, dict) or \
            settings.get("flash_mode") not in ("qio", "dio") or \
            settings.get("flash_freq") != "80m" or settings.get("flash_size") not in ("keep", "16MB") \
            or extra.get("chip") != "esp32s3":
        raise InstallError("Unsupported board flash parameters.")
    return summary, source


def verify_signature(app: bytes, public_key: bytes, expected_identity: str):
    import espsecure
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    key = serialization.load_pem_public_key(public_key)
    if not isinstance(key, rsa.RSAPublicKey) or key.key_size != 3072:
        raise InstallError("Publisher verification key is invalid.")
    der = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    if hashlib.sha256(der).hexdigest() != expected_identity:
        raise InstallError("The release is from a different publisher.")
    with contextlib.redirect_stdout(io.StringIO()):
        espsecure.verify_signature(SimpleNamespace(version="2", datafile=io.BytesIO(app),
                                                   keyfile=io.BytesIO(public_key), hsm=False))


@dataclass(frozen=True)
class VerifiedImage:
    release: Release
    data: bytes
    sha256: str


def prepare(release: Release, public_key: bytes, resource_root: Path, fetch=get_bytes) -> VerifiedImage:
    if not public_key:
        raise InstallError("Publisher trust is not configured. Installation is disabled.")
    if set(dict(release.assets)) != set(LIMITS) or len(release.assets) != len(LIMITS):
        raise InstallError("Incomplete installation package.")
    with tempfile.TemporaryDirectory(prefix="prd-desktop-release-") as temporary:
        directory = Path(temporary)
        total = 0
        for name, url in release.assets:
            expected = f"https://github.com/{REPOSITORY}/releases/download/{release.tag}/{name}"
            if url != expected:
                raise InstallError("Release asset has changed identity.")
            data = fetch(url, LIMITS[name], download=True)
            total += len(data)
            if not data or len(data) > LIMITS[name] or total > MAX_TOTAL:
                raise InstallError("Installation package exceeds its size budget.")
            (directory / name).write_bytes(data)
        summary, source = check_inputs(directory, release, load_gate(resource_root))
        verify_signature(source[REGIONS["0x20000"]], public_key, summary["publicKeySpkiSha256"])
        from esptool.cmds import merge_bin
        output = io.StringIO()
        # The v4 CLI's argparse file objects are not closed by main(). Keep ownership
        # explicit here; all regions were size/overlap checked before using this API.
        with contextlib.ExitStack() as files, contextlib.redirect_stdout(output), \
                contextlib.redirect_stderr(output):
            pairs = [(int(offset, 0), files.enter_context((directory / name).open("rb")))
                     for offset, name in REGIONS.items()]
            merge_bin(SimpleNamespace(chip="esp32s3", addr_filename=pairs,
                                      output=str(directory / "merged.bin"), format="raw",
                                      target_offset=0, fill_flash_size=None, flash_mode="dio",
                                      flash_freq="80m", flash_size="16MB"))
        if "warning" in output.getvalue().lower():
            raise InstallError("The image merger reported a warning: " + output.getvalue()[:500])
        merged = (directory / "merged.bin").read_bytes()
        if merged[0x20000:] != source[REGIONS["0x20000"]] or len(merged) > 16 * 1024 * 1024:
            raise InstallError("Merged image changed the signed application.")
        for offset in ("0x8000", "0x10000"):
            data = source[REGIONS[offset]]
            start = int(offset, 0)
            if merged[start:start + len(data)] != data:
                raise InstallError("Merged image changed initial data.")
        return VerifiedImage(release, merged, hashlib.sha256(merged).hexdigest())


@dataclass
class Session:
    image: VerifiedImage | None = None
    port: str = ""
    board_confirmed: bool = False
    erase_confirmed: bool = False
    busy: bool = False
    enabled: bool = False

    @property
    def can_install(self):
        return bool(self.enabled and self.image and self.port.strip() and self.board_confirmed
                    and self.erase_confirmed and not self.busy)


def install(session: Session, confirmed_port: str, runner=None):
    """Only called after explicit user confirmation. No force/eFuse options."""
    if not session.can_install or confirmed_port != session.port:
        raise InstallError("Confirm the board, selected USB port and erase warning first.")
    image = session.image
    if hashlib.sha256(image.data).hexdigest() != image.sha256:
        raise InstallError("Verified firmware changed before installation.")
    port = session.port
    session.busy = True
    try:
        with tempfile.TemporaryDirectory(prefix="prd-desktop-write-") as temporary:
            path = Path(temporary) / "verified.bin"
            path.write_bytes(image.data)
            if runner is None:
                import esptool
                runner = esptool.main
            runner(["--chip", "esp32s3", "--port", port, "--baud", "460800",
                    "--before", "default_reset", "--after", "hard_reset", "write_flash",
                    "--erase-all", "--flash_mode", "keep", "--flash_freq", "keep",
                    "--flash_size", "16MB", "0x0", str(path)])
    finally:
        session.busy = False
