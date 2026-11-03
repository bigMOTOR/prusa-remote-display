# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only

"""Required newcomer routes, not a verdict on tone, usability or public readiness.

Discovered by the existing test-tools.sh Linux CI gate. No network or hardware.
The prepared Printables listing keeps its own checks; this is not a live user test.
"""

import re
import shlex
import unittest
from pathlib import Path
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[3]
GITHUB_PATH = "/bigMOTOR/prusa-remote-display"
REQUIRED = {
    "README.md": {
        "docs/get-started.md", "docs/parts-and-cost.md",
        "enclosure/printing.md", "enclosure/assembly.md",
        "docs/installation-and-recovery.md", "CONTRIBUTING.md",
        "docs/development/architecture.md", "docs/development/ui-rendering.md",
    },
    "docs/get-started.md": {
        "README.md", "docs/parts-and-cost.md", "enclosure/README.md",
        "enclosure/printing.md", "enclosure/assembly.md",
        "docs/installation-and-recovery.md", "CONTRIBUTING.md",
    },
    "docs/printables-draft.md": {
        "README.md", "docs/get-started.md", "docs/parts-and-cost.md", "enclosure/printing.md",
        "enclosure/assembly.md", "docs/installation-and-recovery.md", "CONTRIBUTING.md",
    },
    "desktop-installer/README.md": {"README.md", "docs/get-started.md", "CONTRIBUTING.md"},
    "installer/README.md": {"README.md", "docs/get-started.md", "desktop-installer/README.md"},
    "CONTRIBUTING.md": {"README.md", "docs/get-started.md", "desktop-installer/README.md",
                        "docs/development/architecture.md", "docs/development/ui-rendering.md"},
    "docs/development/architecture.md": {"docs/get-started.md", "CONTRIBUTING.md",
                                         "docs/development/ui-rendering.md"},
    "docs/development/ui-rendering.md": {"README.md", "CONTRIBUTING.md",
                                         "docs/development/architecture.md"},
}


def listing_body(text):
    """Accept finalized copy and retain the old draft's publication boundary."""
    marker = "## Listing text — preview\n"
    if marker in text:
        text = text.split(marker, 1)[1]
    return text.split("## Editor notes — not for publication\n", 1)[0]


def visible_markdown(text):
    """Strip hidden comments and code examples before checking visible routes."""
    visible = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    lines = []
    fence = None
    for line in visible.splitlines():
        marker = re.match(r"^ {0,3}(`{3,}|~{3,})(.*)$", line)
        if fence is not None:
            if marker and marker[1][0] == fence[0] and len(marker[1]) >= len(fence) and not marker[2].strip():
                fence = None
            continue
        if marker:
            fence = marker[1]
        elif not line.startswith(("    ", "\t")):
            lines.append(line)
    visible = "\n".join(lines)
    return re.sub(r"`[^`\n]*`", "", visible)


def local_targets(document, text):
    """Resolve this small set of inline Markdown links, excluding hidden examples.

    Not a general Markdown parser: required routes must use visible inline links.
    Four-space-indented list content is not checked; keep required routes unindented.
    Canonical GitHub main links map to local files; other external links and
    anchor-only links are outside this local-file contract. Remote accessibility,
    anchors and the publication state are not certified by this offline test.
    """
    visible = visible_markdown(text)
    targets = set()
    for href in re.findall(r"(?<!!)\[[^\]\n]+\]\(([^\s)]+)\)", visible):
        parsed = urlsplit(href)
        if not parsed.path:
            continue
        if parsed.scheme or parsed.netloc:
            if parsed.scheme != "https" or parsed.netloc != "github.com":
                continue
            if parsed.path.rstrip("/") == GITHUB_PATH:
                target = ROOT / "README.md"
            elif parsed.path.startswith(GITHUB_PATH + "/blob/main/"):
                target = ROOT / unquote(parsed.path[len(GITHUB_PATH + "/blob/main/"):])
            else:
                continue
        else:
            target = (ROOT / document).parent / unquote(parsed.path)
        try:
            targets.add(target.resolve().relative_to(ROOT.resolve()).as_posix())
        except ValueError:
            raise AssertionError(f"{document}: link escapes repository: {href}") from None
    return targets


def check_routes(document, text):
    all_targets = local_targets(document, text)
    if document == "docs/printables-draft.md":
        text = listing_body(text)
    targets = local_targets(document, text)
    missing = REQUIRED[document] - targets
    if missing:
        raise AssertionError(f"{document}: missing visible routes: {', '.join(sorted(missing))}")
    for target in sorted(all_targets):
        if not (ROOT / target).exists():
            raise AssertionError(f"{document}: missing local target: {target}")


def check_flash_commands(text):
    commands = re.findall(r"^idf\.py .+$", text, re.M)
    if len(commands) != 3:
        raise AssertionError("expected flash, monitor and combined commands")
    for command in commands:
        words = shlex.split(command)
        if "-C" not in words or "-B" not in words:
            raise AssertionError("idf.py must select firmware project and build directory")
        project = ROOT / words[words.index("-C") + 1]
        build = ROOT / words[words.index("-B") + 1]
        if project.resolve() != (ROOT / "firmware").resolve() or not (project / "CMakeLists.txt").is_file():
            raise AssertionError("wrong ESP-IDF project directory")
        if build.resolve() != (ROOT / "firmware/build/hardware-validation").resolve():
            raise AssertionError("wrong hardware-validation build directory")


def check_newcomer_contract(document, text):
    """Check direct routes, the browser sentence and the listing's release route.

    Prose, other external URLs and routes through other documents are outside
    this guard's contract; it is not a recursive Mac/preview-content audit.
    """
    if document == "docs/printables-draft.md":
        text = listing_body(text)
    if document in ("README.md", "docs/get-started.md", "docs/printables-draft.md"):
        unavailable = local_targets(document, text) & {
            "desktop-installer/README.md", "installer/README.md",
            "docs/printables-draft.md",
        }
        if unavailable:
            raise AssertionError(f"{document}: unavailable direct routes: {', '.join(sorted(unavailable))}")
    visible = visible_markdown(text)
    if "Use Chrome or Edge (Safari and Firefox are not supported)." not in visible:
        raise AssertionError(f"{document}: missing required browser sentence")
    if document == "docs/printables-draft.md":
        release_links = [urlsplit(href) for href in re.findall(
            r"(?<!!)\[[^\]\n]+\]\(([^\s)]+)\)", visible)]
        if not any(link.scheme == "https" and link.netloc == "github.com" and
                   (link.path == GITHUB_PATH + "/releases" or
                    link.path.startswith(GITHUB_PATH + "/releases/tag/"))
                   for link in release_links):
            raise AssertionError(f"{document}: missing official release route")


