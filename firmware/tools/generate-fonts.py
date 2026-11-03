#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Generate the LVGL font sources required by Production UI Specification §4.

Six faces, one per §4 text role, derived from the Inter weights vendored under
assets/fonts/inter/. Tabular figures are applied at conversion time, which §4's
Numeric Rules require ("Use tabular figures. Do not use proportional figures.").

    Inter-<Weight>.ttf
        ↓  freeze `tnum` into the character map          (fontTools)
    <role>.tnum.ttf                                      (intermediate, discarded)
        ↓  rasterise the subset at the token size        (lv_font_conv)
    firmware/components/ui/src/fonts/prd_font_<role>.c   (COMMITTED)

WHY FREEZING IS NECESSARY. lv_font_conv maps Unicode codepoints straight to
glyphs through the font's `cmap`; it applies no OpenType feature. Asking it for
Inter's digits therefore yields Inter's *proportional* digits, whose `1` is
narrower than its `8` — so a percentage counting 18 → 19 would shift the
characters around it. Inter implements `tnum` as a plain GSUB single substitution
(`zero` → `zero.tf`, and so on), so freezing it is exactly a `cmap` remap: point
each affected codepoint at the tabular variant. No outline is altered.

The whole `tnum` mapping is frozen, not only the digits. It also covers the
space, hyphen, comma, period, colon and tilde — every character that appears
between or beside numerals in a telemetry value. Freezing the digits alone would
leave `1:42` stable in its digits and unstable at its colon.

WHY THE OUTPUT IS COMMITTED. An ordinary build and CI then need neither Node nor
fontTools nor network access, and the bytes in the image are the bytes that were
validated on the bench. Bootstrap Spec §16.8's reproducibility requirement is met
by the files themselves rather than by two toolchains agreeing.

Dependency versions are pinned below and verified before anything is written, so
a regenerated file cannot quietly differ from the committed one because a
converter moved underneath it.

Usage, from anywhere in the repository:

    ./firmware/tools/generate-fonts.py            # regenerate all six faces
    ./firmware/tools/generate-fonts.py --check    # verify committed == generated
    ./firmware/tools/generate-fonts.py --list     # print the face table and exit

Requires, and only when generating:

    python3 -m venv <env> && <env>/bin/pip install 'fonttools==4.63.0'
    npm install lv_font_conv@1.5.3
