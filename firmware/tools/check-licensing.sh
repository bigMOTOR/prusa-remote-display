#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# Every tracked file carries the licence the accepted policy assigns it — or is a recorded
# exception with a stated reason.
#
# Open Source Licensing Decision 001 assigns a licence per repository area and requires new
# firmware/software sources to carry `SPDX-License-Identifier: GPL-3.0-only`. That was true of 280
# of 341 tracked files and drifted in the rest: one C++ file of 241 without an identifier, nine
# CMake files of twenty-one, `.gitignore` while its three sibling dotfiles had one. Drift is what
# happens to a convention nothing checks.
#
# ⛔ THIS GATE MUST NEVER BECOME A REASON TO RE-HEADER SOMEONE ELSE'S WORK. The rules and the
# exceptions live in `licensing-manifest` beside this script, every exception states why, and this
# script prints that reason. A vendored typeface, an upstream licence text, a serial capture that
# is evidence and a file whose parser would break are all deliberately outside the project header
# — and the manifest says so where anyone silencing a failure will read it.
#
# THREE CLASSIFICATIONS, BECAUSE TWO WERE NOT ENOUGH. `require` is project-owned: our copyright
# line and the area's identifier. `except` is outside the scheme, with a reason. `foreign` is the
# one the generated Inter faces forced into existence — a file this project generates and commits,
# and must therefore keep correct, whose licence is NOT ours to choose and which must NOT carry our
# copyright line. Collapsing that into `require` would have made the gate demand exactly the
# re-heading the OFL forbids; collapsing it into `except` would have stopped checking it at all.
#
# THE UNCLASSIFIED CHECK IS THE POINT. A tracked file matching no rule fails. New third-party
# material cannot arrive unnoticed, and a new file type forces a decision in the manifest rather
# than inheriting one by accident.
#
# Bash over `git ls-files`, no toolchain, same as its two siblings in the source-gates job.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
cd "$REPO"

MANIFEST="firmware/tools/licensing-manifest"
COPYRIGHT="SPDX-FileCopyrightText: 2026 Mykola Fiantsev"

# How far into a file a header may be. Generous enough for a shebang, a doctype or a CSS block
# opener, tight enough that a mention in prose further down cannot pass for a header — which is
# exactly how a count of 281 was once reported for 280 headers.
HEADER_LINES=8

if [ ! -f "$MANIFEST" ]; then
    echo "check-licensing: missing $MANIFEST" >&2
    exit 1
fi

# The manifest, read once into parallel arrays, order preserved: first match wins.
verbs=() globs=() extras=()
while IFS= read -r line; do
    case "$line" in
        '#'* | '') continue ;;
    esac
    IFS='|' read -r verb glob extra <<< "$line"
    # shellcheck disable=SC2001
    verb="$(echo "$verb" | tr -d '[:space:]')"
    glob="$(echo "$glob" | sed 's/^ *//; s/ *$//')"
    extra="$(echo "$extra" | sed 's/^ *//; s/ *$//')"
    case "$verb" in
        require | except | foreign) ;;
        *)
            echo "check-licensing: $MANIFEST: unknown verb '$verb'" >&2
            exit 1
            ;;
    esac
    if [ -z "$glob" ] || [ -z "$extra" ]; then
        echo "check-licensing: $MANIFEST: incomplete rule: $line" >&2
        exit 1
    fi
    verbs+=("$verb")
    globs+=("$glob")
    extras+=("$extra")
done < "$MANIFEST"

# ⛔ AN EMPTY FILE LIST IS A BROKEN CHECK, NOT A CLEAN ONE. Run outside a repository — an
# isolated verification tree, an exported tarball — `git ls-files` fails and prints nothing, and
# every loop below has nothing to iterate: the gate said "OK, 0 project-owned" and exited zero.
# That
# is the exact failure this project has paid for repeatedly, a check that found nothing being
# indistinguishable from a check that passed. The list is therefore taken once, up front, and its
# emptiness is fatal.
if ! TRACKED="$(git ls-files 2>/dev/null)" || [ -z "$TRACKED" ]; then
    echo "check-licensing: 'git ls-files' produced no files;" \
        "this must run inside the repository" >&2
    exit 1
