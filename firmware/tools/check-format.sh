#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Formatting check (Bootstrap Spec §12.1). Usage, from the repository root or
# from firmware/:
#
#   ./firmware/tools/check-format.sh          # check, print diff, fail on drift
#   ./firmware/tools/check-format.sh --fix    # rewrite files in place
#
# Formats project-owned .c/.h/.cpp/.hpp only. Generated, ESP-IDF and
# managed-component code is excluded (§12.1).
#
# clang-format is located in this order, first hit wins:
#   1. $CLANG_FORMAT
#   2. clang-format on PATH
#   3. Homebrew LLVM (/opt/homebrew/opt/llvm/bin)
#   4. xcrun clang-format  (Xcode command-line tools, macOS)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd "${FIRMWARE_DIR}/.." && pwd)"

MODE="check"
if [ $# -gt 0 ]; then
    case "$1" in
        --fix) MODE="fix" ;;
        --check) MODE="check" ;;
        *)
            echo "usage: $(basename "$0") [--check|--fix]" >&2
            exit 2
            ;;
    esac
fi

find_clang_format() {
    if [ -n "${CLANG_FORMAT:-}" ]; then
        echo "${CLANG_FORMAT}"
        return 0
    fi

    if command -v clang-format >/dev/null 2>&1; then
        command -v clang-format
        return 0
    fi

    if [ -x /opt/homebrew/opt/llvm/bin/clang-format ]; then
        echo /opt/homebrew/opt/llvm/bin/clang-format
        return 0
    fi

    if command -v xcrun >/dev/null 2>&1 && xcrun --find clang-format >/dev/null 2>&1; then
        xcrun --find clang-format
        return 0
    fi

    return 1
}

if ! CLANG_FORMAT_BIN="$(find_clang_format)"; then
    echo "error: clang-format not found." >&2
    echo "  macOS:  xcode-select --install   (or: brew install llvm)" >&2
    echo "  Or set CLANG_FORMAT to an explicit path." >&2
    exit 1
fi

echo "clang-format: ${CLANG_FORMAT_BIN}"
"${CLANG_FORMAT_BIN}" --version

# Project-owned source roots. `test/target/` is included even though it is
# empty today, so the reservation is formatted from its first commit.
SEARCH_ROOTS=(
    "${FIRMWARE_DIR}/components"
    "${FIRMWARE_DIR}/main"
    "${FIRMWARE_DIR}/test"
)

# Exclusions:
#   build/             generated build trees
#   managed_components ESP Component Manager dependencies (third party)
#   generated/         configure_file output, and the .hpp.in template's siblings
#   prd_font_*.c       the generated LVGL faces (tools/generate-fonts.py)
#
# The generated faces are excluded for the same reason they are excluded from the
# compiler warning policy in components/ui/CMakeLists.txt: they are machine output,
# not project code. Formatting them would also break reproducibility outright —
# `generate-fonts.py --check` compares the committed bytes against a fresh
# generation, and a formatter run in between makes the two differ permanently.
FILES=()
while IFS= read -r file; do
    FILES+=("${file}")
done < <(
    for root in "${SEARCH_ROOTS[@]}"; do
        [ -d "${root}" ] || continue
        find "${root}" \
            \( -name build -o -name managed_components -o -name generated \) -prune -o \
            -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
            -not -name 'prd_font_*.c' -print
    done | LC_ALL=C sort
)

if [ ${#FILES[@]} -eq 0 ]; then
    echo "error: no project-owned sources found; check the search roots" >&2
    exit 1
fi

echo "files: ${#FILES[@]}"

if [ "${MODE}" = "fix" ]; then
    "${CLANG_FORMAT_BIN}" -i --style=file "${FILES[@]}"
    echo "formatted ${#FILES[@]} file(s) in place"
    exit 0
fi

# --dry-run -Werror reports drift as an error but prints only a location, not
# the change. The diff is produced separately so CI output shows what to fix
# (§12.1: "CI shall fail on formatting differences and print the diff").
STATUS=0
DRIFTED=()

for file in "${FILES[@]}"; do
    if ! "${CLANG_FORMAT_BIN}" --dry-run -Werror --style=file "${file}" >/dev/null 2>&1; then
        DRIFTED+=("${file}")
        STATUS=1
    fi
done

if [ ${STATUS} -eq 0 ]; then
    echo "formatting OK"
    exit 0
fi

echo ""
echo "error: ${#DRIFTED[@]} file(s) are not formatted:"
for file in "${DRIFTED[@]}"; do
    echo "  ${file#"${REPO_DIR}/"}"
done

echo ""
echo "--- diff ---"
for file in "${DRIFTED[@]}"; do
    "${CLANG_FORMAT_BIN}" --style=file "${file}" |
        diff -u --label "a/${file#"${REPO_DIR}/"}" --label "b/${file#"${REPO_DIR}/"}" "${file}" - ||
        true
done

echo ""
echo "Fix with: ./firmware/tools/check-format.sh --fix"
exit 1
