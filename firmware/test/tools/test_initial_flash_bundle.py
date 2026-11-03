# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Portable build-region contract; no serial port or device is opened."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "prd_initial_flash_packager", Path(__file__).resolve().parents[2] / "tools/package-firmware.py")
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)


class InitialFlashBundleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prd-initial-flash-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.regions = {
            "bootloader": ("0x0", "bootloader/bootloader.bin"),
            "partition-table": ("0x8000", "partition_table/partition-table.bin"),
            "otadata": ("0x10000", "ota_data_initial.bin"),
            "app": ("0x20000", "prusa_remote_display.bin"),
        }
        self.args = {name: {"offset": offset, "file": file, "encrypted": "false"}
                     for name, (offset, file) in self.regions.items()}
        self.args["flash_files"] = {offset: file for offset, file in self.regions.values()}
        self.args["flash_settings"] = {"flash_mode": "dio", "flash_size": "keep", "flash_freq": "80m"}
        self.args["extra_esptool_args"] = {"chip": "esp32s3", "before": "default_reset"}
        for _, file in self.regions.values():
            path = self.root / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        self.save()

    def save(self):
        (self.root / "flasher_args.json").write_text(json.dumps(self.args))

    def test_paths_are_portable_and_exact_build_settings_are_preserved(self):
        original = (self.root / "flasher_args.json").read_bytes()
        result = packager.portable_flash_args(self.root)
        self.assertEqual(result["flash_settings"], self.args["flash_settings"])
        self.assertEqual(result["extra_esptool_args"], self.args["extra_esptool_args"])
        for name, (offset, file) in self.regions.items():
            self.assertEqual(result[name]["file"], Path(file).name)
            self.assertEqual(result["flash_files"][offset], Path(file).name)
        self.assertEqual((self.root / "flasher_args.json").read_bytes(), original)

    def test_missing_initial_ota_selection_is_not_silently_omitted(self):
        (self.root / "ota_data_initial.bin").unlink()
        with self.assertRaisesRegex(ValueError, "missing initial flash input: otadata"):
            packager.portable_flash_args(self.root)

    def test_every_region_is_required_and_duplicated_paths_must_agree(self):
        for name, (offset, _) in self.regions.items():
            with self.subTest(region=name):
                original = self.args["flash_files"][offset]
                self.args["flash_files"][offset] = "different.bin"
                self.save()
                with self.assertRaisesRegex(ValueError, "inconsistent initial flash region"):
                    packager.portable_flash_args(self.root)
                self.args["flash_files"][offset] = original
        self.args.pop("otadata")
        self.save()
        with self.assertRaises(ValueError):
            packager.portable_flash_args(self.root)

    def test_wrong_path_and_overlapping_ranges_are_rejected(self):
        self.args["app"]["file"] = "../prusa_remote_display.bin"
        self.args["flash_files"]["0x20000"] = "../prusa_remote_display.bin"
        self.save()
        with self.assertRaisesRegex(ValueError, "invalid initial flash path"):
            packager.portable_flash_args(self.root)
        self.args["app"]["file"] = "prusa_remote_display.bin"
        self.args["app"]["offset"] = "0x8001"
        self.args["flash_files"].pop("0x20000")
        self.args["flash_files"]["0x8001"] = "prusa_remote_display.bin"
        self.save()
        with self.assertRaisesRegex(ValueError, "overlapping initial flash regions"):
            packager.portable_flash_args(self.root)


if __name__ == "__main__":
    unittest.main()
