#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Check the completeness and internal consistency of a physical OTA evidence matrix.

This tool does not create physical evidence, inspect hardware, publish releases or handle keys. It
binds an owner-produced observation record to immutable serial-log bytes and rejects a matrix that
omits a required case or contradicts the expected boot-selection invariant.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import sys
from typing import Any
from urllib.parse import urlsplit


SCHEMA_VERSION = 2
MAX_LOG_BYTES = 16 * 1024 * 1024
OTA_TASK_STACK_BYTES = 12288
HEX64 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
PARTITION = re.compile(r"[A-Za-z0-9_.-]+")
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")


class MatrixError(ValueError):
    """The recorded matrix cannot support the claimed physical result."""


CASES: tuple[tuple[str, frozenset[str], str, bool], ...] = (
    ("valid-signed-install", frozenset({"reboot-required", "confirmed"}), "advance", True),
    ("incompatible-candidate", frozenset({"incompatible"}), "unchanged", True),
    ("digest-mismatch", frozenset({"digest-mismatch"}), "unchanged", True),
    ("signature-rejection", frozenset({"verification-failed"}), "unchanged", True),
    ("wrong-key-rejection", frozenset({"verification-failed"}), "unchanged", True),
    ("interrupted-download", frozenset({"truncated"}), "unchanged", True),
    (
        "interrupted-write-power-loss",
        frozenset({"current-image-recovered"}),
        "unchanged",
        False,
    ),
    ("pending-boot-rollback", frozenset({"reboot-required", "rolled-back"}), "rollback", True),
    ("final-recovery-install", frozenset({"reboot-required", "confirmed"}), "advance", True),
)
# Cases 6–9 deliberately retry one unchanged frozen local candidate. The first five cases each
# need their own candidate, so the nine physical runs consume exactly six distinct releases.
REUSED_RELEASE_FIRST_INDEX = 5


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise MatrixError(f"duplicate JSON member: {key!r}")
        result[key] = value
    return result


def worksheet() -> dict[str, Any]:
    """Prepare field names and case order without claiming any physical result."""
    return {
        "schemaVersion": SCHEMA_VERSION,
        "baselineCommit": "",
        "validationOrigin": "",
        "runs": [
            {
                "case": case,
                "tag": "",
                "releaseId": None,
                "fixtureFrozen": None,
                "candidateSha256": "",
                "observations": [],
                "beforeBootPartition": "",
                "selectedBootPartition": "",
                "afterBootPartition": "",
                "currentImageUsable": None,
                "workerStackFreeLowWaterBytes": None,
                "serialLog": f"{index:02d}-{case}.log",
                "serialLogSha256": "",
            }
            for index, (case, _, _, _) in enumerate(CASES, 1)
        ],
    }


def _member(value: Any, name: str, expected: type) -> Any:
    if not isinstance(value, dict) or name not in value or type(value[name]) is not expected:
        raise MatrixError(f"member {name!r} has the wrong type or is missing")
    return value[name]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _semver(tag: str) -> tuple[int, int, int]:
    match = TAG.fullmatch(tag)
    if match is None:
        raise MatrixError(f"release tag is not a stable vMAJOR.MINOR.PATCH value: {tag!r}")
    return tuple(int(part) for part in match.groups())


def _private_origin(origin: str) -> bool:
    try:
        parsed = urlsplit(origin)
        address = ipaddress.ip_address(parsed.hostname or "")
        return (
            parsed.scheme == "https"
            and parsed.port is not None
            and parsed.port > 0
            and origin == f"https://{address}:{parsed.port}"
            and isinstance(address, ipaddress.IPv4Address)
            and (
                address in ipaddress.ip_network("10.0.0.0/8")
                or address in ipaddress.ip_network("172.16.0.0/12")
                or address in ipaddress.ip_network("192.168.0.0/16")
            )
        )
    except ValueError:
        return False


