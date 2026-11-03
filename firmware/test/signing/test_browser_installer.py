# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Real esptool merge and signature checks, disposable keys and non-executable fixtures."""
import contextlib
import hashlib
import importlib.util
import io
import json
import unittest
from pathlib import Path
from unittest.mock import patch

from esptool.bin_image import ESP32S3FirmwareImage, ImageSegment
import test_signed_package as fixtures

packager = fixtures.packager
release_assets = fixtures.release_assets

spec = importlib.util.spec_from_file_location(
    "browser_installer", Path(__file__).resolve().parents[2] / "tools/prepare-installer.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class BrowserInstallerTests(unittest.TestCase):
    # Reuse disposable signing fixtures, not the inherited test methods.
    FULL_COMMIT = fixtures.SignedPackageTests.FULL_COMMIT
    setUpClass = classmethod(fixtures.SignedPackageTests.setUpClass.__func__)
    setUp = fixtures.SignedPackageTests.setUp
    public_bytes = staticmethod(fixtures.SignedPackageTests.public_bytes)
    sign = fixtures.SignedPackageTests.sign
    prepare_cli = fixtures.SignedPackageTests.prepare_cli
    def bundle(self):
        args, metadata = self.prepare_cli()
        boot = ESP32S3FirmwareImage()
        boot.segments = [ImageSegment(0x3fce0000, b"\0" * 32)]
        boot.segments[0].name = ".data"
        boot.save(str(self.root / "bootloader/bootloader.bin"))
        flash_path = self.root / "flasher_args.json"
        flash = json.loads(flash_path.read_text())
        for region in ("bootloader", "partition-table", "otadata", "app"):
            flash[region]["encrypted"] = "false"  # Real ESP-IDF flasher record.
        flash.update(flash_settings={"flash_mode": "qio", "flash_freq": "80m", "flash_size": "keep"},
                     extra_esptool_args={"chip": "esp32s3"})
        flash_path.write_text(json.dumps(flash))
        image = bytearray(self.app.read_bytes())
        image[304:336] = release_assets.BOARD_TARGET.encode().ljust(32, b"\0")
        self.app.write_bytes(image)
        self.sign()
        metadata.update(boardTarget=release_assets.BOARD_TARGET, buildProfile="release",
                        releaseTag="v0.1.0", dirtyTree="false")
        args[args.index("--profile") + 1] = "release"
        with patch("sys.argv", args), patch.object(packager, "load_build_metadata", return_value=metadata), \
             patch.object(packager, "partition_layout", return_value=self.slots), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(packager.main(), 0)
        return self.root / "artifacts-signed"

    def prepare(self, bundle):
        return installer.prepare(bundle, "v0.1.0", self.FULL_COMMIT, self.public, self.root / "web")

    def test_browser_merge_preserves_signed_application_and_initial_data(self):
        bundle = self.bundle()
        result = self.prepare(bundle)
        merged = (self.root / "web/merged-firmware.bin").read_bytes()
        self.assertEqual(merged[0x20000:], self.signed.read_bytes())
        self.assertEqual(merged[2], 2)  # bootloader DIO
        self.assertEqual(merged[3], 0x4f)  # 16 MiB, 80 MHz
        self.assertEqual(result["imageSha256"], hashlib.sha256(merged).hexdigest())
        self.assertEqual(merged[0x10000:0x1000b], b"ota-fixture")
        manifest = json.loads((self.root / "web/web-manifest.json").read_text())
        self.assertEqual(manifest["builds"][0]["parts"], [{"path": "merged-firmware.bin", "offset": 0}])
        with self.assertRaisesRegex(ValueError, "existing output"):
            self.prepare(bundle)

    def test_browser_rejects_wrong_key_missing_and_substituted_initial_inputs(self):
        bundle = self.bundle()
        self.public.write_bytes(self.public_bytes(self.other_key))
        with self.assertRaisesRegex(ValueError, "publisher"):
            self.prepare(bundle)
        self.public.write_bytes(self.public_bytes(self.key))
        (bundle / "bootloader.bin").write_bytes(b"substituted")
        with self.assertRaisesRegex(ValueError, "initial flash input"):
            self.prepare(bundle)
        self.assertFalse((self.root / "web").exists())

    def test_browser_rejects_nonstable_tag_before_emitting_output(self):
        with self.assertRaisesRegex(ValueError, "stable release"):
            installer.prepare(self.root, "v0.1.0-dev", self.FULL_COMMIT, self.public, self.root / "web")
        self.assertFalse((self.root / "web").exists())
