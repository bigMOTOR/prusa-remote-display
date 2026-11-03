# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Build a fresh, not distribution-signed desktop preview with no hardware access."""
import argparse
import importlib.metadata
import json
import os
import platform
import struct
import subprocess
import sys
from pathlib import Path


def make_icon(source, output):
    """Convert the unchanged original artwork into native icon containers."""
    from PySide6.QtCore import QBuffer, QByteArray, QIODevice, Qt
    from PySide6.QtGui import QImage

    original = QImage(str(source))
    if original.isNull() or not original.hasAlphaChannel():
        raise RuntimeError("icon must be a readable transparent PNG")

    def scaled(size):
        return original.scaled(size, size, Qt.AspectRatioMode.KeepAspectRatio,
                               Qt.TransformationMode.SmoothTransformation)

    if platform.system() == "Darwin":
        iconset = output / "app.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for multiplier in (1, 2):
                suffix = "@2x" if multiplier == 2 else ""
                if not scaled(size * multiplier).save(str(iconset / f"icon_{size}x{size}{suffix}.png")):
                    raise RuntimeError("could not encode Mac icon")
        icon = output / "app.icns"
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(icon)], check=True)
        return icon
    frames = []
    for size in (16, 24, 32, 48, 64, 128, 256):
        encoded = QByteArray()
        buffer = QBuffer(encoded)
        buffer.open(QIODevice.OpenModeFlag.WriteOnly)
        if not scaled(size).save(buffer, "PNG"):
            raise RuntimeError("could not encode Windows icon")
        frames.append((size, bytes(encoded)))
    offset = 6 + 16 * len(frames)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    for size, frame in frames:
        dimension = size if size < 256 else 0
        directory.extend(struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                                     len(frame), offset))
        offset += len(frame)
    icon = output / "app.ico"
    icon.write_bytes(bytes(directory) + b"".join(frame for _, frame in frames))
    return icon


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="fresh build directory outside the repository")
    args = parser.parse_args()
    output = args.output.resolve()
    root = Path(__file__).resolve().parents[1]
    if output.exists() or output == root or root in output.parents:
        parser.error("choose a fresh directory outside the repository")
    if sys.version_info[:2] != (3, 12):
        parser.error("use pinned Python 3.12")
    if platform.system() not in ("Darwin", "Windows"):
        parser.error("build Mac on Mac and Windows on Windows; cross-compilation is not claimed")
    pins = {"PySide6-Essentials": "6.11.2", "esptool": "4.12.0",
            "cryptography": "44.0.3", "certifi": "2026.7.22", "PyInstaller": "6.22.3"}
    for name, version in pins.items():
        if importlib.metadata.version(name) != version:
            parser.error(f"incorrect dependency version: {name}")
    output.mkdir(parents=True)
    icon = make_icon(root / "desktop-installer/assets/app-icon.png", output)
    command = [sys.executable, "-B", "-m", "PyInstaller", "--noconfirm", "--windowed", "--onedir",
               "--name", "Display Installer", "--distpath", str(output / "dist"),
               "--icon", str(icon),
               "--workpath", str(output / "work"), "--specpath", str(output),
               "--add-data", f"{root / 'firmware/tools/check-release-assets.py'}:firmware/tools",
               "--add-data", f"{root / 'LICENSE'}:.",
               "--add-data", f"{root / 'desktop-installer/assets'}:desktop-installer/assets",
               "--hidden-import", "serial.tools.list_ports", "--hidden-import", "espsecure",
               "--collect-submodules", "esptool", "--collect-data", "esptool",
               "--exclude-module", "PySide6.QtQml", "--exclude-module", "PySide6.QtQuick",
               "--exclude-module", "PySide6.QtNetwork", str(root / "desktop-installer/app.py")]
    # Deliberately no publisher.pem: this local candidate cannot access USB or install.
    with (output / "build.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT,
                       env={**os.environ, "PYINSTALLER_CONFIG_DIR": str(output / "cache"),
                            "PYTHONDONTWRITEBYTECODE": "1"})
    allocated = sum(p.stat().st_size for p in output.rglob("*") if p.is_file() and not p.is_symlink())
    if allocated > 512 * 1024 * 1024:
        raise RuntimeError("desktop build exceeds 512 MiB budget; inspect, do not delete blindly")
    versions = {d.metadata["Name"]: d.version for d in importlib.metadata.distributions()}
    (output / "provenance.json").write_text(json.dumps({
        "platform": platform.platform(), "python": sys.version, "dependencies": versions,
        "installEnabled": False, "distributionSigned": False,
        "localSigning": "ad-hoc" if platform.system() == "Darwin" else "none", "notarized": False,
        "bytes": allocated}, indent=2) + "\n")
    print(f"Built local preview: {output / 'dist'} ({allocated} bytes). Not a distribution release.")


if __name__ == "__main__":
    main()