class PublicNavigationTests(unittest.TestCase):
    def test_live_newcomer_routes_and_local_files(self):
        for document in REQUIRED:
            with self.subTest(document=document):
                check_routes(document, (ROOT / document).read_text(encoding="utf-8"))

    def test_removed_build_guide_fails(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        changed, count = re.subn(
            r"(?<!!)\[[^\]\n]+\]\(docs/get-started\.md\)", "removed", text
        )
        self.assertGreater(count, 0)
        with self.assertRaisesRegex(AssertionError, "missing visible routes: docs/get-started.md"):
            check_routes("README.md", changed)

    def test_newcomer_direct_routes_and_browser_sentence(self):
        for document in ("README.md", "docs/get-started.md", "docs/installation-and-recovery.md",
                         "docs/printables-draft.md"):
            with self.subTest(document=document):
                check_newcomer_contract(document, (ROOT / document).read_text(encoding="utf-8"))

    def test_added_preview_route_fails(self):
        for document in ("README.md", "docs/get-started.md", "docs/printables-draft.md"):
            text = (ROOT / document).read_text(encoding="utf-8")
            prefix = "../" if document.startswith("docs/") else ""
            for target in ("desktop-installer/README.md", "installer/README.md"):
                with self.subTest(document=document, target=target):
                    with self.assertRaisesRegex(AssertionError, "unavailable direct routes"):
                        check_newcomer_contract(document, text + f"\n[App]({prefix}{target})\n")

    def test_removed_browser_sentence_fails(self):
        for document in ("README.md", "docs/get-started.md", "docs/installation-and-recovery.md",
                         "docs/printables-draft.md"):
            with self.subTest(document=document):
                text = (ROOT / document).read_text(encoding="utf-8")
                changed = text.replace("Use Chrome or Edge (Safari and Firefox are not supported).", "")
                with self.assertRaisesRegex(AssertionError, "missing required browser sentence"):
                    check_newcomer_contract(document, changed)

    def test_hidden_links_do_not_count(self):
        for wrapper in ("<!-- {} -->", "```markdown\n{}\n```", "`{}`"):
            with self.subTest(wrapper=wrapper):
                link = "[App](desktop-installer/README.md)"
                self.assertNotIn("desktop-installer/README.md", local_targets("README.md", wrapper.format(link)))

    def test_broken_file_fails(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "missing local target: docs/no-such-guide.md"):
            check_routes("README.md", text + "\n[Missing](docs/no-such-guide.md)\n")

    def test_indented_example_does_not_count(self):
        for indent in ("    ", "\t"):
            with self.subTest(indent=indent):
                self.assertEqual(local_targets("README.md", indent + "[App](desktop-installer/README.md)"), set())

    def test_unclosed_fence_does_not_count(self):
        self.assertEqual(local_targets("README.md", "~~~markdown\n[App](desktop-installer/README.md)"), set())

    def test_relative_routes_are_document_local(self):
        self.assertEqual(
            local_targets("docs/get-started.md", "[App](../desktop-installer/README.md)"),
            {"desktop-installer/README.md"},
        )
        self.assertEqual(
            local_targets("docs/get-started.md", "[App](desktop-installer/README.md)"),
            {"docs/desktop-installer/README.md"},
        )

    def test_external_link_is_not_a_local_route(self):
        self.assertEqual(local_targets("README.md", "[App](https://example.invalid/desktop-installer/README.md)"), set())

    def test_repository_escape_fails(self):
        with self.assertRaisesRegex(AssertionError, "link escapes repository"):
            local_targets("README.md", "[Outside](../outside.md)")

    def test_canonical_github_main_routes_map_to_actual_local_targets(self):
        self.assertEqual(local_targets("docs/printables-draft.md",
                         "[Project](https://github.com/bigMOTOR/prusa-remote-display)\n"
                         "[App](https://github.com/bigMOTOR/prusa-remote-display/blob/main/desktop-installer/README.md)"),
                         {"README.md", "desktop-installer/README.md"})
        for host in ("github.com.evil.invalid", "github.com:443", "other.invalid"):
            with self.subTest(host=host):
                self.assertEqual(local_targets("README.md",
                                 f"[App](https://{host}{GITHUB_PATH}/blob/main/desktop-installer/README.md)"), set())

    def test_listing_links_are_portable_and_editor_notes_cannot_supply_routes(self):
        text = (ROOT / "docs/printables-draft.md").read_text(encoding="utf-8")
        body = listing_body(text)
        for href in re.findall(r"(?<!!)\[[^\]\n]+\]\(([^\s)]+)\)", body):
            self.assertEqual(urlsplit(href).scheme, "https", href)
        changed = text.replace("https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/get-started.md",
                               "https://example.invalid/")
        for prefix in ("", "## Listing text — preview\n"):
            with self.subTest(prefix=prefix):
                with self.assertRaisesRegex(AssertionError, "missing visible routes: docs/get-started.md"):
                    check_routes("docs/printables-draft.md", prefix + changed +
                                 "\n## Editor notes — not for publication\n"
                                 "[Guide](get-started.md)\n")

    def test_each_final_listing_route_is_required(self):
        document = "docs/printables-draft.md"
        text = (ROOT / document).read_text(encoding="utf-8")
        check_routes(document, text)
        for target in REQUIRED[document]:
            with self.subTest(target=target):
                route = "https://github.com" + GITHUB_PATH
                if target != "README.md":
                    route += "/blob/main/" + target
                changed = text.replace("(" + route + ")", "(https://example.invalid/)")
                self.assertNotEqual(text, changed)
                with self.assertRaisesRegex(AssertionError, "missing visible routes: " + re.escape(target)):
                    check_routes(document, changed)

    def test_hidden_listing_route_cannot_restore_missing_route(self):
        document = "docs/printables-draft.md"
        text = (ROOT / document).read_text(encoding="utf-8")
        route = "https://github.com" + GITHUB_PATH + "/blob/main/enclosure/assembly.md"
        changed = text.replace(route, "https://example.invalid/")
        for wrapper in ("<!-- {} -->", "```markdown\n{}\n```", "`{}`", "    {}"):
            with self.subTest(wrapper=wrapper):
                with self.assertRaisesRegex(AssertionError, "missing visible routes: enclosure/assembly.md"):
                    check_routes(document, changed + "\n" + wrapper.format(f"[Assembly]({route})"))

    def test_listing_release_route_is_visible_and_official(self):
        document = "docs/printables-draft.md"
        text = (ROOT / document).read_text(encoding="utf-8")
        route = "https://github.com" + GITHUB_PATH + "/releases"
        changed = text.replace(route, "https://example.invalid/")
        for extra in ("", "\n<!-- [Release](" + route + ") -->",
                      "\n## Editor notes — not for publication\n[Release](" + route + ")"):
            with self.subTest(extra=extra):
                with self.assertRaisesRegex(AssertionError, "missing official release route"):
                    check_newcomer_contract(document, changed + extra)
        for host in ("github.com.evil.invalid", "github.com:443"):
            with self.subTest(host=host):
                with self.assertRaisesRegex(AssertionError, "missing official release route"):
                    check_newcomer_contract(document, text.replace("https://github.com", "https://" + host))
        check_newcomer_contract(document, text.replace(route, route + "/tag/v1.0.0"))

    def test_listing_browser_sentence_cannot_be_hidden_in_editor_notes(self):
        document = "docs/printables-draft.md"
        text = (ROOT / document).read_text(encoding="utf-8")
        sentence = "Use Chrome or Edge (Safari and Firefox are not supported)."
        changed = text.replace(sentence, "Use Safari or Firefox.")
        for suffix in ("\n## Editor notes — not for publication\n" + sentence,
                       "\n<!-- " + sentence + " -->", "\n```\n" + sentence + "\n```"):
            with self.subTest(suffix=suffix):
                with self.assertRaisesRegex(AssertionError, "missing required browser sentence"):
                    check_newcomer_contract(document, changed + suffix)

    def test_broken_canonical_github_target_fails(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "missing local target: docs/no-such-guide.md"):
            check_routes("README.md", text +
                         "\n[Missing](https://github.com/bigMOTOR/prusa-remote-display/blob/main/docs/no-such-guide.md)")

    def test_broken_editor_note_link_still_fails(self):
        text = (ROOT / "docs/printables-draft.md").read_text(encoding="utf-8")
        with self.assertRaisesRegex(AssertionError, "missing local target: docs/no-such-editor-note.md"):
            check_routes("docs/printables-draft.md", text + "\n[Editor](no-such-editor-note.md)")

    def test_first_flash_selects_project_and_negative_control_fires(self):
        text = (ROOT / "docs/development/first-flash.md").read_text(encoding="utf-8")
        check_flash_commands(text)
        with self.assertRaisesRegex(AssertionError, "must select firmware"):
            check_flash_commands(text.replace("-C firmware ", ""))
        with self.assertRaisesRegex(AssertionError, "wrong ESP-IDF project"):
            check_flash_commands(text.replace("-C firmware ", "-C . "))

    def test_developer_gate_and_decision_are_discoverable(self):
        render = (ROOT / "docs/development/ui-rendering.md").read_text(encoding="utf-8")
        self.assertIn("bash ./firmware/tools/test-render.sh", render)
        self.assertIn("python3 firmware/tools/render-frame-to-png.py", render)
        self.assertTrue((ROOT / "firmware/tools/render-frame-to-png.py").is_file())
        index = (ROOT / "docs/decisions/README.md").read_text(encoding="utf-8")
        self.assertIn("[0013](0013-desktop-installation.md)", index)


if __name__ == "__main__":
    unittest.main()