"""

from __future__ import annotations

import argparse
import filecmp
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# --- Pinned tooling --------------------------------------------------------
#
# Recorded in every generated file and verified before generation. An unpinned
# converter is a reproducibility hole: the same inputs would produce different
# bitmaps and the committed sources would drift with whatever happened to be
# installed.

FONTTOOLS_VERSION = "4.63.0"
LV_FONT_CONV_VERSION = "1.5.3"

# Bits per pixel for the glyph bitmaps. 4 gives 16 anti-aliasing levels, which
# Production UI §11 requires for typography; 8 doubles the flash cost for a
# difference invisible at this size and viewing distance.
BPP = 4

# --- Subsets ---------------------------------------------------------------
#
# Printable ASCII plus the degree sign. The degree sign is needed because every
# temperature carries it (§8 Temperature Row); everything else a Phase C scene
# draws is ASCII.
#
# Non-ASCII in printer-supplied job names is deliberately NOT covered. That needs
# a missing-glyph fallback policy, and the data that would motivate one arrives
# with PrusaLink in Milestone 4. Guessing a range now would be flash spent on a
# guess.
ASCII_RANGE = "0x20-0x7E"
DEGREE_SIGN = "0x00B0"

TEXT_SUBSET = f"{ASCII_RANGE},{DEGREE_SIGN}"

# The 36 px face draws one thing: the primary value (§4 Text Roles). Digits, the
# percent sign, and the separators a numeric value can contain. At 36 px a full
# ASCII set would be roughly six times the size for glyphs nothing draws.
DISPLAY_SUBSET = "0x20,0x25,0x2E,0x30-0x39,0x3A"

# --- Faces -----------------------------------------------------------------
#
# One per Production UI §4 text role. `role` is the C symbol suffix and the token
# name in ui/src/theme/tokens.hpp; the two must not drift, and a host test
# asserts the token table names exactly these.

FACES = (
    # role,      size, weight file,           §4 role name,     subset
    #
    # hero_72 is NOT in Production UI §4's table. §4 gives the primary value
    # `font.size.display` = 36 px, which produces a numeral at less than half the visual
    # weight of the accepted UI Design Decision 001 artwork — measured cap height 26 px
    # against the artwork's 58-62 px on a 240 px display. Owner instruction 2026-08-06
    # directs matching the artwork rather than the token, so the primary value is 72 px
    # and §4's 36 px is retained as the size of its unit, which reproduces the artwork's
    # unit-to-numeral ratio.
    #
    # 64 px AND NOT LARGER, on measurement rather than on the font tables. Inter's tabular
    # digit advance is 0.653 em — measured on the generated face at start-up, not the 0.594
    # the glyph metrics suggested. At 72 px the three-digit field at 100 % plus its unit
    # comes to 175 px against a 175 px chord inside the ring: no clearance at all. 64 px
    # leaves 17 px, and still takes the numeral's cap height from 26 px to 49 px, which is
    # 20 % of the display height against the artwork's 24-26 %.
    ("hero_68", 68, "Inter-Bold.ttf", "Primary value", DISPLAY_SUBSET),
    ("display_36", 36, "Inter-Bold.ttf", "Primary unit", DISPLAY_SUBSET),
    ("title_20", 20, "Inter-SemiBold.ttf", "State title", TEXT_SUBSET),
    ("outcome_28", 28, "Inter-SemiBold.ttf", "Outcome title", TEXT_SUBSET),
    ("section_16", 16, "Inter-Medium.ttf", "Section", TEXT_SUBSET),
    ("body_14", 14, "Inter-Regular.ttf", "Body", TEXT_SUBSET),
    ("caption_12", 12, "Inter-Medium.ttf", "Label", TEXT_SUBSET),
    ("micro_10", 10, "Inter-Regular.ttf", "Metadata", TEXT_SUBSET),
) + tuple(
    (f"motion_value_{size}", size, "Inter-Bold.ttf", "Owner-reviewed motion value", DISPLAY_SUBSET)
    for size in range(44, 68)
) + tuple(
    (f"motion_unit_{size}", size, "Inter-Bold.ttf", "Owner-reviewed motion unit", "0x25")
    for size in range(23, 36)
) + tuple(
    (f"motion_title_{size}", size, "Inter-SemiBold.ttf", "Owner-reviewed motion title", TEXT_SUBSET)
    for size in range(21, 28)
)

REPO_ROOT = Path(__file__).resolve().parents[2]
FONT_ASSETS = REPO_ROOT / "assets" / "fonts" / "inter"
OUTPUT_DIR = REPO_ROOT / "firmware" / "components" / "ui" / "src" / "fonts"


def fail(message: str) -> None:
    print(f"generate-fonts: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


# --- Tooling discovery -----------------------------------------------------


def find_lv_font_conv() -> list[str]:
    """Locate lv_font_conv and verify its version against the pin."""
    candidates: list[list[str]] = []

    local = REPO_ROOT / "node_modules" / ".bin" / "lv_font_conv"
    if local.is_file():
        candidates.append([str(local)])

    found = shutil.which("lv_font_conv")
    if found:
        candidates.append([found])

    if shutil.which("npx"):
        candidates.append(["npx", "--yes", f"lv_font_conv@{LV_FONT_CONV_VERSION}"])

    for command in candidates:
        try:
            result = subprocess.run(
                command + ["--version"], capture_output=True, text=True, check=True
            )
        except (OSError, subprocess.CalledProcessError):
            continue

        version = result.stdout.strip()
        if version != LV_FONT_CONV_VERSION:
            fail(
                f"lv_font_conv {version} found, {LV_FONT_CONV_VERSION} pinned. "
                f"Install the pinned version rather than regenerating with another: "
                f"npm install lv_font_conv@{LV_FONT_CONV_VERSION}"
            )
        return command

    fail(
        "lv_font_conv not found. It is needed only to regenerate fonts; the "
        "generated sources are committed, so an ordinary build does not need it. "
        f"Install with: npm install lv_font_conv@{LV_FONT_CONV_VERSION}"
    )
    raise AssertionError("unreachable")


def import_fonttools():
    try:
        import fontTools  # noqa: PLC0415
        from fontTools.ttLib import TTFont  # noqa: PLC0415
    except ImportError:
        fail(
            "fontTools not importable. It is needed only to regenerate fonts. "
            f"Install with: pip install 'fonttools=={FONTTOOLS_VERSION}'"
        )
        raise AssertionError("unreachable")

    if fontTools.version != FONTTOOLS_VERSION:
        fail(
            f"fontTools {fontTools.version} found, {FONTTOOLS_VERSION} pinned. "
            f"Install with: pip install 'fonttools=={FONTTOOLS_VERSION}'"
        )

    return TTFont


# --- tnum freezing ---------------------------------------------------------


def tnum_mapping(font) -> dict[str, str]:
    """The `tnum` feature's substitution map, read from the font itself.

    Read rather than hardcoded: if a future Inter changes which characters the
    feature covers, this follows it instead of silently freezing a stale list.
    """
    if "GSUB" not in font:
        fail("font has no GSUB table, so it declares no tnum feature")

    table = font["GSUB"].table
    lookup_indices: list[int] = []
    for record in table.FeatureList.FeatureRecord:
        if record.FeatureTag == "tnum":
            lookup_indices.extend(record.Feature.LookupListIndex)

    if not lookup_indices:
        fail("font declares no tnum feature; tabular figures are not available")

    mapping: dict[str, str] = {}
    for index in lookup_indices:
        lookup = table.LookupList.Lookup[index]
        if lookup.LookupType != 1:
            fail(
                f"tnum uses GSUB lookup type {lookup.LookupType}; only type 1 "
                f"(single substitution) can be frozen by a cmap remap"
            )
        for subtable in lookup.SubTable:
            mapping.update(getattr(subtable, "mapping", {}))

    return mapping


def freeze_tnum(source: Path, destination: Path, TTFont) -> int:
    """Repoint the character map at the tabular variants. Returns the count."""
    font = TTFont(source)
    mapping = tnum_mapping(font)

    remapped = 0
    glyph_order = set(font.getGlyphOrder())

    for table in font["cmap"].tables:
        for codepoint, glyph in list(table.cmap.items()):
            replacement = mapping.get(glyph)
            if replacement is None:
                continue
            if replacement not in glyph_order:
                fail(f"tnum maps {glyph} to missing glyph {replacement}")
            table.cmap[codepoint] = replacement
            remapped += 1

    font.save(destination)
    font.close()

    # Counted across every cmap subtable, so a font with both a BMP and a full
    # Unicode subtable reports each. Reported rather than asserted against a
    # fixed number: the figure is evidence, and a change in it is a signal to
    # look, not necessarily a fault.
    return remapped


# --- Conversion ------------------------------------------------------------


def convert(
    lv_font_conv: list[str], frozen: Path, size: int, subset: str, symbol: str, output: Path
) -> None:
    command = lv_font_conv + [
        "--font",
        str(frozen),
        "--size",
        str(size),
        "--range",
        subset,
        "--bpp",
        str(BPP),
        "--no-compress",
        "--format",
        "lvgl",
        "--lv-include",
        "lvgl.h",
        "--force-fast-kern-format",
        "--no-prefilter",
        "-o",
        str(output),
    ]

    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"lv_font_conv failed for {symbol}:\n{result.stderr.strip()}")


def rewrite_header(path: Path, symbol: str, banner: str) -> None:
    """Replace lv_font_conv's generated preamble with the project's own.

    The converter emits its own comment naming the source file and options. It is
    replaced rather than appended to because it records the path of a temporary
    intermediate that does not exist in the repository, which would read as a
    missing file to anyone who went looking for it.
    """
    text = path.read_text(encoding="utf-8")

    # Splice immediately after the converter's leading block comment, which is the only
    # thing being replaced. Everything below it — the LVGL include block, the include
    # guard, the bitmaps — is kept verbatim.
    #
    # Deliberately NOT anchored on the include guard or on a particular #ifdef spelling.
    # An earlier version anchored on the guard, which sits *below* the include block, so
    # the include was silently dropped and every file failed to compile with LVGL's
    # macros undeclared. The lvgl.h assertion below is what turns that class of mistake
    # into an immediate failure rather than a confusing compile error.
    end_of_comment = text.find("*/")
    if end_of_comment < 0:
        fail(f"unrecognised lv_font_conv output for {symbol}; no leading comment found")

    result = banner + text[end_of_comment + len("*/") :].lstrip("\n")
    # Keep the seven historical faces byte-identical; new additions have one final LF.
    # The converter's extra blank EOF otherwise fails the new-file whitespace gate.
    if symbol.startswith("prd_font_motion_") or symbol == "prd_font_outcome_28":
        result = result.rstrip("\n") + "\n"

    if "lvgl.h" not in result:
        fail(f"{symbol}: the lvgl.h include was lost while replacing the header")

    path.write_text(result, encoding="utf-8")


def banner_for(
    symbol: str, role: str, size: int, weight_file: str, weight_sha: str, subset: str, remapped: int
) -> str:
    authority = ("Owner-reviewed operational motion (2026-09-06)"
                 if "Owner-reviewed" in role or role == "Outcome title"
                 else "Production UI Specification §4 text role")
    return f"""// SPDX-FileCopyrightText: Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)
// SPDX-License-Identifier: OFL-1.1-no-RFN
//
// GENERATED FILE — DO NOT EDIT.
//
// Regenerate with:  ./firmware/tools/generate-fonts.py
//
// {authority}: {role}
//   font.size  {size} px
//   weight     {weight_file}
//
// Derived from Inter 4.1 (SIL Open Font License 1.1), vendored at
// assets/fonts/inter/ with its licence, copyright notice and full provenance.
// Inter 4.1 declares no Reserved Font Name; see assets/fonts/inter/PROVENANCE.md.
//
// This is a MODIFIED version of Inter within the meaning of the OFL: the `tnum`
// feature has been frozen into the character map ({remapped} codepoints repointed at
// their tabular variants) and the outlines rasterised to fixed-size bitmaps. It is
// named for the production role it serves rather than presented as the official
// Inter font.
//
//   source sha256   {weight_sha}
//   codepoints      {subset}
//   bpp             {BPP}
//   fontTools       {FONTTOOLS_VERSION}
//   lv_font_conv    {LV_FONT_CONV_VERSION}
//
// TABULAR FIGURES ARE NOT ASSERTED HERE. Every digit's advance width is compared
// at renderer start-up and reported; see prd::ui::fonts::verify() in
// ui/src/fonts/fonts.cpp. A comment claiming tabular figures would be a claim; a
// start-up check is a measurement.