def _evidence_file(root: Path, relative: str) -> Path:
    candidate = Path(relative)
    if candidate.is_absolute() or not candidate.parts or ".." in candidate.parts:
        raise MatrixError(f"serialLog must be a relative path inside the evidence directory: {relative!r}")
    unresolved = root / candidate
    if unresolved.is_symlink():
        raise MatrixError(f"serialLog must not be a symbolic link: {relative!r}")
    resolved = unresolved.resolve()
    try:
        resolved.relative_to(root.resolve())
    except ValueError as error:
        raise MatrixError(f"serialLog escapes the evidence directory: {relative!r}") from error
    if not resolved.is_file():
        raise MatrixError(f"serialLog is not a regular file: {relative!r}")
    size = resolved.stat().st_size
    if size == 0 or size > MAX_LOG_BYTES:
        raise MatrixError(f"serialLog has invalid size: {relative!r} ({size} bytes)")
    return resolved


def _partition(run: dict[str, Any], name: str) -> str:
    value = _member(run, name, str)
    if PARTITION.fullmatch(value) is None:
        raise MatrixError(f"{name} is not a bounded partition identity")
    return value


def _check_boot_invariant(case: str, relation: str, run: dict[str, Any]) -> None:
    before = _partition(run, "beforeBootPartition")
    selected = _partition(run, "selectedBootPartition")
    after = _partition(run, "afterBootPartition")
    if relation == "unchanged" and not (before == selected == after):
        raise MatrixError(f"{case}: rejected/interrupted candidate changed boot selection")
    if relation == "advance" and not (selected != before and after == selected):
        raise MatrixError(f"{case}: successful candidate did not remain the selected image")
    if relation == "rollback" and not (selected != before and after == before):
        raise MatrixError(f"{case}: pending candidate did not roll back to the previous image")


