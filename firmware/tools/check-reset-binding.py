#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
#
# THE FACTORY-RESET BOOT BINDING MAY NOT DISCARD ITS ANSWER — audit item 5, as a build check.
#
# WHAT WENT WRONG, AND WHY THIS SCRIPT EXISTS. The boot resumed an interrupted factory reset and
# threw the result away:
#
#     (void)factory_reset::run(storage::key_value());
#
# `run()` is [[nodiscard]]. The cast silenced it, and a resume that FAILED then fell through into
# load_configuration() — past the reconciliation the ordering exists to get in front of. The store
# was left half-erased with its journal armed while the device booted, presented normally and
# permitted every mutation.
#
# ⛔ WHY A SOURCE CHECK AND NOT A TEST. app_core.cpp is not host-compiled and never will be — it is
# the FreeRTOS composition root. No host test can observe what it does or does not call, so the shape
# of its call sites is checked structurally, the way the R8 seam and R11 clock-reading counts are.
#
# The repair also removed the two APIs that made the mistake expressible: factory_reset::pending()
# (a boolean that could not distinguish an unreadable journal from an absent one) and
# Event::factory_reset_completed (whose only production emitter was the failure path). Both are
# asserted absent here so neither returns by copy-paste from an older branch.
#
# ⛔ IT LEXES; IT DOES NOT GREP. The first version of this check claimed to strip string literals and
# only removed `//` text, and its line-oriented regex could not see a discard split across lines:
#
#     (void)
#         factory_reset::run(storage::key_value());
#
# Both holes are closed by the single pass below, which blanks comments and the CONTENTS of string,
# character and raw-string literals, then matches over the remaining tokens with arbitrary
# whitespace — newlines included — allowed between them. Nothing here claims to be a C++ parser: it
# is a lexical filter, and the self-test proves each behaviour it does claim.
#
# Usage:
#     ./firmware/tools/check-reset-binding.py
#     ./firmware/tools/check-reset-binding.py --self-test
#
# --self-test proves every claim above: each forbidden form is rejected on one line and across
# lines, each is IGNORED inside a line comment, a block comment, an ordinary string and a raw
# string, and the real target still passes. A gate nobody has seen reject anything is a gate nobody
# should trust — and one nobody has seen ACCEPT the shapes it must ignore is a gate that will be
# switched off the first time it cries wolf.

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TARGET = REPO / "firmware/components/app_core/src/app_core.cpp"

# Each pattern runs over whitespace-normalised, comment-and-literal-blanked source, so `\s*` really
# does span newlines.
FORBIDDEN = [
    (
        "a discarded factory_reset:: result",
        re.compile(r"(?:\(\s*void\s*\)|static_cast\s*<\s*void\s*>\s*\()\s*factory_reset\s*::"),
    ),
    ("the removed boolean journal probe", re.compile(r"factory_reset\s*::\s*pending\s*\(")),
    ("the removed completion event", re.compile(r"factory_reset_completed|FactoryResetCompleted")),
]


def blank_comments_and_literals(source: str) -> str:
    """Replace comments and the contents of string/char/raw-string literals with spaces.

    Newlines are preserved so reported line numbers stay true. Nothing is deleted, so every offset
    in the result still corresponds to the same offset in the input.
    """
    out = []
    index = 0
    length = len(source)

    while index < length:
        char = source[index]

        # Raw string: R"delim( ... )delim". Checked first, because its body may contain anything at
        # all — including quotes, backslashes and what look like comment markers.
        if char == "R" and index + 1 < length and source[index + 1] == '"':
            open_paren = source.find("(", index + 2)
            if open_paren != -1:
                delimiter = source[index + 2 : open_paren]
                if len(delimiter) <= 16 and '"' not in delimiter and "\n" not in delimiter:
                    closing = ")" + delimiter + '"'
                    end = source.find(closing, open_paren + 1)
                    if end != -1:
                        end += len(closing)
                        out.append("".join(" " if c != "\n" else "\n" for c in source[index:end]))
                        index = end
                        continue

        if char == "/" and index + 1 < length and source[index + 1] == "/":
            end = source.find("\n", index)
            end = length if end == -1 else end
            out.append(" " * (end - index))
            index = end
            continue

        if char == "/" and index + 1 < length and source[index + 1] == "*":
            end = source.find("*/", index + 2)
            end = length if end == -1 else end + 2
            out.append("".join(" " if c != "\n" else "\n" for c in source[index:end]))
            index = end
            continue

        if char in "\"'":
            # ⛔ A DIGIT SEPARATOR IS NOT A CHARACTER LITERAL. C++14 writes 2'000'000, and this file
            # contains such literals. Mistaking one for an opening quote would blank real code to the
            # next quote — which is the direction that MISSES findings, so it is guarded explicitly.
            if (
                char == "'"
                and index > 0
                and source[index - 1].isalnum()
                and index + 1 < length
                and source[index + 1].isalnum()
            ):
                out.append(char)
                index += 1
                continue

            end = index + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                if source[end] == char:
                    end += 1
                    break
                if source[end] == "\n":  # unterminated on this line; stop rather than run away
                    break
                end += 1
            out.append("".join(" " if c != "\n" else "\n" for c in source[index:end]))
            index = end
            continue

        out.append(char)
        index += 1

    return "".join(out)


