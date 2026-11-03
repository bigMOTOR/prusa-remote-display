#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# The deterministic browser-flow suite over the real served provisioning page.
#
# Runs on Node's built-in modules and a browser that is already installed. Nothing is downloaded,
# there is no npm dependency, and this is not part of any firmware build; it exists because three
# physical validation cycles were spent on page defects that a browser finds instantly and
# source-text assertions cannot see at all.
#
# The browser is resolved, version-checked, proven to start and printed by run.mjs — one place,
# so the binary that is checked is always the binary that runs. Set PRD_CHROME to override it.
# This wrapper only establishes the one thing Node cannot establish about itself.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO"

if ! command -v node > /dev/null 2>&1; then
    echo "test-browser: node is required (Node 22 is what CI pins; see ADR-0006)" >&2
    exit 1
fi

exec node firmware/test/browser/run.mjs
