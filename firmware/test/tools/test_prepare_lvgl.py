# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Fail-closed checks for the deterministic local callback transformation."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "prepare_lvgl", Path(__file__).resolve().parents[2] / "tools/prepare-lvgl.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def fixture():
    result = []
    for name in ("line", "radius", "angle", "fade", "map"):
        start = f"lv_draw_mask_{name}(lv_opa_t * mask_buf, int32_t abs_x,\n int32_t abs_y, int32_t len,\n "
        kind = f"lv_draw_sw_mask_{name}_param_t"
        result.extend((start + kind + " * param);\n", start + kind + " * p)\n{\n return 0;\n}\n",
                       f"param->dsc.cb = (lv_draw_sw_mask_xcb_t)lv_draw_mask_{name};\n"))
    return ''.join(result).encode()


def qr_fixture():
    return b"""/**********************
 *  STATIC PROTOTYPES
 **********************/
    uint8_t * qr0 = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    LV_ASSERT_MALLOC(qr0);
    uint8_t * data_tmp = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    LV_ASSERT_MALLOC(data_tmp);
        lv_free(qr0);
        lv_free(data_tmp);
    lv_free(qr0);
    lv_free(data_tmp);
"""


class PrepareLVGL(unittest.TestCase):
    def test_wrong_preimage_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "preimage"):
            MODULE.repair_callbacks(b"wrong")

    def test_five_signatures_and_assignments(self):
        before = fixture()
        with patch.object(MODULE, "BACKPORT_SHA256", MODULE.digest(before)):
            after = MODULE.repair_callbacks(before)
        self.assertEqual(after.count(b"void * param);"), 5)
        self.assertEqual(after.count(b"void * param)\n{"), 5)
        self.assertEqual(after.count(b" * p = param;"), 5)
        self.assertNotIn(b"(lv_draw_sw_mask_xcb_t)", after)
        self.assertEqual(after.count(b"return 0;"), 5)

    def test_missing_and_duplicate_each_site_rejected(self):
        original = fixture()
        for name in ("line", "radius", "angle", "fade", "map"):
            for token in (f"lv_draw_sw_mask_{name}_param_t * param);",
                          f"lv_draw_sw_mask_{name}_param_t * p)\n{{",
                          f"param->dsc.cb = (lv_draw_sw_mask_xcb_t)lv_draw_mask_{name};"):
                start = f"lv_draw_mask_{name}(lv_opa_t * mask_buf, int32_t abs_x,\n int32_t abs_y, int32_t len,\n "
                duplicate = token if token.startswith("param->") else start + token
                for data in (original.replace(token.encode(), b"BROKEN"), original + duplicate.encode()):
                    with self.subTest(name=name, token=token):
                        with patch.object(MODULE, "BACKPORT_SHA256", MODULE.digest(data)):
                            with self.assertRaisesRegex(RuntimeError, f"lv_draw_mask_{name}"):
                                MODULE.repair_callbacks(data)

    def test_final_postimage_is_checked(self):
        original = fixture() + MODULE.OLD
        backported = original.replace(MODULE.OLD, MODULE.NEW)
        with patch.object(MODULE, "ORIGINAL_SHA256", MODULE.digest(original)), \
                patch.object(MODULE, "BACKPORT_SHA256", MODULE.digest(backported)):
            with self.assertRaisesRegex(RuntimeError, "postimage mismatch"):
                MODULE.patched_mask(original)

    def test_qr_wrong_preimage_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "QR preimage"):
            MODULE.patched_qr(b"wrong")

    def test_qr_both_cleanup_sites_are_wiped(self):
        original = qr_fixture()
        preimage = MODULE.digest(original)
        with patch.object(MODULE, "QR_ORIGINAL_SHA256", preimage), \
                patch.object(MODULE, "QR_PATCHED_SHA256", "matched"), \
                patch.object(MODULE, "digest", side_effect=[preimage, "matched"]):
            result = MODULE.patched_qr(original)
        self.assertEqual(result.count(b"prd_wipe_qr_temp(qr0, qr_temp_size);"), 2)
        self.assertEqual(result.count(b"prd_wipe_qr_temp(data_tmp, qr_temp_size);"), 2)
        self.assertEqual(result.count(b"prd_qr_temp_audit(qr0, data_tmp, qr_temp_size);"), 2)
        self.assertEqual(result.count(b"lv_free(qr0);"), 2)
        self.assertEqual(result.count(b"lv_free(data_tmp);"), 2)

    def test_qr_missing_or_duplicate_site_rejected(self):
        original = qr_fixture()
        sites = (
            b"/**********************\n *  STATIC PROTOTYPES\n **********************/",
            b"    uint8_t * qr0 = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));\n"
            b"    LV_ASSERT_MALLOC(qr0);\n"
            b"    uint8_t * data_tmp = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));\n"
            b"    LV_ASSERT_MALLOC(data_tmp);\n",
            b"        lv_free(qr0);\n        lv_free(data_tmp);",
            b"    lv_free(qr0);\n    lv_free(data_tmp);",
        )
        for site in sites:
            for changed in (original.replace(site, b"BROKEN"), original + site):
                with self.subTest(site=site, changed=changed.endswith(site)):
                    with patch.object(MODULE, "QR_ORIGINAL_SHA256", MODULE.digest(changed)):
                        with self.assertRaisesRegex(RuntimeError, "changed"):
                            MODULE.patched_qr(changed)

    def test_qr_postimage_is_checked(self):
        original = qr_fixture()
        with patch.object(MODULE, "QR_ORIGINAL_SHA256", MODULE.digest(original)):
            with self.assertRaisesRegex(RuntimeError, "postimage mismatch"):
                MODULE.patched_qr(original)

    def test_qr_upgrade_requires_exact_original(self):
        original = b"pinned upstream QR"
        patched = b"locally scrubbed QR"
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "lv_qrcode.c"
            with patch.object(MODULE, "QR_ORIGINAL_SHA256", MODULE.digest(original)):
                MODULE.upgrade_unmodified_qr(target, patched)
                self.assertFalse(target.exists())
                target.write_bytes(b"locally modified")
                MODULE.upgrade_unmodified_qr(target, patched)
                self.assertEqual(target.read_bytes(), b"locally modified")
                target.write_bytes(original)
                MODULE.upgrade_unmodified_qr(target, patched)
                self.assertEqual(target.read_bytes(), patched)


if __name__ == '__main__':
    unittest.main()
