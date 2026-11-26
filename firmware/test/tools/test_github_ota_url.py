# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real OTA CLI and redirect policy with offline HTTP fixtures."""

from contextlib import redirect_stderr, redirect_stdout
from email.message import Message
import io
import json
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch
import urllib.error
import urllib.response


SCRIPT = Path(__file__).resolve().parents[2] / "tools/check-github-ota-url.py"
TAG = "v1.2.3"
ASSET_ID = 123456789
ASSET_NAME = "prusa_remote_display.bin"
RELEASE_URL = "https://api.github.com/repos/bigMOTOR/prusa-remote-display/releases/latest"
ASSET_URL = f"https://github.com/bigMOTOR/prusa-remote-display/releases/download/{TAG}/{ASSET_NAME}"
QUERY = "?sig=fixture-signed-query"
ERROR_VALUE = "fixture-private-error-value"
HEADER_VALUE = "fixture-private-header-value"
TOKEN = "fixture-ota-metadata-token"
UNRELATED_TOKEN = "fixture-unrelated-token"
TOKEN_ENV = "OTA_RELEASE_METADATA_TOKEN"
OTHER_URL = "https://outside.invalid/fixture" + QUERY
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


def http_fixture(code=200, body=b"", location=None):
    headers = Message()
    headers["X-Fixture-Response"] = HEADER_VALUE
    if location is not None:
        headers["Location"] = location
    response = urllib.response.addinfourl(io.BytesIO(body), headers, ERROR_VALUE, code)
    response.msg = ERROR_VALUE
    return response


class GithubOtaUrlTests(unittest.TestCase):
    def run_cli(self, *responses, token=None):
        # Stub the transport so urllib's real opener processes each response.
        env = {"GITHUB_TOKEN": UNRELATED_TOKEN, "GH_TOKEN": UNRELATED_TOKEN}
        if token is not None:
            env[TOKEN_ENV] = token
        stdout, stderr = io.StringIO(), io.StringIO()
        with (
            patch.dict("os.environ", env, clear=True),
            patch("urllib.request.AbstractHTTPHandler.do_open", side_effect=responses) as transport,
            redirect_stdout(stdout),
            redirect_stderr(stderr),
        ):
            try:
                runpy.run_path(str(SCRIPT), run_name="__main__")
            except SystemExit as result:
                status = result.code
            else:
                status = 0

        requests = [call.args[1] for call in transport.call_args_list]
        self.assertEqual([request.full_url for request in requests],
                         [RELEASE_URL, ASSET_URL][:len(responses)])
        for request in requests:
            expected = f"Bearer {token}" if token and request.full_url == RELEASE_URL else None
            self.assertEqual(request.get_header("Authorization"), expected)
            self.assertNotIn("Authorization", request.headers)

        output = stdout.getvalue() + stderr.getvalue()
        for value in (RELEASE_URL, ASSET_URL, OTHER_URL, LOCATION, QUERY,
                      ERROR_VALUE, HEADER_VALUE, TOKEN, UNRELATED_TOKEN):
            self.assertNotIn(value, output)
        return status, stdout.getvalue(), stderr.getvalue()

    def measure(self, location, token=None):
        with (http_fixture(body=release_fixture()) as metadata,
              http_fixture(302, location=location) as redirect):
            result = self.run_cli(metadata, redirect, token=token)
            self.assertTrue(metadata.closed)
            self.assertTrue(redirect.closed)
        return result

    def test_1024_utf8_bytes_pass_with_sample_identity(self):
        status, stdout, stderr = self.measure(sized_location(1024), token=TOKEN)
        self.assertEqual(status, 0)
        self.assertEqual(
            stdout,
            f"real GitHub OTA release tag={TAG}; asset ID={ASSET_ID}\n"
            "real GitHub download URL bytes=1024; shared firmware limit=2048; "
            "redirect response header bytes=1088\n"
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
        with (http_fixture(body=release_fixture()) as metadata,
              http_fixture(403, body=ERROR_VALUE.encode(), location=LOCATION) as response):
            self.assertEqual(self.run_cli(metadata, response, token=TOKEN), (1, "", FAILURE))
            self.assertTrue(response.closed)

    def test_asset_redirect_requires_expected_scheme_host_path_and_query(self):
        for location in (
            LOCATION.replace("https://", "http://"),
            OTHER_URL,
            LOCATION.replace("github-production-release-asset/", "unexpected/"),
            LOCATION.removesuffix(QUERY),
        ):
            with self.subTest(location=location):
                self.assertEqual(self.measure(location, token=TOKEN), (1, "", FAILURE))

    def test_opener_error_withholds_raw_error_value(self):
        with http_fixture(body=release_fixture()) as metadata:
            status, stdout, stderr = self.run_cli(metadata, urllib.error.URLError(ERROR_VALUE),
                                                  token=TOKEN)
        self.assertEqual(status, 1)
        self.assertEqual(stdout, "")
        self.assertEqual(stderr, FAILURE.replace("ValueError", "URLError"))

    def test_invalid_sample_identity_is_rejected_before_asset_request(self):
        for fixture in (
            release_fixture(tag=ERROR_VALUE),
            release_fixture(asset_id=ERROR_VALUE),
            release_fixture(asset_id=True),
            release_fixture(url=ASSET_URL.replace(TAG, "v2.0.0")),
            release_fixture(url=OTHER_URL),
        ):
            with self.subTest(fixture=fixture), http_fixture(body=fixture) as metadata:
                self.assertEqual(self.run_cli(metadata, token=TOKEN), (1, "", FAILURE))

    def test_metadata_http_failures_have_only_numeric_status_and_no_fallback(self):
        for code in (401, 403, 404, 429, 500):
            with self.subTest(code=code), http_fixture(
                code, body=ERROR_VALUE.encode(), location=OTHER_URL,
            ) as response:
                failure = FAILURE.replace("ValueError", f"release metadata HTTP {code}")
                self.assertEqual(self.run_cli(response, token=TOKEN), (1, "", failure))
                self.assertTrue(response.closed)

    def test_metadata_redirects_are_never_followed(self):
        for code in (301, 302, 303, 307, 308):
            for location in (f"{RELEASE_URL}{QUERY}", OTHER_URL):
                with self.subTest(code=code, location=location), http_fixture(
                    code, location=location,
                ) as response:
                    failure = FAILURE.replace("ValueError", f"release metadata HTTP {code}")
                    self.assertEqual(self.run_cli(response, token=TOKEN), (1, "", failure))
                    self.assertTrue(response.closed)

    def test_metadata_transport_failure_does_not_retry_anonymously(self):
        response = urllib.error.URLError(f"{ERROR_VALUE}: {TOKEN} {OTHER_URL}")
        failure = FAILURE.replace("ValueError", "URLError")
        self.assertEqual(self.run_cli(response, token=TOKEN), (1, "", failure))

    def test_empty_metadata_token_fails_before_network(self):
        self.assertEqual(self.run_cli(token=""), (1, "", FAILURE))
