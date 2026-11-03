#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Print the build identity of a firmware build directory.

Bootstrap Spec §14.2 lists the fields every firmware image shall expose. They are
generated into build_info_generated.hpp at CMake configure time; this reads that
header rather than the binary, because the metadata constants are `inline
constexpr` and unreferenced by firmware code, so the linker does not keep them.

Usage, from the repository root or from firmware/:

    ./firmware/tools/print-build-info.py                       # debug profile
    ./firmware/tools/print-build-info.py --profile release
    ./firmware/tools/print-build-info.py --json
    ./firmware/tools/print-build-info.py --check               # §14.5 purity scan

Output is deterministic: fields are emitted in a fixed order, and nothing is read
from the environment or the clock.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# §14.2 build identity, in the order the specification lists it. `target` is the
# ESP chip target and comes from project_description.json; everything else comes
# from the generated header.
FIELD_ORDER = (
    "productVersion",
    "gitCommit",
    "gitDescribe",
    "buildProfile",
    "idfVersion",
    "target",
    "boardTarget",
    "buildEpoch",
    "dirtyTree",
    "releaseTag",
)

HEADER_TO_FIELD = {
    "kProjectVersion": "productVersion",
    "kGitCommit": "gitCommit",
    "kGitDescribe": "gitDescribe",
    "kBuildProfile": "buildProfile",
    "kIdfVersion": "idfVersion",
    "kBoardTarget": "boardTarget",
    "kBuildEpoch": "buildEpoch",
    "kGitDirty": "dirtyTree",
    "kReleaseTag": "releaseTag",
}

STRING_CONSTANT = re.compile(
    r'^\s*inline\s+constexpr\s+char\s+(?P<name>k\w+)\s*\[\]\s*=\s*"(?P<value>[^"]*)"\s*;'
)
BOOL_CONSTANT = re.compile(
    r"^\s*inline\s+constexpr\s+bool\s+(?P<name>k\w+)\s*=\s*(?P<value>true|false)\s*;"
)

# §14.5: build metadata shall contain no usernames, absolute source paths,
# machine names, Wi-Fi information or credentials. These patterns are what a
# leak would actually look like on the platforms this project builds on.
FORBIDDEN_PATTERNS = (
    (re.compile(r"/Users/[^/\s]+"), "macOS home directory path"),
    (re.compile(r"/home/[^/\s]+"), "Linux home directory path"),
    (re.compile(r"[A-Za-z]:\\\\"), "Windows absolute path"),
    (re.compile(r"\.local\b"), "mDNS machine name"),
    (re.compile(r"(?i)\b(password|passwd|api[_-]?key|secret|token)\b"), "credential keyword"),
    (re.compile(r"(?i)\bssid\b"), "Wi-Fi network name"),
)


def repository_root() -> Path:
    """Resolve the firmware root from this script's own location (§10.2)."""
    return Path(__file__).resolve().parent.parent


def read_generated_header(build_dir: Path) -> dict[str, str]:
    header = (
        build_dir / "prd_generated" / "app_model" / "generated" / "build_info_generated.hpp"
    )

    if not header.is_file():
        raise SystemExit(
            f"error: no build metadata at {header}\n"
            f"  Build the profile first, e.g. ./firmware/tools/build.sh "
            f"{build_dir.name}"
        )

    fields: dict[str, str] = {}

    for line in header.read_text(encoding="utf-8").splitlines():
        match = STRING_CONSTANT.match(line) or BOOL_CONSTANT.match(line)
        if match is None:
            continue

        field = HEADER_TO_FIELD.get(match.group("name"))
        if field is not None:
            fields[field] = match.group("value")

    missing = [name for name in HEADER_TO_FIELD.values() if name not in fields]
    if missing:
        raise SystemExit(
            "error: build metadata is incomplete; missing "
            + ", ".join(sorted(missing))
            + f"\n  in {header}"
        )

    return fields


def read_chip_target(build_dir: Path) -> str:
    """The ESP chip target, which the generated header does not carry."""
    description = build_dir / "project_description.json"

    if not description.is_file():
        return "unknown"

    with description.open(encoding="utf-8") as handle:
        return str(json.load(handle).get("target", "unknown"))


def collect(build_dir: Path) -> dict[str, str]:
    fields = read_generated_header(build_dir)
    fields["target"] = read_chip_target(build_dir)
    return {name: fields[name] for name in FIELD_ORDER}


def check_purity(fields: dict[str, str]) -> list[str]:
    """Return one finding per §14.5 violation. Empty means clean."""
    findings: list[str] = []

    for name, value in fields.items():
        for pattern, description in FORBIDDEN_PATTERNS:
            if pattern.search(value):
                findings.append(f"{name}={value!r} contains a {description}")

    # §14.3: a release image must not carry local wall-clock time. A zero epoch
    # means Git metadata was unavailable, which the specification permits but
    # which must not pass silently in a release.
    if fields["buildProfile"] == "release" and fields["buildEpoch"] == "0":
        findings.append(
            "buildEpoch=0 in a release build: no Git or SOURCE_DATE_EPOCH "
            "metadata was available, so the build is not reproducible"
        )

    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--profile",
        default="debug",
        choices=("debug", "hardware-validation", "release"),
        help="build profile to read (default: debug)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        help="explicit build directory; overrides --profile",
    )
    parser.add_argument("--json", action="store_true", help="emit JSON")
    parser.add_argument(
        "--check",
        action="store_true",
        help="scan the metadata for §14.5 violations and exit non-zero on any",
    )
    args = parser.parse_args()

    build_dir = args.build_dir or (repository_root() / "build" / args.profile)
    fields = collect(build_dir)

    if args.check:
        findings = check_purity(fields)

        if findings:
            print("build metadata FAILED the §14.5 purity check:", file=sys.stderr)
            for finding in findings:
                print(f"  {finding}", file=sys.stderr)
            return 1

        print(f"build metadata clean ({len(fields)} fields checked)")
        return 0

    if args.json:
        print(json.dumps(fields, indent=2, sort_keys=True))
        return 0

    width = max(len(name) for name in fields)
    for name in FIELD_ORDER:
        print(f"{name.ljust(width)} = {fields[name]}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
