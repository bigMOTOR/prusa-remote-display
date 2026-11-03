# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Run in the ESP-IDF Python environment. Ephemeral test keys only, never owner keys."""

import contextlib
import hashlib
import importlib.util
import io
import json
import struct
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import espsecure
import esptool
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

FIRMWARE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('signed_packager', FIRMWARE / 'tools/package-firmware.py')
packager = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(packager)
RELEASE_SPEC = importlib.util.spec_from_file_location(
    'release_assets', FIRMWARE / 'tools/check-release-assets.py')
release_assets = importlib.util.module_from_spec(RELEASE_SPEC)
RELEASE_SPEC.loader.exec_module(release_assets)


class SignedPackageTests(unittest.TestCase):
    FULL_COMMIT = 'a' * 40

    @classmethod
    def setUpClass(cls):
        cls.key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.other_key = rsa.generate_private_key(public_exponent=65537, key_size=3072)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='prd-signed-package-test-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.app = self.root / 'prusa_remote_display.bin'
        # Structurally valid packaging identity, not an executable ESP application.
        # Keep the original 5123-byte extent: signing must still pad to 8192 and
        # add a 4096-byte signature sector, exercising signed-vs-unsigned size.
        image = bytearray(bytes(range(256)) * 20 + b'end')
        image[0:2] = b'\xe9\x01'
        struct.pack_into('<I', image, 28, 344)
        struct.pack_into('<I', image, 32, 0xabcd5432)
        image[48:80] = b'0.1.0'.ljust(32, b'\0')
        image[80:112] = b'prusa_remote_display'.ljust(32, b'\0')
        struct.pack_into('<8sII32s32s8s', image, 288, b'PRDOTA01', 1, 1,
                         b'fixture-board', b'0.1.0', bytes(8))
        self.app.write_bytes(image)
        self.signed = self.root / 'external-signed.bin'
        self.public = self.root / 'public.pem'
        self.public.write_bytes(self.public_bytes(self.key))
        self.slots = [{'name': 'ota_0', 'type': 'app', 'subtype': 'ota_0', 'size': 16384, 'offset': 0},
                      {'name': 'ota_1', 'type': 'app', 'subtype': 'ota_1', 'size': 16384, 'offset': 16384}]
        self.sign()

    @staticmethod
    def public_bytes(key):
        return key.public_key().public_bytes(serialization.Encoding.PEM,
                                             serialization.PublicFormat.SubjectPublicKeyInfo)

    def sign(self):
        private = self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                         serialization.NoEncryption())
        with self.app.open('rb') as source, contextlib.redirect_stdout(io.StringIO()):
            espsecure.sign_secure_boot_v2(SimpleNamespace(datafile=source, keyfile=[io.BytesIO(private)],
                output=str(self.signed), signature=None, pub_key=None, append_signatures=False, hsm=False))

    def verify(self):
        return packager.verify_signed_app(self.app, self.signed, self.public, self.slots)

    def test_valid_signature_returns_exact_signed_bytes(self):
        data, metadata = self.verify()
        self.assertEqual(data, self.signed.read_bytes())
        self.assertEqual(len(data), 12288)
        self.assertEqual(metadata['algorithm'], 'RSA-PSS-SHA256')
        der = self.key.public_key().public_bytes(serialization.Encoding.DER,
                                                 serialization.PublicFormat.SubjectPublicKeyInfo)
        self.assertEqual(metadata['publicKeySpkiSha256'], hashlib.sha256(der).hexdigest())

    def test_wrong_publisher_key_rejected(self):
        self.public.write_bytes(self.public_bytes(self.other_key))
        with self.assertRaises(esptool.FatalError):
            self.verify()

    def test_other_build_and_truncation_rejected(self):
        original = self.app.read_bytes()
        self.app.write_bytes(original[:-1] + b'X')
        with self.assertRaises(ValueError):
            self.verify()
        self.app.write_bytes(original)
        self.signed.write_bytes(self.signed.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            self.verify()

    def test_signature_tampering_rejected(self):
        data = bytearray(self.signed.read_bytes())
        data[8192 + 100] ^= 1
        self.signed.write_bytes(data)
        with self.assertRaises(esptool.FatalError):
            self.verify()

    def test_signature_padding_tampering_rejected(self):
        data = bytearray(self.signed.read_bytes())
        data[-1] = 0
        self.signed.write_bytes(data)
        with self.assertRaises(ValueError):
            self.verify()

    def test_signed_size_not_unsigned_size_must_fit(self):
        self.slots[0]['size'] = self.app.stat().st_size
        with self.assertRaises(ValueError):
            self.verify()
        self.slots[0]['size'] = 12288
        self.verify()
        self.slots.pop()
        with self.assertRaises(ValueError):
            self.verify()

    def test_private_key_not_accepted_as_public_input(self):
        self.public.write_bytes(self.key.private_bytes(serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
        with self.assertRaises(ValueError):
            self.verify()

    def prepare_cli(self):
        (self.root / 'bootloader').mkdir()
        (self.root / 'partition_table').mkdir()
        (self.root / 'bootloader/bootloader.bin').write_bytes(b'boot-fixture')
        (self.root / 'partition_table/partition-table.bin').write_bytes(b'partition-fixture')
        (self.root / 'ota_data_initial.bin').write_bytes(b'ota-fixture')
        regions = {
            'bootloader': ('0x0', 'bootloader/bootloader.bin'),
            'partition-table': ('0x8000', 'partition_table/partition-table.bin'),
            'otadata': ('0x10000', 'ota_data_initial.bin'),
            'app': ('0x20000', 'prusa_remote_display.bin'),
        }
        flash = {name: {'offset': offset, 'file': file}
                 for name, (offset, file) in regions.items()}
        flash['flash_files'] = {offset: file for offset, file in regions.values()}
        (self.root / 'flasher_args.json').write_text(json.dumps(flash))
        metadata = {'productVersion': '0.1.0', 'boardTarget': 'fixture-board', 'target': 'esp32s3',
                    'buildProfile': 'debug', 'gitCommit': self.FULL_COMMIT[:12], 'gitDescribe': 'test',
                    'idfVersion': 'v5.5.4', 'buildEpoch': '0', 'releaseTag': '', 'dirtyTree': 'true'}
        args = ['package-firmware.py', '--profile', 'debug', '--build-dir', str(self.root),
                '--signed-app', str(self.signed), '--verification-key', str(self.public), '--no-archive']
        return args, metadata

    def test_cli_packages_verified_snapshot_and_reports_signed_hash(self):
        args, metadata = self.prepare_cli()
        expected = self.signed.read_bytes()
        original_verify = packager.verify_signed_app

        def replace_after_verification(*args):
            result = original_verify(*args)
            self.signed.write_bytes(b'replaced-after-verification')
            return result

        with patch('sys.argv', args), patch.object(packager, 'load_build_metadata', return_value=metadata), \
             patch.object(packager, 'partition_layout', return_value=self.slots), \
             patch.object(packager, 'verify_signed_app', side_effect=replace_after_verification), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(packager.main(), 0)
        output = self.root / 'artifacts-signed'
        self.assertEqual((output / self.app.name).read_bytes(), expected)
        self.assertEqual((output / 'ota_data_initial.bin').read_bytes(), b'ota-fixture')
        flash = json.loads((output / 'flash_args.json').read_text())
        for region in ('bootloader', 'partition-table', 'otadata', 'app'):
            self.assertTrue((output / flash[region]['file']).is_file())
        manifest = json.loads((output / 'manifest.json').read_text())
        self.assertEqual(manifest['image']['sizeBytes'], len(expected))
        self.assertEqual(manifest['image']['sha256'], hashlib.sha256(expected).hexdigest())
        self.assertEqual(manifest['image']['name'], self.app.name)
        self.assertEqual(manifest['boardTarget'], 'fixture-board')
        self.assertEqual(manifest['productVersion'], '0.1.0')
        self.assertEqual(manifest['compatibility']['minBootloaderSchema'], 1)
        self.assertEqual(manifest['signing']['signature'], 'embedded-esp-secure-boot-v2')
        self.assertEqual(manifest['signing']['note'],
                         'application image signature verified; bootloader, partition table and JSON manifest are unsigned')
        self.assertFalse((self.root / 'artifacts').exists())

    def test_signed_packager_output_passes_the_offline_release_gate(self):
        args, metadata = self.prepare_cli()
        board = release_assets.BOARD_TARGET.encode()
        image = bytearray(self.app.read_bytes())
        image[304:336] = board.ljust(32, b'\0')
        self.app.write_bytes(image)
        self.sign()
        metadata.update({
            'boardTarget': release_assets.BOARD_TARGET,
            'buildProfile': 'release',
            'releaseTag': 'v0.1.0',
            'dirtyTree': 'false',
        })
        args[args.index('--profile') + 1] = 'release'
        with patch('sys.argv', args), \
             patch.object(packager, 'load_build_metadata', return_value=metadata), \
             patch.object(packager, 'partition_layout', return_value=self.slots), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(packager.main(), 0)
        summary = release_assets.verify(
            self.root / 'artifacts-signed', metadata['releaseTag'], self.FULL_COMMIT)
        self.assertEqual(summary['imageSha256'], hashlib.sha256(self.signed.read_bytes()).hexdigest())

    def test_signed_validation_bundle_remains_on_private_channel(self):
        args, metadata = self.prepare_cli()
        origin = 'https://192.168.1.40:8443'
        url = f'{origin}/releases/latest'.encode()
        image = bytearray(self.app.read_bytes())
        image[304:336] = release_assets.BOARD_TARGET.encode().ljust(32, b'\0')
        image[500:500 + len(url)] = url
        self.app.write_bytes(image)
        self.sign()
        metadata.update({
            'boardTarget': release_assets.BOARD_TARGET,
            'buildProfile': 'hardware-validation',
            'releaseTag': 'v0.1.0',
            'dirtyTree': 'false',
        })
        args[args.index('--profile') + 1] = 'hardware-validation'
        with patch('sys.argv', args), \
             patch.object(packager, 'load_build_metadata', return_value=metadata), \
             patch.object(packager, 'partition_layout', return_value=self.slots), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(packager.main(), 0)
        output = self.root / 'artifacts-signed'
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, 'release profile'):
            release_assets.verify(output, metadata['releaseTag'], self.FULL_COMMIT)
        summary = release_assets.verify(
            output, metadata['releaseTag'], self.FULL_COMMIT, origin)
        self.assertEqual(summary['imageSha256'], hashlib.sha256(self.signed.read_bytes()).hexdigest())

    def test_cli_rejects_signed_legacy_image_without_creating_output(self):
        args, metadata = self.prepare_cli()
        image = bytearray(self.app.read_bytes())
        image[288:376] = bytes(88)
        self.app.write_bytes(image)
        self.sign()
        # The rejection is identity policy, not a fake or damaged signature.
        self.assertEqual(self.verify()[0], self.signed.read_bytes())
        with patch('sys.argv', args), patch.object(packager, 'load_build_metadata', return_value=metadata), \
             patch.object(packager, 'partition_layout', return_value=self.slots):
            with self.assertRaisesRegex(ValueError, 'unsupported OTA image identity'):
                packager.main()
        self.assertFalse((self.root / 'artifacts-signed').exists())
        self.assertFalse((self.root / 'artifacts').exists())

    def test_cli_rejects_metadata_mismatch_before_creating_output(self):
        args, metadata = self.prepare_cli()
        self.assertEqual(self.verify()[0], self.signed.read_bytes())
        for field, mismatch in (('productVersion', '9.0.0'), ('boardTarget', 'other-board')):
            with self.subTest(field=field):
                changed = dict(metadata, **{field: mismatch})
                with patch('sys.argv', args), \
                     patch.object(packager, 'load_build_metadata', return_value=changed), \
                     patch.object(packager, 'partition_layout', return_value=self.slots):
                    with self.assertRaisesRegex(ValueError, 'image identity differs from build metadata'):
                        packager.main()
                self.assertFalse((self.root / 'artifacts-signed').exists())
                self.assertFalse((self.root / 'artifacts').exists())

    def test_cli_rejects_signed_bad_header_or_segment_bounds_without_output(self):
        args, metadata = self.prepare_cli()
        baseline = self.app.read_bytes()
        for case in ('magic', 'short-segment', 'oversize-segment'):
            with self.subTest(case=case):
                image = bytearray(baseline)
                if case == 'magic':
                    image[0] = 0
                    expected_error = 'missing OTA image identity'
                else:
                    length = 343 if case == 'short-segment' else len(image) - 31
                    struct.pack_into('<I', image, 28, length)
                    expected_error = 'OTA identity outside first image segment'
                self.app.write_bytes(image)
                self.sign()
                self.assertEqual(self.verify()[0], self.signed.read_bytes())
                with patch('sys.argv', args), \
                     patch.object(packager, 'load_build_metadata', return_value=metadata), \
                     patch.object(packager, 'partition_layout', return_value=self.slots):
                    with self.assertRaisesRegex(ValueError, expected_error):
                        packager.main()
                self.assertFalse((self.root / 'artifacts-signed').exists())
                self.assertFalse((self.root / 'artifacts').exists())


if __name__ == '__main__':
    unittest.main()
