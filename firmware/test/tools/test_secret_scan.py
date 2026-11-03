# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Tool-level tests for §13.5's release secret scan.

⛔ WHY THESE EXIST. The private-key check used to be a standalone `BEGIN (RSA |EC )?PRIVATE KEY`
marker, and Milestone 4 broke it: esp_http_client pulls in esp-tls and mbedTLS, and libmbedcrypto's
PEM parser carries the delimiters it searches for as string literals. Every release build failed on a
parser format string. The replacement detects a whole PEM block — delimiter, payload, matching
delimiter — and these tests are what pin the difference in both directions.

⛔ NO REAL KEY IS COMMITTED. Every payload here is generated at run time from a fixed alphabet. They
are the right SHAPE and are not, and have never been, usable key material.
"""

import base64
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent.parent / "tools"

_spec = importlib.util.spec_from_file_location("package_firmware", TOOLS / "package-firmware.py")
assert _spec is not None and _spec.loader is not None
package_firmware = importlib.util.module_from_spec(_spec)
sys.modules["package_firmware"] = package_firmware
_spec.loader.exec_module(package_firmware)


def synthetic_payload(chars: int) -> bytes:
    """Base64 of a fixed byte pattern. Deterministic, non-secret, and never a usable key."""
    raw = bytes((index * 7 + 11) & 0xFF for index in range((chars * 3) // 4 + 3))
    return base64.b64encode(raw)[:chars]


def wrap(payload: bytes, width: int = 64, newline: bytes = b"\n") -> bytes:
    lines = [payload[at : at + width] for at in range(0, len(payload), width)]
    return b"".join(line + newline for line in lines)


def pem_block(label: bytes, payload_chars: int = 512, newline: bytes = b"\n",
              headers: bytes = b"") -> bytes:
    body = wrap(synthetic_payload(payload_chars), newline=newline)
    return (
        b"-----BEGIN " + label + b"-----" + newline
        + headers
        + body
        + b"-----END " + label + b"-----" + newline
    )


# The exact family libmbedcrypto's pem.c and pkparse.c carry, each a NUL-terminated constant with no
# payload after it. Taken from the literals the control build found in the release image.
MBEDTLS_PARSER_LITERALS = (
    b"-----BEGIN RSA PRIVATE KEY-----",
    b"-----END RSA PRIVATE KEY-----",
    b"-----BEGIN EC PRIVATE KEY-----",
    b"-----END EC PRIVATE KEY-----",
    b"-----BEGIN DSA PRIVATE KEY-----",
    b"-----END DSA PRIVATE KEY-----",
    b"-----BEGIN PRIVATE KEY-----",
    b"-----END PRIVATE KEY-----",
    b"-----BEGIN ENCRYPTED PRIVATE KEY-----",
    b"-----END ENCRYPTED PRIVATE KEY-----",
    b"-----BEGIN CERTIFICATE-----",
    b"-----END CERTIFICATE-----",
)


class WholeBlocksAreRejected(unittest.TestCase):
    """A complete block is a leak, whatever form it takes."""

    def test_rsa_block(self) -> None:
        self.assertEqual(
            package_firmware.find_pem_private_key(pem_block(b"RSA PRIVATE KEY")),
            "RSA PRIVATE KEY",
        )

    def test_ec_block(self) -> None:
        self.assertEqual(
            package_firmware.find_pem_private_key(pem_block(b"EC PRIVATE KEY")),
            "EC PRIVATE KEY",
        )

    def test_generic_pkcs8_block(self) -> None:
        self.assertEqual(
            package_firmware.find_pem_private_key(pem_block(b"PRIVATE KEY")),
            "PRIVATE KEY",
        )

    def test_crlf_form(self) -> None:
        self.assertEqual(
            package_firmware.find_pem_private_key(
                pem_block(b"RSA PRIVATE KEY", newline=b"\r\n")
            ),
            "RSA PRIVATE KEY",
        )

    def test_encrypted_block_with_rfc1421_headers(self) -> None:
        headers = b"Proc-Type: 4,ENCRYPTED\nDEK-Info: AES-256-CBC,0123456789ABCDEF\n\n"
        self.assertEqual(
            package_firmware.find_pem_private_key(
                pem_block(b"RSA PRIVATE KEY", headers=headers)
            ),
            "RSA PRIVATE KEY",
        )

    def test_a_block_embedded_in_binary_noise_is_still_found(self) -> None:
        # The real scan target is a firmware image, not a text file.
        data = bytes(range(256)) * 8 + pem_block(b"EC PRIVATE KEY") + b"\x00" * 4096
        self.assertEqual(package_firmware.find_pem_private_key(data), "EC PRIVATE KEY")


class ParserLiteralsAreAccepted(unittest.TestCase):
    """⛔ THE REGRESSION. A delimiter with no key behind it is not a key."""

    def test_each_literal_alone(self) -> None:
        for literal in MBEDTLS_PARSER_LITERALS:
            with self.subTest(literal=literal):
                self.assertIsNone(package_firmware.find_pem_private_key(literal))

    def test_the_whole_family_as_the_linker_lays_it_out(self) -> None:
        # NUL-separated constants in one rodata run, which is exactly what the control build found.
        data = b"\x00".join(MBEDTLS_PARSER_LITERALS)
        self.assertIsNone(package_firmware.find_pem_private_key(data))

    def test_standalone_begin_marker(self) -> None:
        self.assertIsNone(
            package_firmware.find_pem_private_key(b"-----BEGIN RSA PRIVATE KEY-----\n")
        )

    def test_standalone_end_marker(self) -> None:
        self.assertIsNone(
            package_firmware.find_pem_private_key(b"-----END RSA PRIVATE KEY-----\n")
        )

    def test_begin_and_end_adjacent_with_no_payload(self) -> None:
        self.assertIsNone(
            package_firmware.find_pem_private_key(
                b"-----BEGIN PRIVATE KEY-----\n-----END PRIVATE KEY-----\n"
            )
        )


class TruncatedCombinationsDoNotFalsePositive(unittest.TestCase):
    """Partial blocks must not become arbitrary findings in either direction."""

    def test_missing_footer(self) -> None:
        block = pem_block(b"RSA PRIVATE KEY")
        self.assertIsNone(
            package_firmware.find_pem_private_key(block[: block.rindex(b"-----END")])
        )

    def test_missing_header(self) -> None:
        block = pem_block(b"RSA PRIVATE KEY")
        self.assertIsNone(
            package_firmware.find_pem_private_key(block[block.index(b"\n") + 1 :])
        )

    def test_payload_below_the_floor(self) -> None:
        # A handful of encoded-looking bytes between two delimiters is not key material.
        self.assertIsNone(package_firmware.find_pem_private_key(pem_block(b"PRIVATE KEY", 16)))

    def test_payload_at_the_floor_is_rejected(self) -> None:
        # POSITIVE CONTROL for the floor itself, so the test above is about the length and not about
        # the block never matching.
        self.assertEqual(
            package_firmware.find_pem_private_key(pem_block(b"PRIVATE KEY", 64)),
            "PRIVATE KEY",
        )

    def test_mismatched_labels_do_not_close_a_block(self) -> None:
        data = (
            b"-----BEGIN RSA PRIVATE KEY-----\n"
            + wrap(synthetic_payload(512))
            + b"-----END EC PRIVATE KEY-----\n"
        )
        self.assertIsNone(package_firmware.find_pem_private_key(data))

    def test_a_certificate_block_is_not_a_private_key(self) -> None:
        # Real, complete, and out of scope: this check is about private keys.
        self.assertIsNone(package_firmware.find_pem_private_key(pem_block(b"CERTIFICATE")))

    def test_non_base64_body_is_not_a_payload(self) -> None:
        data = (
            b"-----BEGIN PRIVATE KEY-----\n"
            + b"see the documentation for the key format, which is not this\n"
            + b"-----END PRIVATE KEY-----\n"
        )
        self.assertIsNone(package_firmware.find_pem_private_key(data))


class TheOtherChecksAreUnchanged(unittest.TestCase):
    """⛔ NOTHING ELSE WAS WEAKENED. The password, API-key, SSID and path gates, still firing."""

    def scan(self, content: bytes, shipped: bool = True) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "prusa_remote_display.bin"
            target.write_bytes(content)
            return package_firmware.scan_for_secrets([(target, shipped)])

    def test_password_assignment_still_fails(self) -> None:
        errors, _ = self.scan(b"\x00\x00password=hunter2\x00")
        self.assertTrue(any("password assignment" in error for error in errors), errors)

    def test_api_key_assignment_still_fails(self) -> None:
        errors, _ = self.scan(b"api_key: Zx9QsT4bC1\x00")
        self.assertTrue(any("API key assignment" in error for error in errors), errors)

    def test_ssid_assignment_still_fails(self) -> None:
        errors, _ = self.scan(b"ssid=HomeNetwork\x00")
        self.assertTrue(any("SSID assignment" in error for error in errors), errors)

    def test_a_bare_field_label_is_still_permitted(self) -> None:
        # The distinction the pattern already made: a label with no value is redaction working.
        errors, _ = self.scan(b" ssid=\x00wifi for ssid:%s\x00")
        self.assertEqual(errors, [])

    def test_shipped_absolute_path_still_fails(self) -> None:
        errors, notes = self.scan(b"/Users/someone/build/app.c\x00")
        self.assertTrue(any("directory path" in error for error in errors), errors)
        self.assertEqual(notes, [])

    def test_diagnostic_absolute_path_is_a_note_not_an_error(self) -> None:
        errors, notes = self.scan(b"/Users/someone/build/app.c\x00", shipped=False)
        self.assertEqual(errors, [])
        self.assertTrue(any("directory path" in note for note in notes), notes)

    def test_a_whole_key_block_fails_even_in_a_diagnostic_file(self) -> None:
        # ⛔ A KEY IS AN ERROR WHEREVER IT IS. Only path findings are downgraded for unshipped files.
        errors, _ = self.scan(pem_block(b"RSA PRIVATE KEY"), shipped=False)
        self.assertTrue(any("private key block" in error for error in errors), errors)

    def test_the_parser_literals_do_not_fail_a_shipped_binary(self) -> None:
        errors, _ = self.scan(b"\x00".join(MBEDTLS_PARSER_LITERALS) + b"\x00")
        self.assertEqual(errors, [])

    def test_the_finding_never_carries_key_material(self) -> None:
        payload = synthetic_payload(512)
        errors, _ = self.scan(pem_block(b"RSA PRIVATE KEY"))
        self.assertEqual(len(errors), 1)
        self.assertIn("-----BEGIN RSA PRIVATE KEY-----", errors[0])
        self.assertNotIn(payload[:32].decode("ascii"), errors[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
