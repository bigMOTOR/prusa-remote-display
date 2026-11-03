# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


SCRIPT = Path(__file__).resolve().parents[2] / "tools/render-frame-to-png.py"
SPEC = importlib.util.spec_from_file_location("render_frame", SCRIPT)
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class RenderFrameTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.source = Path(self.directory.name) / "input.rgb565"
        self.output = Path(self.directory.name) / "output.png"

    def decoded_rows(self):
        png = self.output.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        offset, image = 8, bytearray()
        while offset < len(png):
            size = struct.unpack_from(">I", png, offset)[0]
            kind = png[offset + 4:offset + 8]
            payload = png[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", png, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + payload))
            if kind == b"IHDR":
                self.assertEqual(payload, struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
            if kind == b"IDAT":
                image.extend(payload)
            offset += size + 12
        self.assertEqual(offset, len(png))
        self.assertEqual(kind, b"IEND")
        return zlib.decompress(image)

    def test_primary_colours_and_row_order_both_endians(self):
        for order, fmt in (("little", "<4H"), ("big", ">4H")):
            with self.subTest(order=order):
                output = self.output.with_name(order + ".png")
                self.source.write_bytes(struct.pack(fmt, 0xF800, 0x07E0, 0x001F, 0xFFFF))
                TOOL.convert(self.source, output, 2, 2, order)
                self.output = output
                self.assertEqual(self.decoded_rows(), b"\0\xff\0\0\0\xff\0\0\0\0\xff\xff\xff\xff")

    def test_wrong_size_and_missing_input_leave_no_output(self):
        for raw in (None, b"", b"x" * 7, b"x" * 9):
            with self.subTest(size=None if raw is None else len(raw)):
                if raw is not None:
                    self.source.write_bytes(raw)
                with self.assertRaises(ValueError):
                    TOOL.convert(self.source, self.output, 2, 2)
                self.assertFalse(self.output.exists())

    def test_invalid_dimensions_or_order_leave_no_output(self):
        for width, height, order in ((0, 2, "little"), (2, -1, "little"),
                                      (4097, 2, "little"), (2, 4097, "little"), (2, 2, "native")):
            with self.subTest(width=width, height=height, order=order):
                with self.assertRaises(ValueError):
                    TOOL.convert(self.source, self.output, width, height, order)
                self.assertFalse(self.output.exists())

    def test_existing_destination_and_input_are_preserved(self):
        raw = struct.pack("<4H", 0, 1, 2, 3)
        self.source.write_bytes(raw)
        self.output.write_bytes(b"keep this evidence")
        for destination in (self.output, self.source):
            with self.subTest(destination=destination):
                with self.assertRaises(FileExistsError):
                    TOOL.convert(self.source, destination, 2, 2)
        self.assertEqual(self.source.read_bytes(), raw)
        self.assertEqual(self.output.read_bytes(), b"keep this evidence")

    def test_documented_python_cli(self):
        self.source.write_bytes(struct.pack("<4H", 0xF800, 0x07E0, 0x001F, 0xFFFF))
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.source),
                                 str(self.output), "--width", "2", "--height", "2"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.decoded_rows()), 14)
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), str(self.source),
                                 str(self.output), "--width", "2", "--height", "2"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("error:", result.stderr)
