# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Firing controls for the physical OTA evidence consistency gate."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prd_ota_physical_evidence", FIRMWARE / "tools/check-ota-physical-evidence.py"
)
assert SPEC is not None and SPEC.loader is not None
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)


class OtaPhysicalEvidenceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prd-ota-physical-evidence-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.path = self.directory / "evidence.json"
        runs = []
        previous_after = "ota_0"
        for index, (case, observations, relation, stack_required) in enumerate(matrix.CASES, 1):
            log = self.directory / f"{index:02d}-{case}.log"
            log.write_text(f"physical capture {case}\n", encoding="utf-8")
            release_number = min(index, 6)
            before = previous_after
            selected = before
            after = before
            if relation == "advance":
                selected = "ota_1" if before == "ota_0" else "ota_0"
                after = selected
            elif relation == "rollback":
                selected = "ota_1" if before == "ota_0" else "ota_0"
            runs.append(
                {
                    "case": case,
                    "tag": f"v1.0.{release_number}",
                    "releaseId": 1000 + release_number,
                    "fixtureFrozen": True,
                    "candidateSha256": f"{release_number:064x}",
                    "observations": sorted(observations),
                    "beforeBootPartition": before,
                    "selectedBootPartition": selected,
                    "afterBootPartition": after,
                    "currentImageUsable": True,
                    "workerStackFreeLowWaterBytes": 4096 if stack_required else None,
                    "serialLog": log.name,
                    "serialLogSha256": hashlib.sha256(log.read_bytes()).hexdigest(),
                }
            )
            previous_after = after
        self.document = {
            "schemaVersion": matrix.SCHEMA_VERSION,
            "baselineCommit": "a" * 40,
            "validationOrigin": "https://192.168.1.40:8443",
            "runs": runs,
        }
        self.valid_document = copy.deepcopy(self.document)
        self.write()

    def reset_document(self):
        self.document = copy.deepcopy(self.valid_document)

    def write(self):
        self.path.write_text(json.dumps(self.document, indent=2) + "\n", encoding="utf-8")

    def verify(self):
        self.write()
        return matrix.verify(self.path)

    def assert_rejected(self, message: str):
        self.write()
        with self.assertRaisesRegex(matrix.MatrixError, message):
            matrix.verify(self.path)

    def test_complete_ordered_matrix_passes(self):
        summary = self.verify()
        self.assertEqual(summary["runs"], len(matrix.CASES))
        self.assertEqual(summary["distinctReleases"], 6)
        self.assertEqual(summary["minimumWorkerStackFreeLowWaterBytes"], 4096)

    def test_initializer_creates_only_an_unfilled_worksheet_and_never_overwrites(self):
        worksheet = self.directory / "worksheet.json"
        command = [str(FIRMWARE / "tools/check-ota-physical-evidence.py"), "--init", str(worksheet)]
        first = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("TEMPLATE ONLY", first.stdout)
        self.assertNotIn("PASS", first.stdout)
        unfilled = json.loads(worksheet.read_text(encoding="utf-8"))
        self.assertEqual(unfilled["schemaVersion"], matrix.SCHEMA_VERSION)
        self.assertEqual(unfilled["baselineCommit"], "")
        self.assertEqual(
            [run["case"] for run in unfilled["runs"]], [case for case, *_ in matrix.CASES]
        )
        for run in unfilled["runs"]:
            self.assertEqual(run["observations"], [])
            self.assertIsNone(run["fixtureFrozen"])
            self.assertIsNone(run["currentImageUsable"])
            self.assertIsNone(run["workerStackFreeLowWaterBytes"])
            self.assertEqual(run["candidateSha256"], "")
            self.assertEqual(run["serialLogSha256"], "")
        with self.assertRaisesRegex(matrix.MatrixError, "baselineCommit"):
            matrix.verify(worksheet)
        original = worksheet.read_bytes()
        second = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("cannot create worksheet", second.stderr)
        self.assertEqual(worksheet.read_bytes(), original)

    def test_duplicate_json_members_are_rejected_at_any_depth(self):
        source = self.path.read_text(encoding="utf-8")
        self.path.write_text(
            source.replace('"schemaVersion": 2,', '"schemaVersion": 2, "schemaVersion": 2,', 1),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(matrix.MatrixError, "duplicate JSON member"):
            matrix.verify(self.path)
        self.path.write_text(
            source.replace('"tag": "v1.0.1",', '"tag": "v1.0.1", "tag": "v1.0.1",', 1),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(matrix.MatrixError, "duplicate JSON member"):
            matrix.verify(self.path)

    def test_schema_and_candidate_digest_shape_are_required(self):
        self.document["schemaVersion"] = 3
        self.assert_rejected("unsupported evidence schema")
        self.reset_document()
        self.document["runs"][0]["candidateSha256"] = "A" * 64
        self.assert_rejected("candidateSha256 must be a lowercase")

    def test_every_case_and_its_order_are_required(self):
        self.document["runs"].pop(2)
        self.assert_rejected("exactly 9 ordered runs")
        self.reset_document()
        self.document["runs"][1], self.document["runs"][2] = (
            self.document["runs"][2],
            self.document["runs"][1],
        )
        self.assert_rejected("run 2 must be")

    def test_release_sequence_must_be_stable_increasing_unique_and_frozen(self):
        controls = (
            ("tag", "v1.0.1", "increase strictly"),
            ("tag", "v1.0.2-beta", "stable vMAJOR"),
            ("releaseId", 1001, "new releaseId must be unique"),
            ("releaseId", 0, "releaseId must be positive"),
            ("fixtureFrozen", False, "not recorded as frozen"),
        )
        for field, value, message in controls:
            with self.subTest(field=field, value=value):
                original = copy.deepcopy(self.document)
                self.document["runs"][1][field] = value
                self.assert_rejected(message)
                self.document = original

    def test_final_four_runs_must_reuse_exactly_one_frozen_candidate(self):
        for index in range(6, 9):
            for field, value in (
                ("tag", "v1.0.7"),
                ("releaseId", 1007),
                ("candidateSha256", "7".zfill(64)),
            ):
                with self.subTest(index=index, field=field):
                    self.document["runs"][index][field] = value
                    self.assert_rejected("final four runs must reuse")
                    self.reset_document()
        self.document["runs"][8]["fixtureFrozen"] = False
        self.assert_rejected("not recorded as frozen")

    def test_each_new_release_has_a_distinct_identity(self):
        self.document["runs"][5]["tag"] = self.document["runs"][4]["tag"]
        self.assert_rejected("increase strictly")
        self.reset_document()
        self.document["runs"][5]["releaseId"] = self.document["runs"][4]["releaseId"]
        self.assert_rejected("new releaseId must be unique")

    def test_final_valid_candidate_cannot_be_the_installed_image(self):
        self.document["runs"][5]["candidateSha256"] = self.document["runs"][0][
            "candidateSha256"
        ]
        self.assert_rejected("final valid candidate must differ from the installed image")

    def test_each_case_requires_its_discriminating_observation(self):
        for index, (_, observations, _, _) in enumerate(matrix.CASES):
            with self.subTest(case=self.document["runs"][index]["case"]):
                original = copy.deepcopy(self.document)
                self.document["runs"][index]["observations"] = sorted(observations)[1:]
                self.assert_rejected("missing observation")
                self.document = original

    def test_observations_reject_duplicates_and_unexpected_outcomes(self):
        self.document["runs"][0]["observations"].append("confirmed")
        self.assert_rejected("duplicate observation")
        self.reset_document()
        self.document["runs"][0]["observations"].append("rolled-back")
        self.assert_rejected("unexpected observation")

    def test_rejected_and_interrupted_candidates_cannot_change_boot_selection(self):
        self.document["runs"][2]["selectedBootPartition"] = "ota_0"
        self.document["runs"][2]["afterBootPartition"] = "ota_0"
        self.assert_rejected("changed boot selection")

    def test_each_run_starts_from_the_previous_boot_result(self):
        self.document["runs"][1]["beforeBootPartition"] = "ota_0"
        self.document["runs"][1]["selectedBootPartition"] = "ota_0"
        self.document["runs"][1]["afterBootPartition"] = "ota_0"
        self.assert_rejected("does not follow the previous run")

    def test_success_and_rollback_partition_invariants_fire(self):
        self.document["runs"][0]["selectedBootPartition"] = "ota_0"
        self.document["runs"][0]["afterBootPartition"] = "ota_0"
        self.assert_rejected("did not remain the selected image")
        self.reset_document()
        self.document["runs"][7]["afterBootPartition"] = "ota_0"
        self.assert_rejected("did not roll back")

    def test_partition_identity_shape_is_bounded(self):
        self.document["runs"][0]["beforeBootPartition"] = "ota/0"
        self.assert_rejected("bounded partition identity")

    def test_completed_commands_require_bounded_worker_stack_evidence(self):
        self.document["runs"][1]["workerStackFreeLowWaterBytes"] = None
        self.assert_rejected("missing worker stack evidence")
        self.reset_document()
        self.document["runs"][1]["workerStackFreeLowWaterBytes"] = 12289
        self.assert_rejected("stack low-water value is invalid")
        self.reset_document()
        del self.document["runs"][6]["workerStackFreeLowWaterBytes"]
        self.assert_rejected("worker stack low-water field is missing")

    def test_serial_log_bytes_are_regular_distinct_and_hash_bound(self):
        self.document["runs"][0]["serialLogSha256"] = "0" * 64
        self.assert_rejected("does not match")
        self.reset_document()
        self.document["runs"][1]["serialLog"] = self.document["runs"][0]["serialLog"]
        self.assert_rejected("distinct capture")
        self.reset_document()
        self.document["runs"][1]["serialLog"] = "./" + self.document["runs"][0]["serialLog"]
        self.assert_rejected("distinct capture")
        self.reset_document()
        second = self.directory / self.document["runs"][1]["serialLog"]
        first = self.directory / self.document["runs"][0]["serialLog"]
        second.write_bytes(first.read_bytes())
        self.document["runs"][1]["serialLogSha256"] = hashlib.sha256(second.read_bytes()).hexdigest()
        self.assert_rejected("bytes must identify a distinct capture")
        self.reset_document()
        second.write_text("physical capture incompatible-candidate\n", encoding="utf-8")
        self.document["runs"][0]["serialLog"] = "../outside.log"
        self.assert_rejected("relative path")

    def test_serial_log_symbolic_links_are_rejected(self):
        original = self.directory / self.document["runs"][0]["serialLog"]
        symbolic = self.directory / "symbolic.log"
        symbolic.symlink_to(original.name)
        self.document["runs"][0]["serialLog"] = symbolic.name
        self.document["runs"][0]["serialLogSha256"] = hashlib.sha256(
            original.read_bytes()
        ).hexdigest()
        self.assert_rejected("must not be a symbolic link")

    def test_log_must_be_regular_nonempty_and_within_size_bound(self):
        log = self.directory / self.document["runs"][0]["serialLog"]
        log.write_bytes(b"")
        self.assert_rejected("invalid size")
        with log.open("wb") as handle:
            handle.truncate(matrix.MAX_LOG_BYTES + 1)
        self.assert_rejected("invalid size")
        self.reset_document()
        (self.directory / "log-directory").mkdir()
        self.document["runs"][0]["serialLog"] = "log-directory"
        self.assert_rejected("not a regular file")

    def test_symlinked_parent_cannot_escape_evidence_directory(self):
        outside = tempfile.TemporaryDirectory(prefix="prd-ota-external-")
        self.addCleanup(outside.cleanup)
        external_log = Path(outside.name) / "capture.log"
        external_log.write_text("external capture\n", encoding="utf-8")
        (self.directory / "escape").symlink_to(outside.name, target_is_directory=True)
        self.document["runs"][0]["serialLog"] = "escape/capture.log"
        self.assert_rejected("escapes the evidence directory")

    def test_identity_fields_and_usable_image_claim_are_strict(self):
        controls = (
            ("baselineCommit", "A" * 40, "full lowercase"),
            ("validationOrigin", "https://api.github.com:443", "exact private IPv4"),
            ("validationOrigin", "http://192.168.1.40:8443", "exact private IPv4"),
            ("validationOrigin", "https://192.168.1.40:0", "exact private IPv4"),
        )
        for field, value, message in controls:
            with self.subTest(field=field):
                original = copy.deepcopy(self.document)
                self.document[field] = value
                self.assert_rejected(message)
                self.document = original
        self.document["runs"][0]["currentImageUsable"] = False
        self.assert_rejected("not recorded usable")


if __name__ == "__main__":
    unittest.main()
