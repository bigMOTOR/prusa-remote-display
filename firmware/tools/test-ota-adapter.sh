#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
set -euo pipefail
: "${IDF_PATH:?Set IDF_PATH to the existing ESP-IDF v5.5.4 checkout}"
test "$(git -C "$IDF_PATH" describe --tags --always)" = "v5.5.4"
test -z "$(git -C "$IDF_PATH" status --porcelain -- components/app_update)"
crypto_dir="$IDF_PATH/components/mbedtls/mbedtls"
test "$(git -C "$crypto_dir" rev-parse HEAD)" = "ffb280bb63c78bfec1e1ab55040671768c85c923"
test -z "$(git -C "$crypto_dir" status --porcelain)"
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${PRD_OTA_ADAPTER_BUILD_DIR:-$script_dir/../build/ota-adapter-tests}"
cmake -S "$script_dir/../test/ota-adapter" -B "$build_dir" -DIDF_PATH="$IDF_PATH"
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure
