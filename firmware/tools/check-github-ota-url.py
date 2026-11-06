#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Measure a real public GitHub OTA redirect without logging its signed query."""

import json
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        return None


def check():
    firmware = Path(__file__).resolve().parents[1]
    limits = (firmware / "components/network/src/public_https_limits.hpp").read_text()
    match = re.search(r"kMaximumUrl\s*=\s*(\d+)\s*;", limits)
    if match is None:
        raise ValueError("shared firmware URL limit missing")
    maximum = int(match.group(1))
    opener = urllib.request.build_opener(NoRedirect())
    headers = {"User-Agent": "prusa-remote-display-ci", "Cache-Control": "no-cache"}
    request = urllib.request.Request(
        "https://api.github.com/repos/bigMOTOR/prusa-remote-display/releases/latest",
        headers={**headers, "Accept": "application/vnd.github+json"},
    )
    with opener.open(request, timeout=20) as response:
        payload = response.read(1024 * 1024 + 1)
    if len(payload) > 1024 * 1024:
        raise ValueError("release metadata exceeds measurement bound")
    assets = [asset for asset in json.loads(payload)["assets"]
              if asset["name"] == "prusa_remote_display.bin"]
    if len(assets) != 1:
        raise ValueError("exactly one published OTA image required")
    url = assets[0]["browser_download_url"]
    prefix = "https://github.com/bigMOTOR/prusa-remote-display/releases/download/"
    if not url.startswith(prefix) or not url.endswith("/prusa_remote_display.bin"):
        raise ValueError("unexpected published image route")
    request = urllib.request.Request(url, headers={**headers, "Accept": "application/octet-stream"})
    try:
        # Do not follow the CDN URL or download firmware. The real GitHub
        # Location remains only in memory, including on assertion/error paths.
        with opener.open(request, timeout=20):
            raise ValueError("published image did not redirect")
    except urllib.error.HTTPError as response:
        try:
            if response.code != 302:
                raise ValueError("published image did not return HTTP 302") from None
            locations = response.headers.get_all("Location", [])
            if len(locations) != 1:
                raise ValueError("exactly one download Location required")
            location = locations[0]
            if not location.startswith(
                "https://release-assets.githubusercontent.com/github-production-release-asset/"
            ) or "?" not in location:
                raise ValueError("unexpected download redirect authority")
            size = len(location.encode("utf-8"))
            header_bytes = sum(len(f"{key}: {value}\r\n".encode("utf-8"))
                               for key, value in response.headers.items()) + 2
        finally:
            response.close()
    print(f"real GitHub download URL bytes={size}; shared firmware limit={maximum}; "
          f"redirect response header bytes={header_bytes}")
    if size * 2 > maximum:
        raise ValueError("real download URL lacks required 2x firmware headroom")
    print("real GitHub download URL 2x headroom PASS")


if __name__ == "__main__":
    try:
        check()
    except Exception as error:
        # Never print exception messages/tracebacks: urllib errors may contain
        # a signed Location. There is deliberately no auth or artifact output.
        print(f"GitHub OTA URL check FAIL ({type(error).__name__}); "
              "signed URLs and response values withheld", file=sys.stderr)
        sys.exit(1)
