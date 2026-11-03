#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""NO PROJECT-OWNED LOG CALL MAY TAKE A CREDENTIAL VALUE, as a direct-source build check.

⛔ WHY THIS SCRIPT EXISTS. Architecture Spec §13.2 places `wifiSsid` inside SecretConfigV1 and §13.5
forbids credentials in logs, unqualified. The owner clarification of 2026-08-23 settled that the
actual home network name is credential material under §§7.3, 13.2 and 13.5.

`network::restart()` ended an Info line in `ssid=` plus the configured network name. Release compiles
Info in (CONFIG_LOG_MAXIMUM_LEVEL_INFO), so the user's SSID reached the release log — and NOTHING
CAUGHT IT for two reasons that are both worth writing down:

  - The path needs a SECOND same-boot credential submission. restart() runs only when the station is
    already up, so first-time provisioning takes start(), and no bench capture of a first
    provisioning could ever contain the line.
  - The §13.5 packaging scan is STATIC. It searches the image for fixed needles, and the SSID is
    runtime data concatenated onto a format string. No scan of any artifact could detect it.

So the gap was structural: the only place that defect is visible is the source, and nothing was
reading the source for it. That is this script.

⛔ WHAT THIS IS, AND WHAT IT IS NOT — READ BEFORE RELYING ON IT.

THIS IS: a deterministic regression guard over the DIRECT SOURCE TEXT of project-owned logging calls,
for the specific shapes this repository is known to write. It catches the defect that shipped, the
neighbouring shapes a contributor would plausibly write next, and any reuse of the recorded
identifiers.

THIS IS NOT: whole-program analysis. It performs no alias analysis, no data-flow tracking, no type
resolution and no interprocedural reasoning. A credential copied into a differently named local, or
passed through a helper that formats it, or reached through a reference or a pointer, will NOT be
detected. It therefore does NOT prove that no credential can ever reach a log — no source-text
scanner can, and describing it that way would be the same kind of overstatement that let
wifi_log_policy.hpp assert a property it did not have.

What makes it worth having anyway is that it is deterministic, it runs on every push, and it fails on
the exact class of line that actually shipped. Treat it as a regression guard with named coverage,
not as a proof.

⛔ WHY THE MASKING IS A STATE MACHINE AND NOT ORDERED REGEXES — A MEASURED BYPASS, NOT A PREFERENCE.

The first implementation blanked block comments, then line comments, then string literals. That let
this through with NO finding at all:

    diagnostics::log(level, tag, "http://device/" + std::string{password});

The `//` inside the URL literal was blanked as a line comment, taking `device/" + std::string{...})`
with it — so both the credential AND the call's closing parenthesis vanished.

Swapping the order does not fix it, it inverts it. With literals blanked first, a block comment
holding a single quote hides the CALL:

    /* he said " */ diagnostics::log(level, tag, "x" + std::string{password});

the literal regex spans from the quote inside the comment to the quote of "x", erasing
`diagnostics::log(`. Both orders were tried and both were measured; neither is safe, because whether
a `//`, a `/*` or a `"` opens anything depends on what is already open. That is a lexical state, so
_mask() below walks the text once and tracks it.

⬅️ IT MATCHES ARGUMENTS, NOT MESSAGES. A log call is parsed to its balanced closing parenthesis in the
MASKED text — code only, with literal and comment content replaced by spaces of equal length so every
offset and line number still refers to the real source. Presence, length and outcome spellings are
what the project uses instead and pass by construction.

⛔ THERE IS NO FILE-WIDE EXEMPTION, AND THERE MUST NEVER BE ONE. An earlier revision skipped
`setup_ap.cpp` entirely so that its device-generated setup SSID could be logged. That was unsafe:
adding a log of the SETUP PASSWORD — or of any other credential — anywhere in that file would have
bypassed the gate silently. The allowance now binds to ONE CALL and is consumed by it, and even that
call is rejected if it also carries a credential. See ALLOWANCE_MARKER below.

Usage:
    ./firmware/tools/check-credential-logs.py

Exits non-zero on the first finding, listing every one.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

# The logging entry points this project owns. diagnostics::log() reaches ESP_LOG*; diagnostics::
# timeline() reaches ESP_LOGI, as do this repository's file-local `timeline(...)` wrappers. Both are
# compiled into the release image.
#
# ⛔ NOT `log_label(...)`. That is a formatter that turns an enum into a fixed word and is the
# project's own alternative to printing a value — matching it would report the fix as the defect.
LOG_CALL = re.compile(r"\bdiagnostics::(?:log|timeline)\s*\(|(?<![\w:])timeline\s*\(")

