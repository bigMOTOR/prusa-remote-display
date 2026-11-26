# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Keep the public OTA measurement deterministic at its opener boundary."""

from contextlib import redirect_stderr, redirect_stdout
from email.message import Message
import io
import json
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch
import urllib.error


SCRIPT = Path(__file__).resolve().parents[2] / "tools/check-github-ota-url.py"
TAG = "v1.2.3"
ASSET_ID = 123456789
ASSET_NAME = "prusa_remote_display.bin"
RELEASE_URL = "https://api.github.com/repos/bigMOTOR/prusa-remote-display/releases/latest"
ASSET_URL = f"https://github.com/bigMOTOR/prusa-remote-display/releases/download/{TAG}/{ASSET_NAME}"
QUERY = "?sig=fixture-signed-query"
ERROR_VALUE = "fixture-private-error-value"
LOCATION = (
    "https://release-assets.githubusercontent.com/github-production-release-asset/test" + QUERY
)
FAILURE = "GitHub OTA URL check FAIL (ValueError); signed URLs and response values withheld\n"


def sized_location(ascii_size: int, tail: str = "x") -> str:
    return LOCATION.ljust(ascii_size - 1, "x") + tail


def release_fixture(tag=TAG, asset_id=ASSET_ID, url=ASSET_URL) -> bytes:
    return json.dumps({
        "tag_name": tag,
        "assets": [{"id": asset_id, "name": ASSET_NAME, "browser_download_url": url}],
    }).encode("utf-8")


class GithubOtaUrlTests(unittest.TestCase):
    def run_cli(self, *responses):
        # Execute the real CLI error handler. The fake opener stops network access
        # and lets us simulate HTTPError the same way urllib surfaces a declined 302.
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch("urllib.request.build_opener") as build_opener,
            redirect_stdout(stdout),
            redirect_stderr(stderr),
        ):
            opener = build_opener.return_value
            opener.open.side_effect = responses
            try:
                runpy.run_path(str(SCRIPT), run_name="__main__")
            except SystemExit as result:
                status = result.code
            else:
                status = 0
        build_opener.assert_called_once()
        requested_urls = [call.args[0].full_url for call in opener.open.call_args_list]
        self.assertEqual(requested_urls, [RELEASE_URL, ASSET_URL][:len(responses)])

        output = stdout.getvalue() + stderr.getvalue()
        self.assertNotIn(QUERY, output)
        self.assertNotIn(ERROR_VALUE, output)
        return status, stdout.getvalue(), stderr.getvalue()

    def measure(self, location):
        headers = Message()
        headers["Location"] = location
        with io.BytesIO(release_fixture()) as metadata, io.BytesIO() as redirect_body:
            redirect = urllib.error.HTTPError(ASSET_URL, 302, ERROR_VALUE, headers, redirect_body)
            result = self.run_cli(metadata, redirect)
            self.assertTrue(redirect_body.closed)
        return result

    def test_1024_utf8_bytes_pass_with_sample_identity(self):
        status, stdout, stderr = self.measure(sized_location(1024))
        self.assertEqual(status, 0)
        self.assertEqual(
            stdout,
            f"real GitHub OTA release tag={TAG}; asset ID={ASSET_ID}\n"
            "real GitHub download URL bytes=1024; shared firmware limit=2048; "
            "redirect response header bytes=1038\n"
            "real GitHub download URL 2x headroom PASS\n",
        )
        self.assertEqual(stderr, "")

    def test_1025_utf8_bytes_fail_closed(self):
        status, stdout, stderr = self.measure(sized_location(1025))
        self.assertEqual(status, 1)
        self.assertIn("download URL bytes=1025; shared firmware limit=2048;", stdout)
        self.assertNotIn("PASS", stdout)
        self.assertEqual(stderr, FAILURE)

    def test_multibyte_location_is_measured_in_bytes(self):
        for characters, expected_size, expected_status in ((1023, 1024, 0), (1024, 1025, 1)):
            with self.subTest(characters=characters):
                location = sized_location(characters, "é")
                self.assertEqual(len(location), characters)
                status, stdout, stderr = self.measure(location)
                self.assertEqual(status, expected_status)
                self.assertIn(f"download URL bytes={expected_size};", stdout)
                self.assertEqual(stderr, FAILURE if expected_status else "")

    def test_http_failure_withholds_error_and_signed_location_values(self):
        headers = Message()
        headers["Location"] = LOCATION
        with io.BytesIO(release_fixture()) as metadata, io.BytesIO() as error_body:
            response = urllib.error.HTTPError(ASSET_URL, 403, ERROR_VALUE, headers, error_body)
            self.assertEqual(self.run_cli(metadata, response), (1, "", FAILURE))
            self.assertTrue(error_body.closed)

    def test_opener_error_withholds_raw_error_value(self):
        with io.BytesIO(release_fixture()) as metadata:
            status, stdout, stderr = self.run_cli(metadata, urllib.error.URLError(ERROR_VALUE))
        self.assertEqual(status, 1)
        self.assertEqual(stdout, "")
        self.assertEqual(stderr, FAILURE.replace("ValueError", "URLError"))

    def test_invalid_sample_identity_is_rejected_before_asset_request(self):
        for fixture in (
            release_fixture(tag=ERROR_VALUE),
            release_fixture(asset_id=ERROR_VALUE),
            release_fixture(asset_id=True),
            release_fixture(url=ASSET_URL.replace(TAG, "v2.0.0")),
        ):
            with self.subTest(fixture=fixture), io.BytesIO(fixture) as metadata:
                self.assertEqual(self.run_cli(metadata), (1, "", FAILURE))
