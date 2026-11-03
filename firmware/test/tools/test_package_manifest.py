# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Packaging contract only; not an OTA downloader, trust policy or device validator."""

import hashlib
import importlib.util
import json
import tempfile
import unittest
import struct
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prd_manifest_packager", FIRMWARE / "tools/package-firmware.py"
)
assert SPEC is not None and SPEC.loader is not None
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)


class ManifestTests(unittest.TestCase):
    def test_cpp_decoder_fixture_matches_real_producer(self):
        header = (FIRMWARE / 'test/host/ota/producer_manifest.hpp').read_text()
        fixture = header.split('R"json(', 1)[1].split(')json"', 1)[0]
        self.assertEqual(json.loads(fixture), self.manifest())

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prd-manifest-test-")
        self.addCleanup(temporary.cleanup)
        self.image = Path(temporary.name) / "application.bin"
        self.metadata = {
            "productVersion": "0.1.0", "boardTarget": "test-board",
            "target": "esp32s3", "buildProfile": "debug",
            "gitCommit": "a" * 40, "gitDescribe": "test-build",
            "idfVersion": "v5.5.4", "buildEpoch": "0",
            "releaseTag": "", "dirtyTree": "true",
        }
        self.partitions = [
            {"subtype": "ota_0", "size": 3 * 1024 * 1024},
            {"subtype": "ota_1", "size": 2 * 1024 * 1024},
            {"subtype": "ota", "size": 8192},
        ]
        data = bytearray(bytes(range(256)) * 5000)
        data[0:2] = b'\xe9\x01'
        struct.pack_into('<I', data, 28, 344)
        struct.pack_into('<I', data, 32, 0xabcd5432)
        data[48:80] = b'0.1.0'.ljust(32, b'\0')
        data[80:112] = b'prusa_remote_display'.ljust(32, b'\0')
        struct.pack_into('<8sII32s32s8s', data, 288, b'PRDOTA01', 1, 1,
                         b'test-board', b'0.1.0', bytes(8))
        self.image.write_bytes(data)

    def manifest(self):
        return packager.build_manifest(self.metadata, self.image, self.partitions)

    def test_size_and_digest_cover_actual_bytes_across_read_chunks(self):
        value = self.manifest()["image"]
        self.assertEqual(value["sizeBytes"], 1280000)
        self.assertEqual(value["sha256"], hashlib.sha256(self.image.read_bytes()).hexdigest())
        self.assertEqual(value["name"], self.image.name)

    def test_changed_and_truncated_images_cannot_reuse_old_digest(self):
        original = self.manifest()["image"]
        self.image.write_bytes(b"changed")
        with self.assertRaises(ValueError):
            self.manifest()
        self.image.write_bytes(b"")
        with self.assertRaises(ValueError):
            self.manifest()

    def test_smallest_application_slot_excludes_otadata(self):
        value = self.manifest()["compatibility"]
        self.assertEqual(value["otaSlotSizeBytes"], 2 * 1024 * 1024)
        self.assertEqual(value["otaSlotCount"], 2)
        self.assertEqual(value["minBootloaderSchema"], 1)

    def test_identity_corruption_and_noncanonical_fields_are_rejected(self):
        original = self.image.read_bytes()
        for offset in (288, 296, 304, 336, 368, 80, 48):
            with self.subTest(offset=offset):
                changed = bytearray(original)
                changed[offset] ^= 1
                self.image.write_bytes(changed)
                with self.assertRaises(ValueError):
                    self.manifest()
        self.image.write_bytes(original)

    def test_schema_is_taken_from_image_not_invented_by_packager(self):
        data = bytearray(self.image.read_bytes())
        struct.pack_into('<I', data, 300, 2)
        self.image.write_bytes(data)
        self.assertEqual(self.manifest()['compatibility']['minBootloaderSchema'], 2)

    def test_descriptor_must_be_inside_first_segment(self):
        data = bytearray(self.image.read_bytes())
        struct.pack_into('<I', data, 28, 256)
        self.image.write_bytes(data)
        with self.assertRaises(ValueError):
            self.manifest()

    def test_missing_slots_are_not_invented(self):
        self.partitions = []
        value = self.manifest()["compatibility"]
        self.assertEqual(value["otaSlotSizeBytes"], 0)
        self.assertEqual(value["otaSlotCount"], 0)

    def test_build_identity_is_copied_not_replaced_with_host_values(self):
        value = self.manifest()
        for name in ("productVersion", "boardTarget", "buildProfile", "gitCommit",
                     "gitDescribe", "idfVersion", "buildEpoch", "releaseTag"):
            self.assertEqual(value[name], self.metadata[name])
        self.assertEqual(value["chipTarget"], self.metadata["target"])
        self.assertEqual(value["schemaVersion"], 1)
        self.assertIs(value["dirtyTree"], True)
        self.metadata["dirtyTree"] = "false"
        self.assertIs(self.manifest()["dirtyTree"], False)

    def test_unsigned_status_does_not_claim_a_signature(self):
        value = self.manifest()["signing"]
        self.assertIsNone(value["algorithm"])
        self.assertIsNone(value["signature"])
        self.assertIn("unsigned", value["note"])

    def test_deterministic_and_non_mutating(self):
        before = json.dumps([self.metadata, self.partitions], sort_keys=True)
        first = json.dumps(self.manifest(), sort_keys=True)
        self.assertEqual(first, json.dumps(self.manifest(), sort_keys=True))
        self.assertEqual(before, json.dumps([self.metadata, self.partitions], sort_keys=True))

    def test_missing_image_fails_instead_of_reusing_a_manifest(self):
        self.image.unlink()
        with self.assertRaises(FileNotFoundError):
            self.manifest()

    def test_repository_partition_table_declares_two_application_slots(self):
        partitions = packager.partition_layout(FIRMWARE)
        slots = [p for p in partitions if p["type"] == "app"
                 and str(p["subtype"]).startswith("ota_")]
        self.assertEqual({p["subtype"] for p in slots}, {"ota_0", "ota_1"})
        self.assertTrue(all(p["size"] > 0 for p in slots))


if __name__ == "__main__":
    unittest.main()