fi

failures=0
checked=0
excepted=0
foreign=0

report() {
    echo "  $1"
    failures=$((failures + 1))
}

while IFS= read -r path; do
    matched=0
    for i in "${!globs[@]}"; do
        # An unquoted pattern in `case` is a glob, and `*` crosses `/` here — which is what makes
        # `firmware/*.cpp` mean "any .cpp under firmware/".
        pattern="${globs[$i]}"
        case "$path" in
            $pattern) ;;
            *) continue ;;
        esac
        matched=1

        if [ "${verbs[$i]}" = "except" ]; then
            excepted=$((excepted + 1))
            break
        fi

        want="${extras[$i]}"

        # `foreign`: the identifier must be right and our copyright line must be ABSENT. Asserting
        # this project's copyright over material licensed to someone else is the failure this
        # branch exists to catch, so it is checked rather than merely avoided by convention.
        if [ "${verbs[$i]}" = "foreign" ]; then
            head="$(head -n "$HEADER_LINES" "$path" 2>/dev/null || true)"
            escaped="$(printf '%s' "$want" | sed 's/[.+]/\\&/g')"
            if ! printf '%s' "$head" \
                | grep -qE "SPDX-License-Identifier: ${escaped}([^A-Za-z0-9.+-]|\$)"; then
                report "$path: no 'SPDX-License-Identifier: $want' — not ours to relicense"
            fi
            if printf '%s' "$head" | grep -qF "$COPYRIGHT"; then
                report "$path: carries our copyright line, but its licence is $want, not ours"
            fi
            if ! printf '%s' "$head" | grep -q "SPDX-FileCopyrightText:"; then
                report "$path: no upstream 'SPDX-FileCopyrightText:' notice"
            fi
            foreign=$((foreign + 1))
            break
        fi
        head="$(head -n "$HEADER_LINES" "$path" 2>/dev/null || true)"

        # ⬅️ NOT ANCHORED WITH `$`, AND THAT MATTERS. A single-line comment closes after the
        # identifier — `<!-- SPDX-License-Identifier: GPL-3.0-only -->` — so an end-of-line
        # anchor reported index.html as declaring other than it declares. The identifier
        # must instead be followed by end of line or a character that cannot extend it, so
        # `GPL-3.0-only` does not match `GPL-3.0-only-with-extra`. Dots are escaped because an
        # unescaped one would match any character.
        escaped="$(printf '%s' "$want" | sed 's/[.+]/\\&/g')"
        if ! printf '%s' "$head" \
            | grep -qE "SPDX-License-Identifier: ${escaped}([^A-Za-z0-9.+-]|\$)"; then
            found="$(printf '%s' "$head" | sed -n 's/.*SPDX-License-Identifier: *\([^ ]*\).*/\1/p' \
                | head -n 1)"
            if [ -n "$found" ]; then
                report "$path: declares '$found', the manifest requires '$want'"
            else
                report "$path: no 'SPDX-License-Identifier: $want' in the first $HEADER_LINES lines"
            fi
        fi

        if ! printf '%s' "$head" | grep -qF "$COPYRIGHT"; then
            report "$path: no '$COPYRIGHT' in the first $HEADER_LINES lines"
        fi

        checked=$((checked + 1))
        break
    done

    if [ "$matched" -eq 0 ]; then
        report "$path: matches no rule in $MANIFEST — decide which licence applies and record it"
    fi
done <<< "$TRACKED"

if [ "$failures" -gt 0 ]; then
    echo ""
    echo "check-licensing: $failures problem(s)."
    echo ""
    echo "If a file above is third-party, generated, evidence, or would break a parser, it does"
    echo "NOT get our header: add an 'except' rule to $MANIFEST with the reason. Never re-header"
    echo "upstream material as project-owned."
    exit 1
fi

echo "check-licensing: OK, $checked project-owned, $foreign foreign-licensed," \
    "$excepted recorded exceptions"
