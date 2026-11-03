# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the real wrapper with instrumented build tools, not a real compiler.

These tests prove orchestration and failure propagation only, not Linux portability.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


WRAPPER = Path(__file__).resolve().parents[2] / "tools/test-host.sh"


class HostWrapper(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        tool_dir = self.root / "firmware/tools"
        tool_dir.mkdir(parents=True)
        self.script = tool_dir / "test-host.sh"
        shutil.copy2(WRAPPER, self.script)
        self.log = self.root / "calls.jsonl"
        bin_dir = self.root / "bin"
        bin_dir.mkdir()
        for name in ("cmake", "ctest"):
            tool = bin_dir / name
            tool.write_text(
                f"#!{sys.executable}\n"
                "import json, os, pathlib, sys\n"
                "name = pathlib.Path(sys.argv[0]).name\n"
                "phase = ('build' if '--build' in sys.argv else 'configure') "
                "if name == 'cmake' else 'test'\n"
                "with open(os.environ['CALL_LOG'], 'a') as log:\n"
                "    log.write(json.dumps({'phase': phase, 'args': sys.argv[1:], "
                "'asan': os.environ.get('ASAN_OPTIONS', ''), "
                "'ubsan': os.environ.get('UBSAN_OPTIONS', '')}) + '\\n')\n"
                "sys.exit(7 if os.environ.get('FAIL_PHASE') == phase else 0)\n"
            )
            tool.chmod(0o755)
        self.env = dict(os.environ)
        for key in ("PRD_BUILD_JOBS", "ASAN_OPTIONS", "UBSAN_OPTIONS", "FAIL_PHASE"):
            self.env.pop(key, None)
        self.env.update(PATH=f"{bin_dir}:{os.environ['PATH']}", CALL_LOG=str(self.log))

    def run_wrapper(self, *args):
        result = subprocess.run(
            [str(self.script), *args], env=self.env, text=True,
            capture_output=True, timeout=10,
        )
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def test_default_is_bounded_and_ctest_is_serial(self):
        result, calls = self.run_wrapper()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([c['phase'] for c in calls], ['configure', 'build', 'test'])
        self.assertEqual(calls[1]['args'][-2:], ['--parallel', '2'])
        self.assertNotIn('--parallel', calls[2]['args'])
        for phase in ('configure', 'build', 'test'):
            self.assertIn(f'{phase}_seconds=', result.stdout)

    def test_explicit_jobs(self):
        self.env['PRD_BUILD_JOBS'] = '3'
        result, calls = self.run_wrapper('--plain')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(calls[1]['args'][-2:], ['--parallel', '3'])

    def test_invalid_jobs_do_not_start_tools(self):
        for value in ('0', '-1', 'all', '2 3', '01', '1;true'):
            with self.subTest(value=value):
                self.env['PRD_BUILD_JOBS'] = value
                result, calls = self.run_wrapper()
                self.assertEqual(result.returncode, 2)
                self.assertEqual(calls, [])

    def test_extra_argument_is_rejected(self):
        result, calls = self.run_wrapper('--plain', '--sanitize')
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])

    def test_sanitizers_and_separate_build(self):
        result, calls = self.run_wrapper('--sanitize')
        self.assertEqual(result.returncode, 0)
        self.assertIn('-DPRD_ENABLE_SANITIZERS=ON', calls[0]['args'])
        self.assertIn('host-tests-asan', calls[1]['args'][1])
        self.assertIn('detect_leaks=0', calls[2]['asan'])
        self.assertIn('halt_on_error=1', calls[2]['asan'])
        self.assertIn('halt_on_error=1', calls[2]['ubsan'])

    def test_each_failure_stops_the_pipeline(self):
        for phase, count in (('configure', 1), ('build', 2), ('test', 3)):
            with self.subTest(phase=phase):
                self.log.unlink(missing_ok=True)
                self.env['FAIL_PHASE'] = phase
                result, calls = self.run_wrapper()
                self.assertEqual(result.returncode, 7)
                self.assertEqual(len(calls), count)
                self.assertNotIn(f'{phase}_seconds=', result.stdout)


if __name__ == '__main__':
    unittest.main()
