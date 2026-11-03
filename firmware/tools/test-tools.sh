#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Tool-level tests, over the packaging tool's own logic.
#
# The §13.5 release secret scan is a gate, and a gate whose behaviour is only ever observed by
# running a full release build is a gate nobody can reason about. Milestone 4 proved that: adding
# esp_http_client linked mbedTLS's PEM parser, whose format-string literals matched the old
# standalone marker, and the failure was indistinguishable from a real leak until the image was
# taken apart by hand.
#
# Python's own unittest, no dependency, no toolchain, and no ESP-IDF.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO"

exec python3 -m unittest discover --start-directory firmware/test/tools --pattern 'test_*.py' --verbose
