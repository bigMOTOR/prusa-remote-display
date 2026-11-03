# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the named CI command's environment setup, not the SDK or native suite."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
STEP = 'Test OTA staging adapter with pinned SHA-256 and sanitizers'


class OtaCiEnvironmentTests(unittest.TestCase):
    def command(self):
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        # Deliberately bounded to this plain, one-line run step, not a YAML parser.
        matches = re.findall(r'^      - name: ' + re.escape(STEP) +
                             r'\n        run: ([^\n]+)$', workflow, re.MULTILINE)
        self.assertEqual(len(matches), 1, 'Expected exactly one named OTA adapter run step')
        return matches[0]

    def run_in_fresh_shell(self, command):
        with tempfile.TemporaryDirectory(prefix='prd-ci-ota-env-') as directory:
            root = Path(directory)
            sdk = root / 'sdk'
            sdk.mkdir()
            (sdk / 'export.sh').write_text('export PRD_TEST_IDF_ACTIVATED=1\n')
            script = root / 'firmware/tools/test-ota-adapter.sh'
            script.parent.mkdir(parents=True)
            script.write_text('test "${PRD_TEST_IDF_ACTIVATED:-}" = 1 || exit 37\n'
                              'printf "adapter reached after activation\\n"\n')
            environment = {'PATH': os.defpath, 'IDF_PATH': str(sdk)}
            return subprocess.run(['bash', '--noprofile', '--norc', '-c', command],
                                  cwd=root, env=environment, capture_output=True, text=True)

    def test_actual_named_step_activates_idf_before_adapter(self):
        command = self.command()
        self.assertEqual(command, '. "${IDF_PATH}/export.sh" && bash ./firmware/tools/test-ota-adapter.sh')
        result = self.run_in_fresh_shell(command)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'adapter reached after activation\n')

    def test_missing_activation_control_fails_in_fresh_shell(self):
        result = self.run_in_fresh_shell('bash ./firmware/tools/test-ota-adapter.sh')
        self.assertEqual(result.returncode, 37)
        self.assertEqual(result.stdout, '')


if __name__ == '__main__':
    unittest.main()
