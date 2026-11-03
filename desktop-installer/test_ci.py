# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""CI policy controls; parsing is not a substitute for hosted platform evidence."""
import copy
import unittest
from pathlib import Path
from unittest.mock import patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
MATRIX = "${{ github.event_name == 'workflow_dispatch' && fromJSON('[\"ubuntu-24.04\",\"macos-14\",\"windows-2022\"]') || fromJSON('[\"ubuntu-24.04\"]') }}"
UV_ACTION = "astral-sh/setup-uv@bec219d24cd3e171d82865faccec33120bb574f4"


def validate_policy(workflow):
    """Fail closed on costly automatic native runs or weakened runtime/hash gates."""
    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(set(workflow["on"]) == {"push", "pull_request", "workflow_dispatch"},
            "unexpected desktop trigger")
    require(workflow["permissions"] == {"contents": "read"}, "unexpected permissions")
    require(workflow["concurrency"]["cancel-in-progress"] == "true", "no stale-run cancellation")
    jobs = workflow["jobs"]
    require(set(jobs) == {"desktop"}, "unexpected extra jobs")
    job = jobs["desktop"]
    require(job["strategy"]["matrix"]["os"] == MATRIX, "native jobs must be manual only")
    require(job["runs-on"] == "${{ matrix.os }}", "runner bypasses matrix")
    steps = job["steps"]
    setups = [step for step in steps if step.get("uses", "").startswith("astral-sh/setup-uv@")]
    require(len(setups) == 1 and setups[0]["uses"] == UV_ACTION, "unpinned runtime setup")
    options = setups[0]["with"]
    require(options["version"] == "0.11.6" and options["python-version"] == "3.12.13",
            "unexpected runtime version")
    require(options["activate-environment"] == "true"
            and options["venv-path"] == "${{ runner.temp }}/desktop-venv", "no isolated environment")
    installs = [step["run"] for step in steps if "bootstrap.lock" in step.get("run", "")]
    expected = ("uv pip install --require-hashes -r desktop-installer/bootstrap.lock\n"
                "uv pip install --no-build-isolation --require-hashes -r desktop-installer/requirements.lock\n")
    require(installs == [expected], "dependency hashes or backend order changed")
    for suite in ("desktop-installer", "firmware/test/signing"):
        matches = [step for step in steps if step.get("run") ==
                   f"python -B -m unittest discover -s {suite} -p 'test_*.py' -v"]
        require(len(matches) == 1 and "if" not in matches[0], "suite omitted or conditional")
    builds = [step for step in steps if "desktop-installer/build.py" in step.get("run", "")]
    require(len(builds) == 1 and builds[0].get("if") == "runner.os != 'Linux'",
            "native build is not target-only")


class CiPolicyTests(unittest.TestCase):
    def setUp(self):
        # BaseLoader keeps GitHub's 'on' key rather than YAML 1.1's boolean interpretation.
        self.workflow = yaml.load((ROOT / ".github/workflows/desktop-installer.yml").read_text(encoding="utf-8"),
                                  Loader=yaml.BaseLoader)

    def test_actual_workflow_preserves_policy(self):
        validate_policy(self.workflow)

    def test_automatic_native_matrix_negative_control_fires(self):
        broken = copy.deepcopy(self.workflow)
        broken["jobs"]["desktop"]["strategy"]["matrix"]["os"] = ["macos-14", "windows-2022"]
        with self.assertRaisesRegex(ValueError, "manual only"):
            validate_policy(broken)

    def test_floating_runtime_negative_control_fires(self):
        broken = copy.deepcopy(self.workflow)
        for step in broken["jobs"]["desktop"]["steps"]:
            if step.get("uses") == UV_ACTION:
                step["with"]["python-version"] = "3.12"
        with self.assertRaisesRegex(ValueError, "runtime version"):
            validate_policy(broken)

    def test_missing_hash_enforcement_negative_control_fires(self):
        broken = copy.deepcopy(self.workflow)
        for step in broken["jobs"]["desktop"]["steps"]:
            if "bootstrap.lock" in step.get("run", ""):
                step["run"] = step["run"].replace("--require-hashes", "")
        with self.assertRaisesRegex(ValueError, "dependency hashes"):
            validate_policy(broken)

    def test_linux_packaging_negative_control_fires(self):
        broken = copy.deepcopy(self.workflow)
        for step in broken["jobs"]["desktop"]["steps"]:
            if "build.py" in step.get("run", ""):
                step.pop("if")
        with self.assertRaisesRegex(ValueError, "target-only"):
            validate_policy(broken)

    def test_owner_reminder_and_exact_candidate_are_documented(self):
        readme = (ROOT / "desktop-installer/README.md").read_text(encoding="utf-8")
        self.assertIn("PM must remind the owner", readme)
        self.assertIn("Run workflow", readme)
        self.assertIn("full commit SHA", readme)
        self.assertIn("cancelled/skipped native job is insufficient", readme)

    def test_utf8_sources_do_not_depend_on_windows_default_encoding(self):
        real_read = Path.read_text

        def windows_read(path, encoding=None, errors=None):
            return real_read(path, encoding=encoding or "cp1252", errors=errors)

        with patch.object(Path, "read_text", autospec=True, side_effect=windows_read):
            # The old implicit read must fail on the actual document, not an ASCII fixture.
            with self.assertRaises(UnicodeDecodeError):
                (ROOT / "desktop-installer/README.md").read_text()
            self.setUp()
            validate_policy(self.workflow)
            self.test_owner_reminder_and_exact_candidate_are_documented()


if __name__ == "__main__":
    unittest.main()
