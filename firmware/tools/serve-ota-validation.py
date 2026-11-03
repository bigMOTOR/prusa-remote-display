#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Serve one frozen OTA candidate over private-LAN HTTPS; never publish it."""

from __future__ import annotations

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import json
from pathlib import Path
import re
import ssl
import sys
from urllib.parse import urlsplit


MANIFEST_NAME = "manifest.json"
IMAGE_NAME = "prusa_remote_display.bin"
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
MANIFEST_LIMIT = 8192
IMAGE_LIMIT = 0x680000


class FixtureError(ValueError):
    """The local fixture is unsafe or inconsistent."""


def parse_origin(origin: str) -> tuple[str, int]:
    try:
        parsed = urlsplit(origin)
        port = parsed.port
    except ValueError as error:
        raise FixtureError("origin must be exact https://PRIVATE_IPV4:PORT") from error
    if (
        parsed.scheme != "https"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path
        or parsed.query
        or parsed.fragment
        or parsed.hostname is None
        or port is None
        or port == 0
        or origin != f"https://{parsed.hostname}:{port}"
    ):
        raise FixtureError("origin must be exact https://PRIVATE_IPV4:PORT")
    try:
        address = ipaddress.ip_address(parsed.hostname)
    except ValueError as error:
        raise FixtureError("origin must use a private IPv4 address") from error
    if not isinstance(address, ipaddress.IPv4Address) or not (
        address in ipaddress.ip_network("10.0.0.0/8")
        or address in ipaddress.ip_network("172.16.0.0/12")
        or address in ipaddress.ip_network("192.168.0.0/16")
    ):
        raise FixtureError("origin must use an RFC1918 private IPv4 address")
    return str(address), port


def load_candidate(directory: Path, tag: str, release_id: int, origin: str) -> dict[str, bytes]:
    if TAG.fullmatch(tag) is None or release_id <= 0:
        raise FixtureError("candidate needs a stable version tag and positive release ID")
    assets: dict[str, bytes] = {}
    for name, limit in ((MANIFEST_NAME, MANIFEST_LIMIT), (IMAGE_NAME, IMAGE_LIMIT)):
        path = directory / name
        if path.is_symlink() or not path.is_file() or not 0 < path.stat().st_size <= limit:
            raise FixtureError(f"candidate asset is missing, linked or out of bounds: {name}")
        assets[name] = path.read_bytes()
    try:
        manifest = json.loads(assets[MANIFEST_NAME])
    except (UnicodeError, json.JSONDecodeError) as error:
        raise FixtureError("manifest is not valid JSON") from error
    if not isinstance(manifest, dict) or manifest.get("releaseTag") != tag:
        raise FixtureError("manifest releaseTag does not match fixture tag")
    release = {
        "id": release_id,
        "url": f"{origin}/releases/{release_id}",
        "tag_name": tag,
        "draft": False,
        "prerelease": False,
        "immutable": True,
        "assets": [
            {
                "id": release_id * 10 + offset,
                "name": name,
                "size": len(content),
                "browser_download_url": f"{origin}/releases/download/{tag}/{name}",
            }
            for offset, (name, content) in enumerate(assets.items(), 1)
        ],
    }
    assets["release.json"] = json.dumps(release, separators=(",", ":")).encode("ascii")
    return assets


def validate_tls_files(certificate: Path, private_key: Path) -> None:
    if certificate.is_symlink() or private_key.is_symlink():
        raise FixtureError("TLS files must not be symbolic links")
    if not certificate.is_file() or not private_key.is_file():
        raise FixtureError("TLS files must be regular files")
    if private_key.stat().st_mode & 0o077:
        raise FixtureError("TLS private key must not be group- or world-accessible")


def handler_for(assets: dict[str, bytes], tag: str) -> type[BaseHTTPRequestHandler]:
    routes = {
        "/releases/latest": (assets["release.json"], "application/json"),
        f"/releases/download/{tag}/{MANIFEST_NAME}": (assets[MANIFEST_NAME], "application/json"),
        f"/releases/download/{tag}/{IMAGE_NAME}": (assets[IMAGE_NAME], "application/octet-stream"),
    }

    class FixtureHandler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            selected = routes.get(self.path)
            if selected is None:
                self.send_error(404)
                return
            body, media_type = selected
            self.send_response(200)
            self.send_header("Content-Type", media_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Deliberate interruption is one physical matrix case.

        def log_message(self, format: str, *args: object) -> None:
            # Do not log client addresses or paths into a potentially shared console.
            return

    return FixtureHandler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True)
    parser.add_argument("--certificate", type=Path, required=True)
    parser.add_argument("--private-key", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--release-id", type=int, required=True)
    args = parser.parse_args()
    try:
        host, port = parse_origin(args.origin)
        assets = load_candidate(args.assets, args.tag, args.release_id, args.origin)
        validate_tls_files(args.certificate, args.private_key)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(args.certificate, args.private_key)
        server = ThreadingHTTPServer((host, port), handler_for(assets, args.tag))
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    except (FixtureError, OSError, ssl.SSLError, ValueError) as error:
        print(f"local OTA fixture: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"local OTA fixture: serving frozen {args.tag} release {args.release_id} at {args.origin}")
    for name in (MANIFEST_NAME, IMAGE_NAME):
        print(f"  {name} sha256={hashlib.sha256(assets[name]).hexdigest()}")
    print("Local-fixture evidence only; this does not prove GitHub delivery or immutability.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
