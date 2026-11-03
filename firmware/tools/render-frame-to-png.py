#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

"""Convert host-renderer RGB565 frames to PNG without third-party dependencies."""

import argparse
from pathlib import Path
import struct
import sys
import zlib


def chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload)))


def convert(source, destination, width=320, height=240, byte_order="little"):
    if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise ValueError("width and height must each be between 1 and 4096")
    if byte_order not in ("little", "big"):
        raise ValueError("byte order must be little or big")
    expected = width * height * 2
    source = Path(source)
    if not source.is_file() or source.stat().st_size != expected:
        raise ValueError(f"frame must be a regular file containing exactly {expected} bytes")
    with source.open("rb") as stream:
        raw = stream.read(expected + 1)
    if len(raw) != expected:
        raise ValueError("frame size changed while reading")
    pixels = struct.iter_unpack("<H" if byte_order == "little" else ">H", raw)
    rows = bytearray()
    for _ in range(height):
        rows.append(0)  # PNG filter: none
        for _ in range(width):
            value = next(pixels)[0]
            red, green, blue = value >> 11, (value >> 5) & 63, value & 31
            rows.extend(((red << 3) | (red >> 2), (green << 2) | (green >> 4),
                         (blue << 3) | (blue >> 2)))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    # Never replace existing screenshots, source frames, or evidence.
    with Path(destination).open("xb") as stream:
        stream.write(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--byte-order", choices=("little", "big"), default="little")
    args = parser.parse_args()
    try:
        convert(args.source, args.destination, args.width, args.height, args.byte_order)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(args.destination)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