# Values that may NEVER be logged, in any file, marked or not: passphrases, API keys, and the home
# network name.
#
# ⬅️ WHY DEREFERENCE-INTO-TEXT IS THE DISCRIMINATOR FOR SOME AND NOT OTHERS. `report.wifi_ssid` in
# server.cpp is a FieldOutcome — a validation verdict printed through log_label() as "ok" or
# "too-long" — while `submitted.wifi_ssid.view()` is the value itself. The member names are
# identical; what separates them is whether the expression yields characters. Passphrase-shaped
# identifiers get no such benefit of the doubt: a bare `password` in logged CODE is a finding.
#
# Each alternative names a real accessor or variable in this repository, so a finding always points
# at code rather than at prose.
CREDENTIAL_VALUE = re.compile(
    r"""
    \b g_ssid \b
  | \b g_setup_session \b
  | \b (?: wifi_ssid
        | wifi_password
        | printer_api_key
        | api_key
        )
      \s* \. \s* (?: view | data | c_str ) \s* \( \s* \)
  | \b credentials \s* \. \s* (?: ssid | password ) \b
  | \b submitted \s* \. \s* (?: wifi_ssid | wifi_password | printer_api_key ) \b
  | \b session \s* \. \s* password \b
  | \b session_ \s* \. \s* password \b
  | \b (?: password | passphrase | api_key | apikey | psk ) \b
    """,
    re.VERBOSE,
)

# The device's OWN generated setup network name. Not a user credential — §14.2 requires the panel to
# show it, and `PRD-Setup-<device suffix>` is this device's identity for the length of one
# provisioning session.
#
# ⛔ STILL NOT FREELY LOGGABLE. It is public device identity, but it is adjacent to the session
# password in the same struct and in the same locals, so an unmarked log of it is a finding too. Only
# a call bound to an ALLOWANCE_MARKER may print it, and only if that call carries nothing worse.
SESSION_IDENTITY_VALUE = re.compile(
    r"""
    \b session \s* \. \s* ssid \b
  | \b session_ \s* \. \s* ssid \b
  | \b ssid \s* \. \s* (?: view | data | c_str ) \s* \( \s* \)
  | \b ssid \b
    """,
    re.VERBOSE,
)

# Spellings that report a credential's PRESENCE, LENGTH or OUTCOME rather than its characters.
#
# ⬅️ APPLIED AS A SUFFIX TEST, NOT A WHOLE-CALL STRIP. Removing `yes_no(` from the call text would
# leave its argument behind and report it, which is how the first draft of this gate reported seven
# correct lines in server.cpp as defects.
PRESENCE_SUFFIX = re.compile(r"\s*\.\s*(?:empty|size|length)\s*\(\s*\)")

# The one thing that may permit a session-identity value, and it permits exactly one call.
#
# ⛔ BOUND AND CONSUMED, NOT MERELY NEARBY. An earlier revision allowed any session-identity call
# within three lines below the marker, which is not the same as call-scoped: two short log calls fit
# inside three lines, so one marker could have permitted both. Each marker now binds to THE NEXT
# logging call, claims it, and is spent. A second session-SSID log needs its own marker, and a marker
# that binds to nothing — no following call, a call another marker already claimed, or executable
# code in between — is itself reported rather than silently granting ambiguous permission.
#
# ⛔ AND IT DOES NOT OVERRIDE CREDENTIAL_VALUE. A bound call that also prints a passphrase is still a
# finding, which is the property a path-wide exemption could not give.
#
# Its uniqueness in the tree is asserted by firmware/test/tools/test_credential_log_scan.py, so the
# marker spreading shows up as a failing test rather than as quiet erosion.
ALLOWANCE_MARKER = "credential-log-allowance: device-generated setup SSID"

_IDENTIFIER_TAIL = re.compile(r"[0-9A-Za-z_]")


@dataclass(frozen=True)
class Finding:
    path: Path
    line: int
    call: str
    reason: str


@dataclass(frozen=True)
class Masked:
    """The source with every non-code region replaced by spaces, plus where the comments were."""

    code: str
    comments: tuple[tuple[int, int], ...]

    def comment_end_at(self, offset: int) -> int | None:
        """End of the comment containing `offset`, or None if it is not inside one."""
        for start, end in self.comments:
            if start <= offset < end:
                return end
        return None


