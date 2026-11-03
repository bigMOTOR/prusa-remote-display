#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Profile build wrapper (Bootstrap Spec §10.2). Usage, from the repository
# root or from firmware/:
#
#   ./firmware/tools/build.sh <debug|hardware-validation|release>
#
# Requires an activated ESP-IDF v5.5.4 environment (Bootstrap Spec §4.3):
#
#   . "$HOME/Developer/esp/esp-idf/export.sh"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

REQUIRED_IDF_VERSION="ESP-IDF v5.5.4"
BOARD_DEFAULTS="sdkconfig.defaults.waveshare_esp32_s3_lcd_2_8"

usage() {
    echo "usage: $(basename "$0") <debug|hardware-validation|release>" >&2
    exit 2
}

[ $# -eq 1 ] || usage

PROFILE="$1"
case "${PROFILE}" in
    debug|hardware-validation|release) ;;
    *) usage ;;
esac

if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py not found; activate ESP-IDF first:" >&2
    echo "  . \"\$HOME/Developer/esp/esp-idf/export.sh\"" >&2
    exit 1
fi

IDF_VERSION="$(idf.py --version)"
if [ "${IDF_VERSION}" != "${REQUIRED_IDF_VERSION}" ]; then
    echo "error: ${REQUIRED_IDF_VERSION} required; found '${IDF_VERSION}'" >&2
    exit 1
fi

BUILD_DIR="build/${PROFILE}"

# Defaults merge order per Bootstrap Spec §8.1: base -> board -> profile.
SDKCONFIG_DEFAULTS="sdkconfig.defaults;${BOARD_DEFAULTS};sdkconfig.defaults.${PROFILE}"

# Build metadata (Bootstrap Spec §14.2); the release profile additionally
# enforces the clean-tree rule (§14.4) at configure time.
export PRD_BUILD_PROFILE="${PROFILE}"

cd "${FIRMWARE_DIR}"

# Keep release compiler timestamps reproducible, including the bootloader (§14.3).
if [ "${PROFILE}" = "release" ] && [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$(git -C "${FIRMWARE_DIR}/.." show -s --format=%ct HEAD)"
    export SOURCE_DATE_EPOCH
fi

IDF_ARGS=(
    -B "${BUILD_DIR}"
    -D "SDKCONFIG=${BUILD_DIR}/sdkconfig"
    -D "SDKCONFIG_DEFAULTS=${SDKCONFIG_DEFAULTS}"
)

if [ ! -f "${BUILD_DIR}/sdkconfig" ]; then
    idf.py "${IDF_ARGS[@]}" set-target esp32s3
fi

# reconfigure keeps the Git build metadata fresh: it is collected at CMake
# configure time, so an incremental build alone could bake stale commit info.
idf.py "${IDF_ARGS[@]}" reconfigure build

idf.py "${IDF_ARGS[@]}" size

GENERATED_HEADER="${BUILD_DIR}/prd_generated/app_model/generated/build_info_generated.hpp"

echo ""
echo "Build metadata (${GENERATED_HEADER}):"
grep "inline constexpr" "${GENERATED_HEADER}" | sed 's/^ *//'