def verify(path: Path) -> dict[str, Any]:
    try:
        root = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise MatrixError(f"cannot read evidence JSON: {error}") from error
    if not isinstance(root, dict):
        raise MatrixError("evidence root must be an object")
    if _member(root, "schemaVersion", int) != SCHEMA_VERSION:
        raise MatrixError("unsupported evidence schema")
    baseline = _member(root, "baselineCommit", str)
    if COMMIT.fullmatch(baseline) is None:
        raise MatrixError("baselineCommit must be a full lowercase 40-character SHA")
    origin = _member(root, "validationOrigin", str)
    if not _private_origin(origin):
        raise MatrixError("validationOrigin must be an exact private IPv4 HTTPS origin")
    runs = _member(root, "runs", list)
    if len(runs) != len(CASES):
        raise MatrixError(f"matrix must contain exactly {len(CASES)} ordered runs")

    previous_version: tuple[int, int, int] | None = None
    installed_candidate_digest: str | None = None
    reused_release: tuple[str, int, str] | None = None
    release_ids: set[int] = set()
    previous_after_partition: str | None = None
    serial_logs: set[Path] = set()
    serial_log_hashes: set[str] = set()
    evidence_root = path.resolve().parent
    minimum_stack = OTA_TASK_STACK_BYTES

    for index, (expected_case, expected_observations, relation, stack_required) in enumerate(CASES):
        run = runs[index]
        if not isinstance(run, dict):
            raise MatrixError(f"run {index + 1} must be an object")
        case = _member(run, "case", str)
        if case != expected_case:
            raise MatrixError(f"run {index + 1} must be {expected_case!r}, found {case!r}")
        tag = _member(run, "tag", str)
        version = _semver(tag)
        release_id = _member(run, "releaseId", int)
        if release_id <= 0:
            raise MatrixError(f"{case}: releaseId must be positive")
        if _member(run, "fixtureFrozen", bool) is not True:
            raise MatrixError(f"{case}: local fixture was not recorded as frozen")
        candidate = _member(run, "candidateSha256", str)
        if HEX64.fullmatch(candidate) is None:
            raise MatrixError(f"{case}: candidateSha256 must be a lowercase SHA-256 digest")
        identity = (tag, release_id, candidate)
        if index <= REUSED_RELEASE_FIRST_INDEX:
            if previous_version is not None and version <= previous_version:
                raise MatrixError(f"{case}: new release tags must increase strictly")
            if release_id in release_ids:
                raise MatrixError(f"{case}: new releaseId must be unique")
            previous_version = version
            release_ids.add(release_id)
            if index == 0:
                installed_candidate_digest = candidate
            if index == REUSED_RELEASE_FIRST_INDEX:
                if candidate == installed_candidate_digest:
                    raise MatrixError(
                        f"{case}: final valid candidate must differ from the installed image"
                    )
                reused_release = identity
        elif identity != reused_release:
            raise MatrixError(
                f"{case}: final four runs must reuse the same tag, releaseId and candidateSha256"
            )

        observations = _member(run, "observations", list)
        if any(type(value) is not str for value in observations):
            raise MatrixError(f"{case}: observations must contain strings only")
        missing = expected_observations - set(observations)
        if missing:
            raise MatrixError(f"{case}: missing observation(s): {', '.join(sorted(missing))}")
        if len(observations) != len(set(observations)):
            raise MatrixError(f"{case}: duplicate observation")
        unexpected = set(observations) - expected_observations
        if unexpected:
            raise MatrixError(f"{case}: unexpected observation(s): {', '.join(sorted(unexpected))}")
        if _member(run, "currentImageUsable", bool) is not True:
            raise MatrixError(f"{case}: the previous/current installation was not recorded usable")
        _check_boot_invariant(case, relation, run)
        before = _partition(run, "beforeBootPartition")
        if previous_after_partition is not None and before != previous_after_partition:
            raise MatrixError(f"{case}: before boot partition does not follow the previous run")
        previous_after_partition = _partition(run, "afterBootPartition")

        if "workerStackFreeLowWaterBytes" not in run:
            raise MatrixError(f"{case}: worker stack low-water field is missing")
        stack = run["workerStackFreeLowWaterBytes"]
        if stack is not None and (type(stack) is not int or not 0 < stack <= OTA_TASK_STACK_BYTES):
            raise MatrixError(f"{case}: worker stack low-water value is invalid")
        if stack_required and stack is None:
            raise MatrixError(f"{case}: completed OTA command is missing worker stack evidence")
        if stack is not None:
            minimum_stack = min(minimum_stack, stack)

        relative_log = _member(run, "serialLog", str)
        log_path = _evidence_file(evidence_root, relative_log)
        if log_path in serial_logs:
            raise MatrixError(f"{case}: serialLog must identify a distinct capture")
        serial_logs.add(log_path)
        recorded_log_hash = _member(run, "serialLogSha256", str)
        if HEX64.fullmatch(recorded_log_hash) is None or _sha256(log_path) != recorded_log_hash:
            raise MatrixError(f"{case}: serialLogSha256 does not match the captured bytes")
        if recorded_log_hash in serial_log_hashes:
            raise MatrixError(f"{case}: serialLog bytes must identify a distinct capture")
        serial_log_hashes.add(recorded_log_hash)

    return {
        "baselineCommit": baseline,
        "validationOrigin": origin,
        "runs": len(runs),
        "distinctReleases": len(release_ids),
        "minimumWorkerStackFreeLowWaterBytes": minimum_stack,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("evidence", type=Path, help="physical OTA evidence JSON")
    parser.add_argument(
        "--init", action="store_true", help="create an unfilled worksheet (never a PASS)"
    )
    args = parser.parse_args()
    if args.init:
        try:
            with args.evidence.open("x", encoding="utf-8") as handle:
                json.dump(worksheet(), handle, indent=2)
                handle.write("\n")
        except OSError as error:
            print(f"OTA physical matrix: FAIL: cannot create worksheet: {error}", file=sys.stderr)
            return 1
        print("OTA physical matrix: TEMPLATE ONLY (no physical observations recorded)")
        return 0
    try:
        summary = verify(args.evidence)
    except MatrixError as error:
        print(f"OTA physical matrix: FAIL: {error}", file=sys.stderr)
        return 1
    print("OTA physical matrix: PASS (record consistency only; physical truth remains owner evidence)")
    for name, value in summary.items():
        print(f"  {name}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