def _mask(text: str) -> Masked:
    """Blank out string, character, raw-string and comment content in one left-to-right pass.

    ⛔ ONE PASS, BECAUSE WHAT A DELIMITER MEANS DEPENDS ON WHAT IS ALREADY OPEN. `//` inside a string
    literal is not a comment; `"` inside a comment does not open a string; `/*` inside either is
    text. Ordered regex substitution cannot express that, and both possible orders were measured to
    have a bypass — see the module docstring.

    Length is preserved exactly, and newlines are kept, so offsets and line numbers computed on the
    masked text still refer to the real source.

    ⬅️ WHAT IS DELIBERATELY NOT MODELLED, because this repository does not contain it and guessing
    would add failure modes rather than remove them: preprocessor conditionals (a credential inside
    `#if 0` is still reported, which is the safe direction), digraphs and trigraphs, and `<...>`
    include arguments, which contain no credentials.
    """

    out: list[str] = []
    comments: list[tuple[int, int]] = []

    index = 0
    length = len(text)

    def blank(start: int, end: int) -> None:
        out.append("".join("\n" if character == "\n" else " " for character in text[start:end]))

    while index < length:
        character = text[index]

        # --- comments ---------------------------------------------------------
        if character == "/" and index + 1 < length and text[index + 1] == "/":
            start = index
            while True:
                newline = text.find("\n", index)
                if newline < 0:
                    index = length
                    break
                # A line comment ending in a backslash continues onto the next line, exactly as the
                # compiler reads it. Rare, but treating the continuation as code would report a
                # credential the compiler never sees.
                if newline > 0 and text[newline - 1] == "\\":
                    index = newline + 1
                    continue
                index = newline
                break
            comments.append((start, index))
            blank(start, index)
            continue

        if character == "/" and index + 1 < length and text[index + 1] == "*":
            start = index
            close = text.find("*/", index + 2)
            index = length if close < 0 else close + 2
            comments.append((start, index))
            blank(start, index)
            continue

        # --- raw strings ------------------------------------------------------
        # R"delim( ... )delim" — the delimiter is at most 16 characters and excludes '(' and space.
        if character == "R" and index + 1 < length and text[index + 1] == '"':
            probe = index + 2
            limit = min(length, probe + 17)
            while probe < limit and text[probe] not in "( \t\n\\":
                probe += 1
            if probe < length and text[probe] == "(":
                terminator = ")" + text[index + 2 : probe] + '"'
                close = text.find(terminator, probe + 1)
                start = index
                index = length if close < 0 else close + len(terminator)
                blank(start, index)
                continue

        # --- ordinary string literals ----------------------------------------
        if character == '"':
            start = index
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == '"':
                    index += 1
                    break
                if text[index] == "\n":
                    # Unterminated: C++ forbids a raw newline inside an ordinary literal, so stop
                    # here rather than swallowing the rest of the file.
                    break
                index += 1
            blank(start, index)
            continue

        # --- character literals ----------------------------------------------
        # ⛔ NOT EVERY QUOTE IS A LITERAL. C++14 digit separators are written `2'000'000`, and this
        # repository uses them. A quote between two digits is a separator; anything else opens a
        # character literal, which keeps `L'x'` and `u8'x'` correct too.
        if character == "'":
            previous = text[index - 1] if index > 0 else ""
            following = text[index + 1] if index + 1 < length else ""
            if previous.isdigit() and following.isdigit():
                out.append(character)
                index += 1
                continue

            start = index
            index += 1
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == "'":
                    index += 1
                    break
                if text[index] == "\n":
                    break
                index += 1
            blank(start, index)
            continue

        out.append(character)
        index += 1

    code = "".join(out)
    assert len(code) == length, "the mask must preserve every byte offset"
    return Masked(code=code, comments=tuple(comments))


def _balanced_call(text: str, open_paren: int) -> str:
    """The call text from its opening parenthesis to the matching close."""
    depth = 0
    index = open_paren
    while index < len(text):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren : index + 1]
        index += 1
    return text[open_paren:]


def _unguarded(call_code: str, pattern: re.Pattern[str]) -> bool:
    """Whether `pattern` matches somewhere a presence/length/outcome suffix does not excuse."""
    for match in pattern.finditer(call_code):
        if PRESENCE_SUFFIX.match(call_code, match.end()) is None:
            return True
    return False


