# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Offline action-pin regression guard, not proof of hosted action execution.

The JavaScript actions declare runs.using=node24. The Pages upload composite
uses upload-artifact@bbbca2ddaa5d8feaa63e36b76fdaad77386f024f (v7.0.0),
also node24; its exact action.yml was inspected before selecting the pin.
Checkout v5 avoids v6's credential-storage migration; setup-node v5 disables its
new automatic cache. upload-artifact v5 still declared node20, hence v6 here.
Updating a pin requires checking the upstream runtime/inputs and hosted CI again.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
PINS = {
    "actions/checkout": "fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09",  # v5.1.0
    "actions/setup-node": "a0853c24544627f65ddf259abe73b1d18a591444",  # v5.0.0
    "actions/upload-artifact": "b7c566a772e6b6bfb58ed0dc250532a479d7789f",  # v6.0.0
    "actions/deploy-pages": "368f82528645a54fb793d4d04e342629a3f51346",  # v5.0.1, node24
    "actions/upload-pages-artifact": "fc324d3547104276b827a68afc52ff2a11cc49c9",  # v5.0.0 composite
    "actions/cache": "caa296126883cff596d87d8935842f9db880ef25",  # v5.1.0
    "astral-sh/setup-uv": "bec219d24cd3e171d82865faccec33120bb574f4",  # v10.1.0
}
USES = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.MULTILINE)


def validate_action_pins(text):
    refs = USES.findall(text)
    if not refs:
        raise ValueError("no action references")
    for ref in refs:
        name, separator, sha = ref.partition("@")
        if not separator or name not in PINS or sha != PINS[name]:
            raise ValueError(f"unreviewed action pin: {ref}")
    if "ACTIONS_ALLOW_USE_UNSECURE_NODE_VERSION" in text:
        raise ValueError("insecure Node runtime opt-out")


class CiActionTests(unittest.TestCase):
    def test_all_workflows_use_reviewed_node24_pins(self):
        workflows = sorted((ROOT / ".github/workflows").glob("*.y*ml"))
        self.assertEqual({p.name for p in workflows}, {"ci.yml", "desktop-installer.yml", "installer-pages.yml"})
        for path in workflows:
            with self.subTest(workflow=path.name):
                validate_action_pins(path.read_text(encoding="utf-8"))

    def test_each_old_node20_major_negative_control_fires(self):
        for name in PINS:
            if name.startswith("actions/"):
                with self.subTest(action=name):
                    with self.assertRaisesRegex(ValueError, "unreviewed action"):
                        validate_action_pins(f"- uses: {name}@v4\n")

    def test_floating_node24_tag_negative_control_fires(self):
        with self.assertRaisesRegex(ValueError, "unreviewed action"):
            validate_action_pins("- uses: actions/checkout@v5\n")

    def test_unreviewed_sha_negative_control_fires(self):
        with self.assertRaisesRegex(ValueError, "unreviewed action"):
            validate_action_pins(f"- uses: actions/checkout@{'a' * 40}\n")

    def test_unknown_action_negative_control_fires(self):
        with self.assertRaisesRegex(ValueError, "unreviewed action"):
            validate_action_pins(f"- uses: unknown/action@{'a' * 40}\n")

    def test_insecure_runtime_optout_negative_control_fires(self):
        text = f"- uses: actions/checkout@{PINS['actions/checkout']}\n"
        with self.assertRaisesRegex(ValueError, "insecure Node"):
            validate_action_pins(text + "env:\n  ACTIONS_ALLOW_USE_UNSECURE_NODE_VERSION: true\n")

    def test_setup_node_does_not_enable_new_automatic_cache(self):
        text = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        # Bound each check to its actual setup-node step, not a global string match.
        blocks = re.split(r"(?m)^      - ", text)
        setups = [b for b in blocks if f"actions/setup-node@{PINS['actions/setup-node']}" in b]
        self.assertEqual(len(setups), 2)
        for block in setups:
            self.assertRegex(block, r"(?m)^          package-manager-cache: false\s*$")
            self.assertIn("node-version: ${{ env.NODE_VERSION }}", block)
        self.assertIn("NODE_VERSION: 22.23.2", text)


if __name__ == "__main__":
    unittest.main()
