#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Host test wrapper (Bootstrap Spec §11.5, §12.5). Usage, from the repository
# root or from firmware/:
#
#   ./firmware/tools/test-host.sh              # plain build
#   ./firmware/tools/test-host.sh --sanitize    # AddressSanitizer + UBSan
#
# Host tests build with the native toolchain and do not require ESP-IDF.
# Compilation defaults to a conservative two jobs on every platform. For a
# larger local machine, set PRD_BUILD_JOBS explicitly (for example, 4).
#
# The two modes use separate build directories, so switching between them never
# reconfigures in place and a sanitizer run cannot leave instrumented objects
# behind for the plain run to link.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SANITIZE="OFF"
BUILD_DIR="${FIRMWARE_DIR}/build/host-tests"
BUILD_JOBS="${PRD_BUILD_JOBS:-2}"
if [[ ! "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "PRD_BUILD_JOBS must be a positive integer" >&2
    exit 2
fi

if [ $# -gt 1 ]; then
    echo "usage: $(basename "$0") [--plain|--sanitize]" >&2
    exit 2
fi

if [ $# -gt 0 ]; then
    case "$1" in
        --sanitize)
            SANITIZE="ON"
            BUILD_DIR="${FIRMWARE_DIR}/build/host-tests-asan"
            ;;
        --plain) ;;
        *)
            echo "usage: $(basename "$0") [--plain|--sanitize]" >&2
            exit 2
            ;;
    esac
fi

if [ "${SANITIZE}" = "ON" ]; then
    # halt_on_error keeps ASan's first report as the failure point rather than
    # letting a corrupted process produce a cascade. abort_on_error makes the
    # process die with a signal, which CTest records as a failure.
    # Preserve the ASan/UBSan scope on Linux too; leak coverage is not claimed.
    export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_stack_use_after_return=1:detect_leaks=0}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

    echo "sanitizers: address, undefined"
    echo "ASAN_OPTIONS=${ASAN_OPTIONS}"
    echo "UBSAN_OPTIONS=${UBSAN_OPTIONS}"
    echo ""
fi

phase_started=$SECONDS
cmake -S "${FIRMWARE_DIR}/test/host" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DPRD_ENABLE_SANITIZERS="${SANITIZE}"

echo "configure_seconds=$((SECONDS - phase_started))"
phase_started=$SECONDS
# A bare --parallel delegates an unlimited job count to Unix Makefiles.
# Keep compilation bounded; do not change CTest's per-test isolation.
echo "build_jobs=${BUILD_JOBS}"
cmake --build "${BUILD_DIR}" --parallel "$BUILD_JOBS"
echo "build_seconds=$((SECONDS - phase_started))"
phase_started=$SECONDS
ctest --test-dir "${BUILD_DIR}" --output-on-failure
echo "test_seconds=$((SECONDS - phase_started))"