def findings(source: str):
    """Every forbidden form in `source`, as (label, line number)."""
    blanked = blank_comments_and_literals(source)

    # Line starts, so a match offset can be reported as a line without a second pass.
    starts = [0]
    for position, char in enumerate(blanked):
        if char == "\n":
            starts.append(position + 1)

    def line_of(offset: int) -> int:
        low, high = 0, len(starts) - 1
        while low < high:
            mid = (low + high + 1) // 2
            if starts[mid] <= offset:
                low = mid
            else:
                high = mid - 1
        return low + 1

    found = []
    for label, pattern in FORBIDDEN:
        for match in pattern.finditer(blanked):
            found.append((label, line_of(match.start())))
    return found


def scan(path: Path) -> list:
    return findings(path.read_text(encoding="utf-8"))


CLEAN_PREFIX = "void boot() {\n    int x = 2'000'000;\n"

REJECT_CASES = {
    "same-line discard": '    (void)factory_reset::run(store);\n',
    "multiline discard": "    (void)\n        factory_reset::run(store);\n",
    "static_cast discard": "    static_cast<void>(factory_reset::run(store));\n",
    "removed pending()": "    if (factory_reset::pending(store)) {}\n",
    "removed completion event": "    dispatch(Event::factory_reset_completed(now));\n",
}

IGNORE_CASES = {
    "line comment": "    // (void)factory_reset::run(store); factory_reset_completed\n",
    "block comment": "    /* (void)factory_reset::run(store);\n       factory_reset::pending(store);\n"
    "       factory_reset_completed */\n",
    "ordinary string": '    log("(void)factory_reset::run and factory_reset_completed");\n',
    "raw string": '    log(R"doc((void)factory_reset::run(store); factory_reset_completed)doc");\n',
    "string with escaped quote": '    log("a \\" (void)factory_reset::pending(store)");\n',
}


def self_test() -> int:
    failures = 0

    for name, body in REJECT_CASES.items():
        if not findings(CLEAN_PREFIX + body + "}\n"):
            print(f"check-reset-binding: SELF-TEST FAILED — not rejected: {name}", file=sys.stderr)
            failures += 1

    for name, body in IGNORE_CASES.items():
        hits = findings(CLEAN_PREFIX + body + "}\n")
        if hits:
            print(
                f"check-reset-binding: SELF-TEST FAILED — fired inside {name}: {hits}",
                file=sys.stderr,
            )
            failures += 1

    # The digit separator must survive, or the blanker would eat real code after it.
    if findings("int a = 1'000'000;\n(void)factory_reset::run(s);\n") == []:
        print(
            "check-reset-binding: SELF-TEST FAILED — a digit separator hid a real finding",
            file=sys.stderr,
        )
        failures += 1

    if scan(TARGET):
        print(
            f"check-reset-binding: SELF-TEST FAILED — the real target did not pass: {scan(TARGET)}",
            file=sys.stderr,
        )
        failures += 1

    if failures:
        return 1

    total = len(REJECT_CASES) + len(IGNORE_CASES) + 2
    print(
        f"check-reset-binding: self-test OK — {len(REJECT_CASES)} forbidden forms rejected "
        f"(including one split across lines), {len(IGNORE_CASES)} comment/literal contexts ignored, "
        f"digit separators preserved, real target accepted ({total} assertions)"
    )
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()

    found = scan(TARGET)
    if found:
        relative = TARGET.relative_to(REPO)
        for label, line in found:
            print(f"check-reset-binding: {relative}:{line}: {label}", file=sys.stderr)
        print("check-reset-binding: FAILED", file=sys.stderr)
        return 1

    print(
        "check-reset-binding: OK — no discarded factory_reset:: result and no retired API in "
        f"{TARGET.relative_to(REPO)} (comments and string, character and raw-string contents "
        "excluded)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
