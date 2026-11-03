# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Disposable keys only. No owner paths, secrets, hardware or firmware publication."""
import contextlib
import getpass
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import warnings

import espsecure
import esptool
from cryptography.exceptions import UnsupportedAlgorithm
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

SPEC = importlib.util.spec_from_file_location('local_signer', Path(__file__).resolve().parents[2] / 'tools/sign-firmware.py')
signer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(signer)


class SignFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        cls.password = b'disposable-test-password'
        cls.encrypted = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                         serialization.BestAvailableEncryption(cls.password))
        cls.plain = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption())
        cls.public = key.public_key().public_bytes(serialization.Encoding.PEM,
                                                 serialization.PublicFormat.SubjectPublicKeyInfo)
        cls.image = bytes(range(256)) * 20
        cls.digest = hashlib.sha256(cls.image).hexdigest()

    def signed(self, **changes):
        args = dict(image=self.image, encrypted_key=self.encrypted, password=self.password,
                    public_key=self.public, expected_sha256=self.digest)
        args.update(changes)
        return signer.sign_image(**args)

    def test_real_signature_verifies_and_preserves_input(self):
        data = self.signed()
        self.assertEqual(data[:len(self.image)], self.image)
        self.assertEqual(len(data), 12288)
        with contextlib.redirect_stdout(io.StringIO()):
            espsecure.verify_signature(SimpleNamespace(version='2', datafile=io.BytesIO(data),
                                                       keyfile=io.BytesIO(self.public), hsm=False))

    def test_wrong_password_plaintext_and_empty_password_rejected(self):
        for change in [dict(password=b'wrong'), dict(password=b''), dict(encrypted_key=self.plain)]:
            with self.subTest(change=list(change)), self.assertRaises(ValueError):
                self.signed(**change)

    def test_wrong_identity_rejected(self):
        other = rsa.generate_private_key(public_exponent=65537, key_size=3072)
        public = other.public_key().public_bytes(serialization.Encoding.PEM,
                                               serialization.PublicFormat.SubjectPublicKeyInfo)
        with self.assertRaises(ValueError):
            self.signed(public_key=public)

    def test_image_digest_empty_and_malformed_digest_rejected(self):
        for change in [dict(image=b''), dict(image=self.image+b'X'),
                       dict(expected_sha256='0'*64), dict(expected_sha256='bad')]:
            with self.subTest(change=list(change)), self.assertRaises(ValueError):
                self.signed(**change)

    def test_corrupted_signer_output_not_returned(self):
        real = espsecure.sign_secure_boot_v2
        def corrupt(args):
            real(args)
            p = Path(args.output)
            data = bytearray(p.read_bytes())
            data[8192+100] ^= 1
            p.write_bytes(data)
        with patch.object(espsecure, 'sign_secure_boot_v2', side_effect=corrupt):
            with self.assertRaises(esptool.FatalError):
                self.signed()

    def cli(self, folder, prompt, tty=True):
        p = Path(folder)
        for name, data in [('image.bin', self.image), ('key.pem', self.encrypted), ('public.pem', self.public)]:
            (p/name).write_bytes(data)
        args = ['sign-firmware.py', '--image', str(p/'image.bin'), '--sha256', self.digest,
                '--encrypted-key', str(p/'key.pem'), '--public-key', str(p/'public.pem'),
                '--output', str(p/'signed.bin')]
        with patch('sys.argv', args), patch('sys.stdin.isatty', return_value=tty), \
             patch.object(signer.getpass, 'getpass', side_effect=prompt), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return signer.main()

    def test_cli_success_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(self.cli(temp, lambda _: self.password.decode()), 0)
            before = (Path(temp)/'signed.bin').read_bytes()
            with self.assertRaises(SystemExit):
                self.cli(temp, lambda _: self.fail('must not prompt for existing output'))
            self.assertEqual((Path(temp)/'signed.bin').read_bytes(), before)
            self.assertEqual((Path(temp)/'key.pem').read_bytes(), self.encrypted)
            self.assertEqual(sorted(p.name for p in Path(temp).iterdir()),
                             ['image.bin', 'key.pem', 'public.pem', 'signed.bin'])

    def test_noninteractive_and_echo_fallback_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(SystemExit):
                self.cli(temp, lambda _: self.fail('must not prompt'), tty=False)
            def echo_warning(_):
                warnings.warn('cannot hide input', getpass.GetPassWarning)
                self.fail('must stop before fallback input')
            self.assertEqual(self.cli(temp, echo_warning), 1)
            self.assertFalse((Path(temp)/'signed.bin').exists())

    def test_output_created_during_prompt_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temp:
            def race(_):
                (Path(temp)/'signed.bin').write_bytes(b'other-file')
                return self.password.decode()
            self.assertEqual(self.cli(temp, race), 1)
            self.assertEqual((Path(temp)/'signed.bin').read_bytes(), b'other-file')

    def test_input_read_errors_are_generic_and_do_not_prompt(self):
        for filename in ('image.bin', 'key.pem', 'public.pem'):
            for error in (FileNotFoundError, PermissionError):
                with self.subTest(filename=filename, error=error), tempfile.TemporaryDirectory() as temp:
                    real_read = Path.read_bytes
                    def read(path):
                        if path.name == filename:
                            raise error('sensitive-path-must-not-appear')
                        return real_read(path)
                    with patch.object(Path, 'read_bytes', read), \
                         patch.object(signer, 'print', create=True) as output:
                        self.assertEqual(self.cli(temp, lambda _: self.fail('must not prompt')), 1)
                    self.assertFalse((Path(temp)/'signed.bin').exists())
                    self.assertEqual(output.call_count, 1)
                    self.assertEqual(output.call_args.args,
                                     ('Signing failed: check image, encrypted key, password and public identity.',))

    def test_unsupported_algorithm_is_generic_and_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temp, \
             patch.object(signer.serialization, 'load_pem_private_key',
                          side_effect=UnsupportedAlgorithm('sensitive-error')), \
             patch.object(signer, 'print', create=True) as output:
            self.assertEqual(self.cli(temp, lambda _: self.password.decode()), 1)
            self.assertFalse((Path(temp)/'signed.bin').exists())
            self.assertEqual(output.call_count, 1)
            self.assertEqual(output.call_args.args,
                             ('Signing failed: check image, encrypted key, password and public identity.',))


if __name__ == '__main__':
    unittest.main()
