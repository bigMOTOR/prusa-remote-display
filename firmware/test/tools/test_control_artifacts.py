# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Retention guards; no renderer, compilation or physical correctness claim."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

PATH = Path(__file__).resolve().parents[2]/'tools/control-artifacts.py'
spec = importlib.util.spec_from_file_location('retention',PATH)
retention = importlib.util.module_from_spec(spec)
spec.loader.exec_module(retention)

class ControlArtifacts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.fw = Path(self.tmp.name).resolve()/'firmware'
        (self.fw/'tools').mkdir(parents=True)
        (self.fw/'tools/test-message-controls.sh').write_text('fixture')
        self.patch = patch.object(retention,'FW',self.fw)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.out = self.fw/'build/controller-tests-1'
        self.root = self.out/'message-controls-ABC123'
        self.root.mkdir(parents=True)
        retention.begin(self.root)
        self.control = self.root/'example-control'
        self.control.mkdir()
        for folder in ('objects','frames'):
            (self.control/folder).mkdir()
            (self.control/folder/('test.o' if folder == 'objects' else 'fixture-0.rgb565')).write_bytes(b'evidence')
        (self.control/'control').write_bytes(b'executable')
        (self.control/'control.log').write_text('FAIL exact assertion\n')

    def test_success_keeps_log_and_all_file_hashes_linux(self):
        retention.compact(self.control,'exact assertion')
        self.assertEqual({p.name for p in self.control.iterdir()}, {'control.log','evidence.json'})
        result = json.loads((self.control/'evidence.json').read_text())
        self.assertEqual(set(result['files_sha256']),{'objects/test.o','frames/fixture-0.rgb565','control','control.log'})
        self.assertEqual(result['status'],'expected-control-fired')

    def test_success_compacts_optional_macos_debug_symbols(self):
        (self.control/'control.dSYM/Contents/Resources/DWARF').mkdir(parents=True)
        (self.control/'control.dSYM/Contents/Resources/DWARF/control').write_text('symbols')
        retention.compact(self.control,'exact assertion')
        self.assertFalse((self.control/'control.dSYM').exists())

    def test_wrong_failure_preserves_everything(self):
        with self.assertRaises(ValueError):retention.compact(self.control,'different assertion')
        self.assertTrue((self.control/'frames/fixture-0.rgb565').exists())

    def test_sanitizer_failure_preserves_everything(self):
        for failure in ('ERROR: AddressSanitizer','runtime error: invalid value'):
            (self.control/'control.log').write_text('FAIL exact assertion\n'+failure)
            with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
            self.assertTrue((self.control/'objects/test.o').exists())

    def test_unknown_file_never_removed(self):
        (self.control/'important.bin').write_bytes(b'keep')
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        self.assertEqual((self.control/'important.bin').read_bytes(),b'keep')

    def test_child_symlink_rejected(self):
        outside = self.fw/'important'
        outside.write_bytes(b'keep')
        (self.control/'frames/link').symlink_to(outside)
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        self.assertEqual(outside.read_bytes(),b'keep')

    def test_unmarked_or_wrong_marker_refused(self):
        marker = self.root/retention.MARKER
        marker.unlink()
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        marker.write_text('{}')
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')

    def test_outside_allocator_refused(self):
        with self.assertRaises(ValueError):retention.compact(self.fw/'important','exact assertion')
        with self.assertRaises(ValueError):retention.allocator(self.fw)

    def test_reusing_run_refused(self):
        with self.assertRaises(ValueError):retention.begin(self.root)

    def test_budget_blocks_without_removing_failure_evidence(self):
        with patch.object(retention,'BUDGET',1):
            with self.assertRaises(ValueError):retention.check(self.out)
        self.assertTrue((self.control/'frames/fixture-0.rgb565').exists())

    def test_changed_payload_refuses_compaction(self):
        real = retention.checksum
        def change(path):
            result = real(path)
            (self.control/'frames/new.bin').write_bytes(b'new')
            return result
        with patch.object(retention,'checksum',side_effect=change):
            with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        self.assertTrue((self.control/'objects/test.o').exists())

    def test_nested_unknown_files_preserve_everything(self):
        for name in ('objects/.DS_Store','frames/keep-note.txt',
                     'control.dSYM/Contents/important.txt'):
            unknown = self.control/name
            unknown.parent.mkdir(parents=True,exist_ok=True)
            unknown.write_text('keep')
            with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
            self.assertEqual(unknown.read_text(),'keep')
            self.assertTrue((self.control/'objects/test.o').exists())
            unknown.unlink()

    def test_nested_unknown_directory_preserves_everything(self):
        (self.control/'objects/sub').mkdir()
        (self.control/'objects/sub/keep.txt').write_text('keep')
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        self.assertTrue((self.control/'objects/sub/keep.txt').exists())

    def test_macos_relocation_layout_allowed(self):
        relocation = self.control/'control.dSYM/Contents/Resources/Relocations/aarch64'
        relocation.mkdir(parents=True)
        (relocation/'control.yml').write_text('generated')
        retention.compact(self.control,'exact assertion')
        self.assertFalse((self.control/'control.dSYM').exists())

    def test_missing_log_and_invalid_marker_preserve_everything(self):
        (self.control/'control.log').unlink()
        with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
        (self.root/retention.MARKER).write_text('{broken')
        with self.assertRaises(json.JSONDecodeError):retention.compact(self.control,'exact assertion')
        self.assertTrue((self.control/'objects/test.o').exists())

    def test_prefix_or_merged_failure_line_refused(self):
        for text in ('FAIL exact assertion extra\n','PASS prefixFAIL exact assertion\n'):
            (self.control/'control.log').write_text(text)
            with self.assertRaises(ValueError):retention.compact(self.control,'exact assertion')
            self.assertTrue((self.control/'objects/test.o').exists())

    def test_wrapper_canonicalizes_checkout_and_uses_exact_failure_line(self):
        script = (PATH.parent/'test-message-controls.sh').read_text()
        self.assertIn('&& pwd -P)',script)
        self.assertIn('grep -qxF "FAIL $expected"',script)

    def test_checkout_alias_works_in_actual_wrapper_prelude(self):
        wrapper = (PATH.parent/'test-message-controls.sh').read_text()
        (self.fw/'tools/test-message-controls.sh').write_text(wrapper)
        shutil.copy2(PATH,self.fw/'tools/control-artifacts.py')
        (self.fw/'tools/prelude.sh').write_text(wrapper.split('\nSAN=',1)[0])
        alias = self.fw.parent/'checkout-alias'
        alias.symlink_to(self.fw,target_is_directory=True)
        result = subprocess.run(['bash',str(alias/'tools/prelude.sh'),'1'],
                                text=True,capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('controls output: '+str(self.out),result.stdout)
        self.assertNotIn('checkout-alias',result.stdout)

    def test_actual_wrapper_gates_compaction_after_sanitizer_check(self):
        script = (PATH.parent/'test-message-controls.sh').read_text()
        self.assertLess(script.index('check "$OUT"'),script.index('mktemp -d'))
        self.assertLess(script.index('CONTROL FAULTED'),script.index('compact "$scratch" "$expected"'))

if __name__ == '__main__':unittest.main()
