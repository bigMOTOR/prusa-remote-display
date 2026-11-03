#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# NO PROJECT SOURCE MAY REACH AN eFUSE WRITE, as a build check.
#
# Owner ruling, 2026-08-14: "There must be no reachable eFuse-writing path in the
# firmware." eFuse writes are ONE-TIME PROGRAMMABLE and cannot be undone on the affected
# board, so this is a hardware-safety invariant rather than a style rule — and it is the
# kind that a review will eventually miss, because the dangerous call is not the one
# anybody writes.
#
# WHAT WENT WRONG, AND WHY THIS SCRIPT EXISTS. Milestone 3.1 originally called
# nvs_flash_init(). Under CONFIG_NVS_ENCRYPTION that API reads the NVS security config
# and, when no key is present, calls nvs_flash_generate_keys_v2(), which under the HMAC
# key-protection scheme reaches esp_efuse_write_key(EFUSE_BLK_KEY0, ...) — an irreversible
# burn on a virgin device. Nothing in the call site says "eFuse". The image built, every
# gate passed, and the burn would have happened on the first boot on real hardware.
#
# So the forbidden names below are not only the eFuse API. They include the ordinary-
# looking NVS initialisation APIs that reach it, because THOSE are what a future
# contributor will reasonably reach for.
#
# Usage:
#     ./firmware/tools/check-efuse-safety.sh
#
# Exits non-zero on the first finding, listing every one.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

SCAN_DIRS=(
    "${REPO_ROOT}/firmware/components"
    "${REPO_ROOT}/firmware/main"
)

for dir in "${SCAN_DIRS[@]}"; do
    if [[ ! -d "${dir}" ]]; then
        echo "check-efuse-safety: ${dir} not found" >&2
        exit 1
    fi
done

# Extended regular expressions, one per forbidden construct.
#
# nvs_flash_init is matched only when it is CALLED — `nvs_flash_init[[:space:]]*\(` — so
# that nvs_flash_init_partition(), which has no security path and is the API the project
# does use, is not caught by its own prefix.
PATTERNS=(
    'nvs_flash_init[[:space:]]*\('
    'nvs_flash_generate_keys'
    'nvs_flash_secure_init'
    'nvs_flash_read_security_cfg'
    'nvs_flash_register_security_scheme'
    'nvs_sec_provider'
    'esp_efuse_write_key'
    'esp_efuse_batch_write'
    'esp_efuse_write_field'
    'esp_efuse_write_block'
)

DESCRIPTIONS=(
    'nvs_flash_init() — reaches HMAC key generation and an eFuse burn under CONFIG_NVS_ENCRYPTION; use nvs_flash_init_partition()'
    'NVS key generation — writes an HMAC key to eFuse under the HMAC scheme'
    'nvs_flash_secure_init() — the encrypted path; deferred production hardening, no scheduled milestone'
    'NVS security-config read — part of the key-provisioning path'
    'NVS security-scheme registration — part of the key-provisioning path'
    'nvs_sec_provider — the component that owns the eFuse burn'
    'esp_efuse_write_key() — IRREVERSIBLE'
    'esp_efuse_batch_write — IRREVERSIBLE'
    'esp_efuse_write_field — IRREVERSIBLE'
    'esp_efuse_write_block — IRREVERSIBLE'
)

# Collected with a while-read loop rather than mapfile: macOS ships bash 3.2, which has no
# mapfile, and this script has to run identically on the developer machine and on the
# Linux CI container.
FILES=()
while IFS= read -r file; do
    FILES+=("${file}")
done < <(
    for dir in "${SCAN_DIRS[@]}"; do
        find "${dir}" -type f \
            \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
            -not -path '*/build/*'
    done | sort
)

if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "check-efuse-safety: no sources found" >&2
    exit 1
fi

# CODE ONLY, NOT PROSE. The forbidden APIs have to be nameable in comments — storage.cpp
# explains at length why nvs_flash_init() must never be called, and a check that forbade
# saying so would force the explanation out of the file that needs it most.
#
# Comments are BLANKED rather than deleted so grep -n still reports the real line number.
# A name inside a string literal may also be blanked; that can only hide a string, never a
# call, so it costs nothing this check is looking for.
strip_comments() {
    awk '
        {
            line = $0
            out = ""
            i = 1
            while (i <= length(line)) {
                c = substr(line, i, 1)
                two = substr(line, i, 2)
                if (in_block) {
                    if (two == "*/") { in_block = 0; i += 2 } else { i += 1 }
                    continue
                }
                if (two == "/*") { in_block = 1; i += 2; continue }
                if (two == "//") { break }
                out = out c
                i += 1
            }
            print out
        }
    ' "$1"
}

FOUND=0

for index in "${!PATTERNS[@]}"; do
    pattern="${PATTERNS[${index}]}"
    description="${DESCRIPTIONS[${index}]}"

    HITS=""
    for file in "${FILES[@]}"; do
        file_hits="$(strip_comments "${file}" | grep -nE "${pattern}" || true)"
        if [[ -n "${file_hits}" ]]; then
            HITS+="$(echo "${file_hits}" | sed "s|^|${file}:|")"$'\n'
        fi
    done

    if [[ -n "${HITS}" ]]; then
        if [[ ${FOUND} -eq 0 ]]; then
            echo "check-efuse-safety: FAILED" >&2
        fi
        FOUND=1
        echo >&2
        echo "  ${description}" >&2
        echo "${HITS}" | sed "s|^${REPO_ROOT}/|    |" >&2
    fi
done

if [[ ${FOUND} -ne 0 ]]; then
    echo >&2
    echo "check-efuse-safety: eFuse writes are ONE-TIME PROGRAMMABLE and cannot be undone." >&2
    echo "check-efuse-safety: encrypted NVS and HMAC/eFuse provisioning are deferred with no" >&2
    echo "check-efuse-safety: scheduled milestone; reaching them needs an explicit owner decision." >&2
    exit 1
fi

echo "check-efuse-safety: OK, ${#FILES[@]} files, ${#PATTERNS[@]} patterns"