def _bind_allowances(text: str, masked: Masked, call_starts: Sequence[int]) -> tuple[set[int], list[tuple[int, str]]]:
    """Bind each marker to the next unclaimed logging call. Returns (allowed calls, orphan markers).

    ⛔ CONSUMED ONCE. A marker claims one call and is spent, so a second session-SSID log after it
    needs its own marker.

    ⬅️ "ONLY WHITESPACE OR COMMENTS IN BETWEEN" IS CHECKED ON THE MASKED TEXT, which is what makes
    the rule cheap and exact: comments are already spaces there, so the condition is simply that the
    slice between the marker's comment and the call holds nothing but whitespace.
    """

    allowed: set[int] = set()
    claimed: set[int] = set()
    orphans: list[tuple[int, str]] = []

    position = text.find(ALLOWANCE_MARKER)
    while position >= 0:
        line = text.count("\n", 0, position) + 1
        comment_end = masked.comment_end_at(position)

        if comment_end is None:
            orphans.append((line, "allowance marker is not inside a comment"))
        else:
            following = [start for start in call_starts if start >= comment_end]
            if not following:
                orphans.append((line, "allowance marker binds to no logging call"))
            elif following[0] in claimed:
                orphans.append(
                    (line, "allowance marker binds to a call another marker already claimed")
                )
            elif masked.code[comment_end : following[0]].strip() != "":
                orphans.append(
                    (line, "executable code sits between the allowance marker and the next log call")
                )
            else:
                claimed.add(following[0])
                allowed.add(following[0])

        position = text.find(ALLOWANCE_MARKER, position + len(ALLOWANCE_MARKER))

    return allowed, orphans


def scan_text(text: str) -> list[tuple[int, str, str]]:
    """Every offending log call — and every orphan marker — as (1-based line, source, reason).

    Pure, so the self-tests need no files on disk.
    """
    masked = _mask(text)
    code = masked.code

    calls: list[tuple[int, int]] = []
    for match in LOG_CALL.finditer(code):
        open_paren = code.find("(", match.start())
        if open_paren >= 0:
            calls.append((match.start(), open_paren))

    allowed, orphans = _bind_allowances(text, masked, [start for start, _ in calls])

    findings: list[tuple[int, str, str]] = [
        (line, ALLOWANCE_MARKER, reason) for line, reason in orphans
    ]

    for call_start, open_paren in calls:
        call_code = _balanced_call(code, open_paren)
        line = code.count("\n", 0, call_start) + 1
        shown = " ".join(_balanced_call(text, open_paren).split())[:160]

        # ⛔ CHECKED FIRST AND UNCONDITIONALLY. No marker excuses a passphrase, an API key or the
        # home network name.
        if _unguarded(call_code, CREDENTIAL_VALUE):
            findings.append((line, shown, "logs a credential value"))
            continue

        if _unguarded(call_code, SESSION_IDENTITY_VALUE) and call_start not in allowed:
            findings.append((line, shown, "logs a network name without an allowance marker"))

    findings.sort(key=lambda finding: finding[0])
    return findings


def scan_paths(paths: Iterable[Path]) -> list[Finding]:
    """Scan every path. NO FILE IS EXEMPT — that is the point of this function's shape."""
    findings: list[Finding] = []

    for path in sorted(paths):
        text = path.read_text(encoding="utf-8", errors="replace")
        findings.extend(Finding(path, line, call, reason) for line, call, reason in scan_text(text))

    return findings


def sources(roots: Iterable[Path]) -> list[Path]:
    found: list[Path] = []
    for root in roots:
        for suffix in ("*.cpp", "*.hpp", "*.c", "*.h"):
            found.extend(root.rglob(suffix))
    return found


def markers(paths: Iterable[Path]) -> list[tuple[Path, int]]:
    """Every allowance marker in the tree, so its uniqueness can be asserted."""
    found: list[tuple[Path, int]] = []
    for path in sorted(paths):
        for number, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1
        ):
            if ALLOWANCE_MARKER in line:
                found.append((path, number))
    return found


def main(argv: Sequence[str]) -> int:
    repo = Path(__file__).resolve().parent.parent.parent
    roots = [repo / "firmware" / "components", repo / "firmware" / "main"]

    missing = [root for root in roots if not root.is_dir()]
    if missing:
        for root in missing:
            print(f"check-credential-logs: {root} not found", file=sys.stderr)
        return 1

    scanned = sources(roots)
    findings = scan_paths(scanned)

    if findings:
        print("check-credential-logs: FAILED — §13.5 forbids credentials in logs", file=sys.stderr)
        for finding in findings:
            print(
                f"  {finding.path.relative_to(repo)}:{finding.line}  {finding.reason}\n"
                f"      {finding.call}",
                file=sys.stderr,
            )
        print(
            "\nReport presence or an outcome instead: yes_no(!value.empty()), log_label(...), "
            '"absent"/"present".',
            file=sys.stderr,
        )
        return 1

    found = markers(scanned)
    print(
        f"check-credential-logs: OK — {len(scanned)} files scanned, no file exempt, "
        f"{len(found)} bound allowance(s)"
    )
    for path, line in found:
        print(f"  allowance {path.relative_to(repo)}:{line}")
    print(
        "  boundary: direct-source regression guard over known logging shapes; "
        "not alias or data-flow analysis"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
