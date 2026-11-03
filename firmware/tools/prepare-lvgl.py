#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Prepare the pinned LVGL mask backport in ignored generated storage only."""

import hashlib
import os
import re
from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile

URL = "https://components-file.espressif.com/components/lvgl/lvgl/9.5.0/lvgl__lvgl-v9.5.0.zip"
ARCHIVE_SHA256 = "95bd2126bdcc8ccd3407eff7c9e76687e41a8a4312bac1538b3d644b461c07fb"
MASK = "src/draw/sw/lv_draw_sw_mask.c"
QR = "src/libs/qrcode/lv_qrcode.c"
ORIGINAL_SHA256 = "e1f852a16219644ae2d8a08c8bb49865a6f5af1bb3e2115ec3e423de46675c22"
BACKPORT_SHA256 = "d29fee0102035d0fde835f06582391763a99eecbf588c04b70c2cdb09fcaf36e"
PATCHED_SHA256 = "0d9192375527ee77ae88551942eebffe7b1476d8853c17b145186592a0bdb3ab"
QR_ORIGINAL_SHA256 = "34677f81ee2af29e8e3df7a301aeeb9068c7b64b57fa46c63365329fea001d83"
QR_PATCHED_SHA256 = "da75a56e463ebe215e660e50a456d324fb96f421b8624c2e128e05bba4811c5c"
# Upstream f9c059715db7cc7c3aeedd77f6c88437f6a22c49, applied without context fuzz.
OLD = b"cir_y[i] == y"
NEW = b"cir_y[i] <= y"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def repair_callbacks(backported):
    """Local C callback compatibility fix; not part of the named upstream backport."""
    if digest(backported) != BACKPORT_SHA256:
        raise RuntimeError("LVGL callback repair preimage mismatch")
    result = backported
    for name in ("line", "radius", "angle", "fade", "map"):
        function = f"lv_draw_mask_{name}"
        param_type = f"lv_draw_sw_mask_{name}_param_t"
        for argument, ending in (("param", ");"), ("p", ")\n{")):
            pattern = (
                rb"(" + function.encode() + rb"\(lv_opa_t \* mask_buf, int32_t abs_x,\n"
                rb" +int32_t abs_y, int32_t len,\n +)"
                + re.escape(f"{param_type} * {argument}{ending}".encode())
            )
            replacement = b"void * param);" if argument == "param" else (
                f"void * param)\n{{\n    {param_type} * p = param;".encode()
            )
            result, count = re.subn(pattern, lambda m: m[1] + replacement, result)
            if count != 1:
                raise RuntimeError(f"LVGL {function} {argument} signature mismatch")
        old = f"param->dsc.cb = (lv_draw_sw_mask_xcb_t){function};".encode()
        if result.count(old) != 1:
            raise RuntimeError(f"LVGL {function} callback assignment mismatch")
        result = result.replace(old, f"param->dsc.cb = {function};".encode())
    return result


def patched_mask(original):
    if digest(original) != ORIGINAL_SHA256 or original.count(OLD) != 1:
        raise RuntimeError("LVGL mask preimage differs from the pinned upstream source")
    backported = original.replace(OLD, NEW)
    if digest(backported) != BACKPORT_SHA256:
        raise RuntimeError("LVGL mask postimage differs from the exact upstream backport")
    patched = repair_callbacks(backported)
    if digest(patched) != PATCHED_SHA256:
        raise RuntimeError("LVGL callback repair postimage mismatch")
    return patched


