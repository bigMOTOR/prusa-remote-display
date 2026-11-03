#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Static-analysis check (Bootstrap Spec §12.2). Usage, from the repository root
# or from firmware/:
#
#   ./firmware/tools/check-tidy.sh
#
# Analyses the project component translation units that the host test build
# compiles. It configures that build purely to obtain a compile database; it does
# not run the tests, and it never touches ESP-IDF or third-party sources.
#
# clang-tidy is located in this order, first hit wins:
#   1. $CLANG_TIDY
#   2. clang-tidy on PATH
#   3. Homebrew LLVM (/opt/homebrew/opt/llvm/bin)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${FIRMWARE_DIR}/.." && pwd)"

BUILD_DIR="${FIRMWARE_DIR}/build/tidy"

find_clang_tidy() {
    if [ -n "${CLANG_TIDY:-}" ]; then
        echo "${CLANG_TIDY}"
        return 0
    fi

    if command -v clang-tidy >/dev/null 2>&1; then
        command -v clang-tidy
        return 0
    fi

    if [ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]; then
        echo /opt/homebrew/opt/llvm/bin/clang-tidy
        return 0
    fi

    return 1
}

if ! CLANG_TIDY_BIN="$(find_clang_tidy)"; then
    echo "error: clang-tidy not found." >&2
    echo "  macOS:  brew install llvm" >&2
    echo "  Note: Apple's Xcode toolchain ships clang-format but NOT clang-tidy." >&2
    echo "  Or set CLANG_TIDY to an explicit path." >&2
    exit 1
fi

echo "clang-tidy: ${CLANG_TIDY_BIN}"
"${CLANG_TIDY_BIN}" --version | sed -n '2p;3p'

# Compile database from the host test build. CMAKE_EXPORT_COMPILE_COMMANDS gives
# clang-tidy the exact flags each TU is compiled with, so it analyses the code as
# the host build sees it rather than guessing.
cmake -S "${FIRMWARE_DIR}/test/host" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null

if [ ! -f "${BUILD_DIR}/compile_commands.json" ]; then
    echo "error: no compile database at ${BUILD_DIR}/compile_commands.json" >&2
    exit 1
fi

# Project component TUs only. GoogleTest lives under _deps/ and the host test
# sources are out of scope by documented decision — see the SCOPE note in
# .clang-tidy.
# A read loop rather than mapfile: macOS ships bash 3.2, which has no mapfile.
FILES=()
while IFS= read -r tu; do
    FILES+=("${tu}")
done < <(
    python3 - "${BUILD_DIR}/compile_commands.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    entries = json.load(handle)

files = {
    entry["file"]
    for entry in entries
    if "/_deps/" not in entry["file"] and "/firmware/components/" in entry["file"]
}

for path in sorted(files):
    print(path)
PY
)

if [ ${#FILES[@]} -eq 0 ]; then
    echo "error: no project component translation units in the compile database" >&2
    exit 1
fi

echo "files: ${#FILES[@]}"

EXTRA_ARGS=()

# Homebrew clang-tidy does not know where Apple's SDK keeps the C++ standard
# library, and without this every standard header resolves as "file not found".
# The resulting clang-diagnostic-errors do not merely hide real findings — they
# make clang-tidy misread types and emit confident nonsense, for example
# reporting a two-string_view function as taking two ints. Measured: 10 errors
# and several false positives without the sysroot, none with it.
if [ "$(uname -s)" = "Darwin" ] && command -v xcrun >/dev/null 2>&1; then
    SDK_PATH="$(xcrun --show-sdk-path)"
    EXTRA_ARGS+=(--extra-arg=-isysroot --extra-arg="${SDK_PATH}")
fi

STATUS=0
for file in "${FILES[@]}"; do
    echo "  ${file#"${REPO_DIR}/"}"
    if ! "${CLANG_TIDY_BIN}" \
        -p "${BUILD_DIR}" \
        --config-file="${REPO_DIR}/.clang-tidy" \
        --quiet \
        "${EXTRA_ARGS[@]}" \
        "${file}"; then
        STATUS=1
    fi
done

if [ ${STATUS} -ne 0 ]; then
    echo ""
    echo "error: clang-tidy reported findings (WarningsAsErrors is '*')" >&2
    echo "Fix them, or add a local NOLINT with a reason (Bootstrap Spec §12.3 rule 4)." >&2
    exit 1
fi

echo ""
echo "clang-tidy OK"