"""


# --- Entry point -----------------------------------------------------------


def generate(output_dir: Path) -> list[tuple[str, Path]]:
    TTFont = import_fonttools()
    lv_font_conv = find_lv_font_conv()

    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[tuple[str, Path]] = []

    with tempfile.TemporaryDirectory() as workspace:
        frozen_cache: dict[str, tuple[Path, int]] = {}

        for role, size, weight_file, role_name, subset in FACES:
            source = FONT_ASSETS / weight_file
            if not source.is_file():
                fail(f"vendored font missing: {source}")

            if weight_file not in frozen_cache:
                frozen = Path(workspace) / f"{Path(weight_file).stem}.tnum.ttf"
                count = freeze_tnum(source, frozen, TTFont)
                frozen_cache[weight_file] = (frozen, count)
                print(f"  froze tnum in {weight_file}: {count} codepoints remapped")

            frozen, remapped = frozen_cache[weight_file]
            symbol = f"prd_font_{role}"
            output = output_dir / f"{symbol}.c"

            convert(lv_font_conv, frozen, size, subset, symbol, output)
            rewrite_header(
                output,
                symbol,
                banner_for(
                    symbol, role_name, size, weight_file, sha256(source), subset, remapped
                ),
            )

            print(f"  {symbol}.c  {output.stat().st_size:>7,} bytes  ({role_name}, {size} px)")
            written.append((symbol, output))

    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--check",
        action="store_true",
        help="regenerate into a temporary directory and diff against the committed sources",
    )
    parser.add_argument("--list", action="store_true", help="print the face table and exit")
    arguments = parser.parse_args()

    if arguments.list:
        print(json.dumps(
            [
                {"role": role, "size": size, "weight": weight, "ui_role": name, "subset": subset}
                for role, size, weight, name, subset in FACES
            ],
            indent=2,
        ))
        return 0

    if arguments.check:
        with tempfile.TemporaryDirectory() as workspace:
            candidate = Path(workspace) / "fonts"
            print(f"generate-fonts: regenerating into {candidate} for comparison")
            written = generate(candidate)

            differences = []
            for symbol, path in written:
                committed = OUTPUT_DIR / path.name
                if not committed.is_file():
                    differences.append(f"{path.name}: not committed")
                elif not filecmp.cmp(committed, path, shallow=False):
                    differences.append(f"{path.name}: differs from committed")

            if differences:
                for line in differences:
                    print(f"generate-fonts: {line}", file=sys.stderr)
                return 1

            print(f"generate-fonts: {len(written)} faces reproduce the committed sources exactly")
            return 0

    print(f"generate-fonts: writing to {OUTPUT_DIR}")
    written = generate(OUTPUT_DIR)
    total = sum(path.stat().st_size for _, path in written)
    print(f"generate-fonts: {len(written)} faces, {total:,} bytes of C source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