def patched_qr(original):
    """Wipe LVGL's QR payload scratch and encoded modules before either is freed."""
    if digest(original) != QR_ORIGINAL_SHA256:
        raise RuntimeError("LVGL QR preimage differs from the pinned upstream source")

    marker = b"/**********************\n *  STATIC PROTOTYPES\n **********************/"
    addition = b"""/* Project security patch: these buffers can contain the setup credential. */
static void prd_wipe_qr_temp(void * data, size_t size)
{
    volatile uint8_t * bytes = (volatile uint8_t *)data;
    for(size_t index = 0; index < size; index++) bytes[index] = 0;
}

#ifdef PRD_QR_TEMP_AUDIT
extern void prd_qr_temp_audit(const uint8_t * qr, const uint8_t * data, size_t size);
#endif

"""
    if original.count(marker) != 1:
        raise RuntimeError("LVGL QR insertion point changed")
    result = original.replace(marker, addition + marker)

    allocation = b"""    uint8_t * qr0 = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    LV_ASSERT_MALLOC(qr0);
    uint8_t * data_tmp = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    LV_ASSERT_MALLOC(data_tmp);
"""
    replacement = b"""    const size_t qr_temp_size = qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version);
    uint8_t * qr0 = lv_malloc(qr_temp_size);
    LV_ASSERT_MALLOC(qr0);
    uint8_t * data_tmp = lv_malloc(qr_temp_size);
    LV_ASSERT_MALLOC(data_tmp);
"""
    if result.count(allocation) != 1:
        raise RuntimeError("LVGL QR allocation block changed")
    result = result.replace(allocation, replacement)

    for indent in (b"        ", b"    "):
        old = indent + b"lv_free(qr0);\n" + indent + b"lv_free(data_tmp);"
        new = (
            indent + b"#ifndef PRD_OLD_QR_TEMP_SCRUB\n"
            + indent + b"prd_wipe_qr_temp(qr0, qr_temp_size);\n"
            + indent + b"prd_wipe_qr_temp(data_tmp, qr_temp_size);\n"
            + indent + b"#endif\n"
            + indent + b"#ifdef PRD_QR_TEMP_AUDIT\n"
            + indent + b"prd_qr_temp_audit(qr0, data_tmp, qr_temp_size);\n"
            + indent + b"#endif\n"
            + old
        )
        if result.count(old) != 1:
            raise RuntimeError("LVGL QR cleanup block changed")
        result = result.replace(old, new)

    if digest(result) != QR_PATCHED_SHA256:
        raise RuntimeError("LVGL QR security patch postimage mismatch")
    return result


def upgrade_unmodified_qr(target, patched):
    """Upgrade only a generated QR source identical to the pinned upstream file."""
    if target.is_file() and digest(target.read_bytes()) == QR_ORIGINAL_SHA256:
        target.write_bytes(patched)


def main():
    firmware = Path(__file__).resolve().parent.parent
    generated = firmware / "build" / "dependencies"
    generated.mkdir(parents=True, exist_ok=True)
    archive = generated / "lvgl-9.5.0.zip"
    if not archive.exists():
        with tempfile.NamedTemporaryFile(dir=generated, suffix=".zip") as download:
            supplied = os.environ.get("PRD_LVGL_ARCHIVE")
            if supplied:
                with open(supplied, "rb") as source:
                    shutil.copyfileobj(source, download)
            else:
                with urllib.request.urlopen(URL, timeout=60) as source:
                    shutil.copyfileobj(source, download)
            download.flush()
            if digest(Path(download.name).read_bytes()) != ARCHIVE_SHA256:
                raise RuntimeError("LVGL archive checksum mismatch; refusing preparation")
            shutil.copyfile(download.name, archive)
    if digest(archive.read_bytes()) != ARCHIVE_SHA256:
        raise RuntimeError("Cached LVGL archive checksum mismatch")

    destination = generated / "lvgl__lvgl"
    with zipfile.ZipFile(archive) as source:
        original = source.read(MASK)
        patched = patched_mask(original)
        original_qr = source.read(QR)
        patched_qrcode = patched_qr(original_qr)
        if not destination.exists():
            with tempfile.TemporaryDirectory(dir=generated, prefix="prepare-") as temporary:
                staging = Path(temporary) / "lvgl__lvgl"
                staging.mkdir()
                for entry in source.infolist():
                    relative = Path(entry.filename)
                    if relative.is_absolute() or ".." in relative.parts:
                        raise RuntimeError("Unsafe archive member")
                    if entry.is_dir():
                        continue
                    target = staging / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    replacement = {MASK: patched, QR: patched_qrcode}.get(entry.filename)
                    target.write_bytes(replacement if replacement is not None else source.read(entry))
                staging.rename(destination)
        qr_target = destination / QR
        upgrade_unmodified_qr(qr_target, patched_qrcode)
        # Reconfigure must not trust a stale sentinel or silently overwrite local edits.
        expected_paths = {entry.filename for entry in source.infolist() if not entry.is_dir()}
        actual_paths = set()
        for path in destination.rglob('*'):
            if path.is_symlink():
                raise RuntimeError("Prepared LVGL contains an unexpected symlink")
            if path.is_file():
                actual_paths.add(path.relative_to(destination).as_posix())
        if actual_paths != expected_paths:
            raise RuntimeError("Prepared LVGL file inventory differs from the pinned archive")
        checked = 0
        for entry in source.infolist():
            if entry.is_dir():
                continue
            expected = {MASK: patched, QR: patched_qrcode}.get(entry.filename)
            if expected is None:
                expected = source.read(entry)
            target = destination / entry.filename
            if not target.is_file() or target.read_bytes() != expected:
                raise RuntimeError(f"Prepared LVGL differs at {entry.filename}; refusing build")
            checked += 1
    print(f"LVGL 9.5.0 + mask backport + QR temporary wipe: {checked} files verified; {destination}")


if __name__ == "__main__":
    main()
