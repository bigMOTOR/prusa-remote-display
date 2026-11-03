# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Portable icon container checks, not a substitute for a Windows build."""
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from PySide6.QtGui import QImage
import build


class IconTests(unittest.TestCase):
    def test_source_transparency_and_windows_multi_size_container(self):
        source = Path(__file__).resolve().parent / "assets/app-icon.png"
        image = QImage(str(source))
        self.assertFalse(image.isNull())
        self.assertEqual(image.width(), image.height())
        self.assertEqual(image.pixelColor(0, 0).alpha(), 0)
        with tempfile.TemporaryDirectory() as temporary, patch.object(build.platform, "system", return_value="Windows"):
            icon = build.make_icon(source, Path(temporary)).read_bytes()
            self.assertEqual(struct.unpack_from("<HHH", icon), (0, 1, 7))
            for index, size in enumerate((16, 24, 32, 48, 64, 128, 256)):
                w, h, _, _, planes, bits, length, offset = struct.unpack_from("<BBBBHHII", icon, 6 + 16 * index)
                self.assertEqual((w or 256, h or 256, planes, bits), (size, size, 1, 32))
                frame = QImage.fromData(icon[offset:offset + length], "PNG")
                self.assertEqual((frame.width(), frame.height()), (size, size))
                self.assertTrue(frame.hasAlphaChannel())


if __name__ == "__main__":
    unittest.main()
