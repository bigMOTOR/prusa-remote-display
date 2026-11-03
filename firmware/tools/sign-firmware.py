#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Owner-local application signing, in the pinned ESP-IDF Python environment.

Requires an encrypted RSA-3072 PEM, a separately chosen public PEM and an exact
input SHA256. Password input is interactive only, never a CLI flag or environment
variable. No upload, installation, hardware configuration or key generation.
Plaintext private material exists in process memory; Python cannot guarantee its
secure erasure or prevent OS swapping/crash dumps. No plaintext key file is made.
"""
import argparse
import contextlib
import getpass
import hashlib
import io
from pathlib import Path
import re
import sys
import tempfile
from types import SimpleNamespace
import warnings

import espsecure
import esptool
from cryptography.exceptions import UnsupportedAlgorithm
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa


def sign_image(image: bytes, encrypted_key: bytes, password: bytes,
               public_key: bytes, expected_sha256: str) -> bytes:
    """Return verified signed bytes; inputs/keys must be supplied by the owner."""
    if not image or not re.fullmatch(r'[0-9a-fA-F]{64}', expected_sha256):
        raise ValueError('nonempty image and exact SHA256 are required')
    if hashlib.sha256(image).hexdigest() != expected_sha256.lower():
        raise ValueError('input image does not match the chosen SHA256')
    if not password or not encrypted_key.startswith(b'-----BEGIN ENCRYPTED PRIVATE KEY-----'):
        raise ValueError('encrypted PKCS8 key and nonempty password required')
    key = serialization.load_pem_private_key(encrypted_key, password=password)
    public = serialization.load_pem_public_key(public_key)
    if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != 3072:
        raise ValueError('expected RSA-3072 private key')
    if not isinstance(public, rsa.RSAPublicKey) or public.key_size != 3072:
        raise ValueError('expected RSA-3072 public key')
    if key.public_key().public_numbers() != public.public_numbers():
        raise ValueError('private key does not match the chosen public identity')
    plain = key.private_bytes(serialization.Encoding.PEM,
                              serialization.PrivateFormat.PKCS8,
                              serialization.NoEncryption())
    with tempfile.TemporaryDirectory(prefix='prd-sign-image-') as temp:
        signed = Path(temp) / 'signed.bin'
        source = io.BytesIO(image)
        source.name = 'captured-application'
        with contextlib.redirect_stdout(io.StringIO()):
            espsecure.sign_secure_boot_v2(SimpleNamespace(
                datafile=source, keyfile=[io.BytesIO(plain)],
                output=str(signed), signature=None, pub_key=None,
                append_signatures=False, hsm=False))
        data = signed.read_bytes()
    padded = (len(image) + 4095) // 4096 * 4096
    if (len(data) != padded + 4096 or
            data[:padded] != image + b'\xff' * (padded - len(image)) or
            data[padded + espsecure.SIG_BLOCK_SIZE:] != b'\xff' * (4096 - espsecure.SIG_BLOCK_SIZE)):
        raise ValueError('signer returned an unexpected image layout')
    with contextlib.redirect_stdout(io.StringIO()):
        espsecure.verify_signature(SimpleNamespace(version='2', datafile=io.BytesIO(data),
                                                   keyfile=io.BytesIO(public_key), hsm=False))
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--sha256', required=True, help='expected unsigned application SHA256')
    parser.add_argument('--encrypted-key', type=Path, required=True)
    parser.add_argument('--public-key', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='new output file; never overwritten')
    args = parser.parse_args()
    if not sys.stdin.isatty():
        parser.error('run interactively; piped password input is not supported')
    if args.output.exists():
        parser.error('output already exists; choose a new output file')
    # Snapshot input before prompting; never re-read mutable image bytes later.
    try:
        image = args.image.read_bytes()
        if hashlib.sha256(image).hexdigest() != args.sha256.lower():
            parser.error('input image does not match the chosen SHA256')
        encrypted = args.encrypted_key.read_bytes()
        public = args.public_key.read_bytes()
        with warnings.catch_warnings():
            warnings.simplefilter('error', getpass.GetPassWarning)
            password = getpass.getpass('OTA key password (hidden): ').encode('utf-8')
        data = sign_image(image, encrypted, password, public, args.sha256)
    except (OSError, ValueError, UnsupportedAlgorithm, getpass.GetPassWarning, esptool.FatalError):
        print('Signing failed: check image, encrypted key, password and public identity.', file=sys.stderr)
        return 1
    # Exclusive creation also rejects a destination created while the owner entered
    # the password, including a dangling symlink. Only already verified bytes go here.
    try:
        with args.output.open('xb') as target:
            target.write(data)
    except OSError:
        print('Cannot create signed output; nothing was overwritten. Discard any incomplete output.', file=sys.stderr)
        return 1
    print('Signed application SHA256:', hashlib.sha256(data).hexdigest())
    print('Application only. Run package-firmware.py for partition-fit and bundle checks.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
