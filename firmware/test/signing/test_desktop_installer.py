# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Desktop initial-image verification with real cryptography, disposable keys, no USB."""
import hashlib
import importlib.util
import sys
import unittest
from pathlib import Path
import test_browser_installer as fixtures

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("desktop_core", ROOT / "desktop-installer/core.py")
core = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = core
SPEC.loader.exec_module(core)


class DesktopSignatureTests(unittest.TestCase):
    FULL_COMMIT = fixtures.BrowserInstallerTests.FULL_COMMIT
    setUpClass = classmethod(fixtures.BrowserInstallerTests.setUpClass.__func__)
    setUp = fixtures.BrowserInstallerTests.setUp
    public_bytes = staticmethod(fixtures.BrowserInstallerTests.public_bytes)
    sign = fixtures.BrowserInstallerTests.sign
    prepare_cli = fixtures.BrowserInstallerTests.prepare_cli
    bundle = fixtures.BrowserInstallerTests.bundle

    def prepare(self, bundle, key=None):
        assets = tuple((n, f"https://github.com/{core.REPOSITORY}/releases/download/v0.1.0/{n}")
                       for n in core.LIMITS)
        release = core.Release("v0.1.0", self.FULL_COMMIT, assets)
        def fetch(url, limit, **kwargs):
            data = (bundle / url.rsplit("/", 1)[-1]).read_bytes()
            return data
        return core.prepare(release, key or self.public.read_bytes(), ROOT, fetch)

    def test_actual_merge_and_signature_preserve_exact_application(self):
        bundle = self.bundle()
        result = self.prepare(bundle)
        self.assertEqual(result.data[0x20000:], self.signed.read_bytes())
        self.assertEqual(result.data[2:4], b"\x02\x4f")
        self.assertEqual(result.data[0x10000:0x1000b], b"ota-fixture")
        self.assertEqual(result.sha256, hashlib.sha256(result.data).hexdigest())

    def test_wrong_key_and_tampered_boot_are_rejected(self):
        bundle = self.bundle()
        with self.assertRaisesRegex(core.InstallError, "publisher"):
            self.prepare(bundle, self.public_bytes(self.other_key))
        (bundle / "bootloader.bin").write_bytes(b"tampered")
        with self.assertRaisesRegex(core.InstallError, "checksum"):
            self.prepare(bundle)

    def test_signature_tampering_even_after_rehash_is_rejected(self):
        bundle = self.bundle()
        app = bundle / "prusa_remote_display.bin"
        data = bytearray(app.read_bytes())
        data[8192 + 100] ^= 1
        # Direct signature verifier must fail even when attacker supplies their own hashes.
        import esptool
        from cryptography.hazmat.primitives import serialization
        der = self.key.public_key().public_bytes(serialization.Encoding.DER,
                                                 serialization.PublicFormat.SubjectPublicKeyInfo)
        with self.assertRaises(esptool.FatalError):
            core.verify_signature(bytes(data), self.public.read_bytes(), hashlib.sha256(der).hexdigest())


if __name__ == "__main__":
    unittest.main()
