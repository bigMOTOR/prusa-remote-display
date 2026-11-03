# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Offline firing controls for the private-LAN OTA release fixture."""

import importlib.util
from http.server import ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
from threading import Thread
import unittest
from urllib.error import HTTPError, URLError
from urllib.request import urlopen


FIRMWARE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prd_ota_validation_fixture", FIRMWARE / "tools/serve-ota-validation.py"
)
assert SPEC is not None and SPEC.loader is not None
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)
ORIGIN = "https://192.168.1.40:8443"


class OtaValidationFixtureTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prd-ota-fixture-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / fixture.MANIFEST_NAME).write_text(
            json.dumps({"releaseTag": "v0.0.2"}), encoding="utf-8"
        )
        (self.root / fixture.IMAGE_NAME).write_bytes(b"signed-image-placeholder")

    def test_exact_private_https_origin_only(self):
        self.assertEqual(fixture.parse_origin(ORIGIN), ("192.168.1.40", 8443))
        for origin in (
            "http://192.168.1.40:8443",
            "https://192.168.1.40:8443/path",
            "https://user@192.168.1.40:8443",
            "https://8.8.8.8:8443",
            "https://127.0.0.1:8443",
            "https://api.github.com:443",
            "https://192.168.1.40:bad",
            "https://192.168.1.40:0",
        ):
            with self.subTest(origin=origin), self.assertRaises(fixture.FixtureError):
                fixture.parse_origin(origin)

    def test_release_routes_are_exact_and_frozen_in_memory(self):
        assets = fixture.load_candidate(self.root, "v0.0.2", 11, ORIGIN)
        release = json.loads(assets["release.json"])
        self.assertEqual(release["url"], f"{ORIGIN}/releases/11")
        self.assertEqual(release["tag_name"], "v0.0.2")
        self.assertTrue(release["immutable"])
        self.assertEqual({item["name"] for item in release["assets"]}, {
            fixture.MANIFEST_NAME, fixture.IMAGE_NAME
        })
        self.assertEqual({item["id"] for item in release["assets"]}, {111, 112})
        original = assets[fixture.IMAGE_NAME]
        (self.root / fixture.IMAGE_NAME).write_bytes(b"changed-after-start")
        self.assertEqual(assets[fixture.IMAGE_NAME], original)
        self.assertTrue(callable(fixture.handler_for(assets, "v0.0.2").do_GET))

    def test_missing_linked_or_mismatched_assets_fail_closed(self):
        with self.assertRaisesRegex(fixture.FixtureError, "stable version"):
            fixture.load_candidate(self.root, "v0.0.2-beta", 11, ORIGIN)
        with self.assertRaisesRegex(fixture.FixtureError, "releaseTag"):
            fixture.load_candidate(self.root, "v0.0.3", 11, ORIGIN)
        image = self.root / fixture.IMAGE_NAME
        image.unlink()
        image.symlink_to(self.root / fixture.MANIFEST_NAME)
        with self.assertRaisesRegex(fixture.FixtureError, "linked"):
            fixture.load_candidate(self.root, "v0.0.2", 11, ORIGIN)

    def test_tls_private_key_requires_restricted_regular_file(self):
        certificate = self.root / "certificate.pem"
        key = self.root / "key.pem"
        certificate.write_text("public test certificate", encoding="ascii")
        key.write_text("disposable test key", encoding="ascii")
        key.chmod(0o644)
        with self.assertRaisesRegex(fixture.FixtureError, "group- or world-accessible"):
            fixture.validate_tls_files(certificate, key)
        key.chmod(0o600)
        fixture.validate_tls_files(certificate, key)
        key.unlink()
        key.symlink_to(certificate)
        with self.assertRaisesRegex(fixture.FixtureError, "symbolic links"):
            fixture.validate_tls_files(certificate, key)

    @unittest.skipUnless(shutil.which("openssl"), "OpenSSL is needed for the local TLS test")
    def test_real_tls_serves_only_the_three_exact_routes(self):
        certificate = self.root / "test-cert.pem"
        key = self.root / "test-key.pem"
        generated = subprocess.run(
            [
                "openssl", "req", "-x509", "-newkey", "rsa:2048", "-noenc",
                "-keyout", str(key), "-out", str(certificate), "-days", "1",
                "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(generated.returncode, 0, generated.stderr)
        assets = fixture.load_candidate(self.root, "v0.0.2", 11, ORIGIN)
        server = ThreadingHTTPServer(("127.0.0.1", 0), fixture.handler_for(assets, "v0.0.2"))
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(certificate, key)
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        worker = Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            local = f"https://127.0.0.1:{server.server_port}"
            trust = ssl.create_default_context(cafile=str(certificate))
            for route, expected in (
                ("/releases/latest", assets["release.json"]),
                ("/releases/download/v0.0.2/manifest.json", assets[fixture.MANIFEST_NAME]),
                ("/releases/download/v0.0.2/prusa_remote_display.bin", assets[fixture.IMAGE_NAME]),
            ):
                with self.subTest(route=route), urlopen(local + route, context=trust) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(response.read(), expected)
            with self.assertRaises(HTTPError) as missing:
                urlopen(local + "/releases/download/v0.0.3/manifest.json", context=trust)
            self.assertEqual(missing.exception.code, 404)
            with self.assertRaises(URLError) as untrusted:
                urlopen(local + "/releases/latest", context=ssl.create_default_context())
            self.assertIsInstance(untrusted.exception.reason, ssl.SSLCertVerificationError)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
