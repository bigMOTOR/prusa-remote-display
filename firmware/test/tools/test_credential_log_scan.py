# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Tool-level tests for the §13.5 credential-log gate.

⛔ WHY THESE EXIST. `network::restart()` logged the configured home SSID at Info, which release
compiles in. The path needs a SECOND same-boot Wi-Fi credential submission — restart() runs only when
the station is already up — so no bench capture of a first-time provisioning could reach it, and the
§13.5 packaging scan is static while the SSID is runtime data, so no artifact scan could detect it
either. `wifi.cpp` includes esp_wifi.h and cannot be compiled by the host C++ suite, so the source is
the only place that property is assertable at all.

⛔ WHAT THE GATE IS, SO THESE TESTS ARE NOT READ AS MORE THAN THEY ARE. It is a deterministic
regression guard over the DIRECT SOURCE TEXT of known project-owned logging shapes. It is NOT alias
or data-flow analysis: a credential copied into a differently named local, or formatted by a helper,
or reached through a reference, is outside its reach. These tests pin the shapes it does cover — they
do not establish that no credential can ever reach a log.

⬅️ EVERY NEGATIVE CLAIM HERE HAS A POSITIVE CONTROL IN THE SAME CLASS. A detector that reported
nothing would satisfy every "this is allowed" case on its own, so each of those sits beside an
offending variant that must be reported.

⛔ NO REAL CREDENTIAL APPEARS BELOW. Every snippet is synthetic C++ that is never compiled, and no
value in it is or has been a usable secret.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent.parent / "tools"
REPO = Path(__file__).resolve().parent.parent.parent.parent

_spec = importlib.util.spec_from_file_location(
    "credential_log_scan", TOOLS / "check-credential-logs.py"
)
assert _spec is not None and _spec.loader is not None
credential_log_scan = importlib.util.module_from_spec(_spec)
sys.modules["credential_log_scan"] = credential_log_scan
_spec.loader.exec_module(credential_log_scan)


def marker_line() -> str:
    """The allowance marker as a source comment, taken from the gate so the two cannot drift."""
    return f"    // {credential_log_scan.ALLOWANCE_MARKER} (§14.2)\n"


# The permitted call as setup_ap.cpp actually writes it, including the trailing sentence about the
# password — which is INSIDE a string literal and must not be mistaken for logging one.
SETUP_SSID_CALL = (
    "    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,\n"
    '                     "setup ap up ssid=" + std::string{ssid} +\n'
    '                         " channel=" + std::to_string(kChannel) +\n'
    '                         " (no server yet; the password is not logged in any build)");\n'
)


def findings(source: str) -> list[tuple[int, str, str]]:
    return credential_log_scan.scan_text(source)


class TheSetupSsidAllowanceIsCallScoped(unittest.TestCase):
    """Correction 1: the path-wide exemption is gone, and this is what replaced it."""

    def test_the_generated_setup_ssid_log_is_allowed_when_marked(self) -> None:
        # Control 1. The device's own session identity, which §14.2 has the panel show.
        self.assertEqual(findings(marker_line() + SETUP_SSID_CALL), [])

    def test_the_same_log_is_rejected_without_the_marker(self) -> None:
        # ⛔ Control 7, from the other side: the allowance is the MARKER, not the file and not the
        # shape. An unmarked network name is a finding wherever it appears.
        self.assertEqual(len(findings(SETUP_SSID_CALL)), 1)

    def test_adding_the_setup_password_to_that_same_log_is_rejected(self) -> None:
        # ⛔ Control 2. THE CASE THE OLD PATH-WIDE EXEMPTION WOULD HAVE MISSED ENTIRELY.
        leaking = SETUP_SSID_CALL.replace(
            '" channel="', '" pw=" + std::string{password} + " channel="'
        )
        reported = findings(marker_line() + leaking)

        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_a_credential_mixed_into_the_permitted_call_is_rejected(self) -> None:
        # ⛔ Control 4. The marker does not launder the rest of the call.
        leaking = SETUP_SSID_CALL.replace(
            "std::string{ssid}", "std::string{ssid} + std::string{session.password.view()}"
        )
        reported = findings(marker_line() + leaking)

        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_a_separate_credential_log_elsewhere_in_the_file_is_rejected(self) -> None:
        # ⛔ Control 3. A second call in the same file, nowhere near the marker.
        elsewhere = (
            marker_line()
            + SETUP_SSID_CALL
            + "\n    // ... other code ...\n\n"
            + "    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,\n"
            + '                     "setup ap pw=" + std::string{password});\n'
        )
        reported = findings(elsewhere)

        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_a_separate_session_password_timeline_elsewhere_is_rejected(self) -> None:
        elsewhere = (
            marker_line()
            + SETUP_SSID_CALL
            + '\n    timeline("ap-up", "pw", session.password.view());\n'
        )
        self.assertEqual(len(findings(elsewhere)), 1)

    def test_the_marker_cannot_rescue_a_home_network_name(self) -> None:
        # ⛔ The credential class is checked FIRST and unconditionally. Copying the marker onto a
        # genuine leak buys nothing.
        marked_leak = (
            marker_line()
            + '    diagnostics::log(diagnostics::LogLevel::Info, tag, "ssid=" + g_ssid);\n'
        )
        reported = findings(marked_leak)

        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_one_marker_cannot_permit_a_second_session_ssid_log(self) -> None:
        # ⛔ THE WEAKNESS THIS REPLACED. The marker used to permit any session-identity call within
        # three lines below it, and two one-line log calls fit inside three lines — so one marker
        # could cover both. Each marker now binds to THE NEXT call and is spent.
        one_line = '    diagnostics::log(l, t, "ssid=" + std::string{ssid});\n'
        reported = findings(marker_line() + one_line + one_line)

        self.assertEqual(len(reported), 1, reported)
        self.assertEqual(reported[0][2], "logs a network name without an allowance marker")

    def test_two_markers_permit_two_calls_each_separately_counted(self) -> None:
        one_line = '    diagnostics::log(l, t, "ssid=" + std::string{ssid});\n'
        self.assertEqual(findings(marker_line() + one_line + marker_line() + one_line), [])

    def test_an_orphan_marker_is_reported_rather_than_ignored(self) -> None:
        # ⛔ AMBIGUOUS PERMISSION FAILS LOUDLY. A marker that binds to nothing is a marker somebody
        # moved, deleted a call from, or copied — none of which should pass quietly.
        reported = findings(marker_line() + "    return true;\n")

        self.assertEqual(len(reported), 1, reported)
        self.assertIn("binds to no logging call", reported[0][2])

    def test_executable_code_between_marker_and_call_breaks_the_binding(self) -> None:
        reported = findings(marker_line() + "    radio::start();\n" + SETUP_SSID_CALL)

        reasons = {reason for _, _, reason in reported}
        self.assertIn(
            "executable code sits between the allowance marker and the next log call", reasons
        )

    def test_comments_and_blank_lines_between_marker_and_call_are_permitted(self) -> None:
        # Only executable code breaks the binding. An explanatory comment is exactly what belongs
        # between a marker and the call it justifies.
        between = "    // an explanatory line\n\n    /* and a block comment */\n"
        self.assertEqual(findings(marker_line() + between + SETUP_SSID_CALL), [])


class TheLexicalMaskingCannotBeBypassed(unittest.TestCase):
    """Correction to a MEASURED bypass, not a hypothetical one.

    ⛔ WHAT WENT WRONG. The first implementation blanked block comments, then line comments, then
    string literals. That let this through with no finding at all:

        diagnostics::log(level, tag, "http://device/" + std::string{password});

    the `//` inside the URL was blanked as a line comment, taking `device/" + std::string{...})` with
    it — so the credential AND the call's closing parenthesis both vanished.

    ⛔ AND SWAPPING THE ORDER ONLY INVERTS IT, which is why the fix is a state machine and not a
    reordering. With literals blanked first, a block comment holding one quote erases the CALL:

        /* he said " */ diagnostics::log(level, tag, "x" + std::string{password});

    Both orders were measured to have a bypass. Every case below is one of the two directions.
    """

    def test_the_reported_bypass_is_rejected(self) -> None:
        # ⛔ THE EXACT REPORTED CASE. This is the regression.
        source = '    diagnostics::log(level, tag, "http://device/" + std::string{password});\n'
        reported = findings(source)

        self.assertEqual(len(reported), 1, reported)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_a_block_comment_sequence_inside_a_literal_hides_nothing(self) -> None:
        source = (
            "    diagnostics::log(level, tag,\n"
            '                     "/* not a comment */" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_a_quote_inside_a_real_block_comment_does_not_hide_the_call(self) -> None:
        # ⛔ THE INVERSE BYPASS. Rejecting this is what a reordering could not do.
        source = (
            '    /* he said " */ diagnostics::log(level, tag, "x" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_escaped_quotes_do_not_terminate_a_string_early(self) -> None:
        source = (
            '    diagnostics::log(level, tag, "say \\"hi\\" //x" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_a_raw_string_with_comment_looking_text_hides_nothing(self) -> None:
        source = (
            '    diagnostics::log(level, tag, R"(// /* not code */)" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_a_raw_string_with_a_custom_delimiter_hides_nothing(self) -> None:
        source = '    diagnostics::log(level, tag, R"tag(// ")tag" + std::string{password});\n'
        self.assertEqual(len(findings(source)), 1)

    def test_a_credential_word_in_a_real_comment_stays_ignored(self) -> None:
        source = (
            "    // the password is deliberately absent from the line below\n"
            '    diagnostics::log(level, tag, "setup ap up");\n'
        )
        self.assertEqual(findings(source), [])

    def test_a_credential_word_in_a_message_literal_stays_ignored(self) -> None:
        # ⬅️ A REAL LINE FROM provisioning_scene.cpp.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Warning, kTag,\n"
            '                     "setup qr not drawn; the panel still shows the network name, "\n'
            '                     "password and address as text");\n'
        )
        self.assertEqual(findings(source), [])

    def test_digit_separators_are_not_character_literals(self) -> None:
        # ⛔ C++14 `2'000'000`, which this repository uses. Mis-lexing it as a character literal
        # would swallow everything to the next quote and hide whatever followed.
        source = (
            "    constexpr std::int64_t kFactoryResetHoldUs = 2'000'000;\n"
            '    diagnostics::log(level, tag, "x" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_a_character_literal_holding_a_quote_does_not_hide_what_follows(self) -> None:
        source = (
            "    const char quote = '\\'';\n"
            '    diagnostics::log(level, tag, "x" + std::string{password});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_masking_preserves_every_byte_offset_and_line(self) -> None:
        # ⛔ WHY THIS MATTERS BEYOND TIDINESS. Call extraction and line reporting both index into the
        # masked text, so a mask that changed length would silently misreport locations.
        source = (
            '    // a comment with "quotes" and /* markers */\n'
            '    diagnostics::log(l, t, R"(raw)" "plain" \'c\' + x);\n'
            "    /* block\n       spanning lines */\n"
        )
        masked = credential_log_scan._mask(source)

        self.assertEqual(len(masked.code), len(source))
        self.assertEqual(masked.code.count("\n"), source.count("\n"))
        for index, character in enumerate(source):
            if character == "\n":
                self.assertEqual(masked.code[index], "\n", f"newline lost at {index}")

    def test_the_line_number_reported_is_the_call_s_own(self) -> None:
        source = (
            "    // padding\n"
            "    // padding\n"
            '    diagnostics::log(level, tag, "http://x/" + std::string{password});\n'
        )
        reported = findings(source)
        self.assertEqual(len(reported), 1)
        self.assertEqual(reported[0][0], 3)


class NoFileIsExemptFromInspection(unittest.TestCase):
    """Correction 1's structural half: the gate has no path-wide skip at all."""

    def test_the_gate_exposes_no_file_exemption_mechanism(self) -> None:
        # ⛔ THE REGRESSION FOR THE CORRECTION ITSELF. The earlier revision carried an EXCEPTIONS
        # table of path suffixes that were skipped before being read. Its absence is the fix, so its
        # absence is asserted rather than assumed.
        self.assertFalse(
            hasattr(credential_log_scan, "EXCEPTIONS"),
            "a path-wide exemption table must not exist",
        )

    def test_setup_ap_is_actually_scanned(self) -> None:
        # And the file that used to be skipped is provably in the scanned set.
        scanned = credential_log_scan.sources(
            [REPO / "firmware" / "components", REPO / "firmware" / "main"]
        )
        names = {path.as_posix() for path in scanned}
        self.assertTrue(
            any(name.endswith("components/network/src/setup_ap.cpp") for name in names),
            "setup_ap.cpp must be inspected, not exempt",
        )

    def test_exactly_one_allowance_exists_in_the_tree(self) -> None:
        # ⛔ THE EROSION GUARD. One marker is a decision; five is a habit. Growth fails here.
        found = credential_log_scan.markers(
            credential_log_scan.sources(
                [REPO / "firmware" / "components", REPO / "firmware" / "main"]
            )
        )
        self.assertEqual(
            [path.as_posix().split("firmware/")[-1] for path, _ in found],
            ["components/network/src/setup_ap.cpp"],
            f"unexpected allowance markers: {found}",
        )


class DetectsTheRealDefect(unittest.TestCase):
    """The exact line that shipped, and the exact line that replaced it."""

    def test_the_removed_restart_line_is_reported(self) -> None:
        # ⛔ VERBATIM, minus the value's name. This is what `network::restart()` contained at
        # 29f2cc28 and what the release image emitted on a second same-boot submission.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,\n"
            '                     "wifi station restarted for a new credential ssid=" + g_ssid);\n'
        )
        reported = findings(source)

        self.assertEqual(len(reported), 1, reported)
        self.assertEqual(reported[0][0], 1)
        self.assertEqual(reported[0][2], "logs a credential value")

    def test_the_replacement_line_is_allowed(self) -> None:
        # Presence plus lifecycle state: neither is a credential.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,\n"
            '                     std::string{"wifi station restarted for a new credential '
            'password="} +\n'
            '                         (credentials.password.empty() ? "absent" : "present") +\n'
            '                         " phase=" + std::string{app_model::log_label(step.phase)});\n'
        )
        self.assertEqual(findings(source), [])


class SeparatesValuesFromPresenceAndOutcomes(unittest.TestCase):
    """The distinction the first draft of this gate got wrong."""

    def test_a_field_outcome_printed_through_log_label_is_allowed(self) -> None:
        # ⬅️ THE FALSE POSITIVE THIS PINS. `report.wifi_ssid` is a FieldOutcome — a validation
        # verdict rendered as "ok" or "too-long" — and an earlier draft reported seven such lines in
        # server.cpp as credential leaks.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            '                     "ssid=" + std::string{log_label(report.wifi_ssid)} +\n'
            '                     " key=" + std::string{log_label(report.printer_api_key)});\n'
        )
        self.assertEqual(findings(source), [])

    def test_a_presence_boolean_is_allowed(self) -> None:
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            '                     std::string{"ssid="} + yes_no(!submitted.wifi_ssid.empty()) +\n'
            '                     " key=" + yes_no(!submitted.printer_api_key.empty()));\n'
        )
        self.assertEqual(findings(source), [])

    def test_a_length_is_allowed(self) -> None:
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            '                     "key_len=" + std::to_string(submitted.printer_api_key.size()));\n'
        )
        self.assertEqual(findings(source), [])

    def test_one_raw_value_among_several_presence_checks_still_reports(self) -> None:
        # ⛔ THE CASE A WHOLE-CALL HEURISTIC WOULD MISS. Most of this line is correct, which is
        # exactly how such a value gets added and reviewed past.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            '                     std::string{"ssid="} + yes_no(!submitted.wifi_ssid.empty()) +\n'
            '                     " key=" + std::string{submitted.printer_api_key.view()});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_the_word_password_inside_a_message_is_not_a_credential(self) -> None:
        # ⛔ WHY LITERALS ARE BLANKED BEFORE MATCHING. Two real lines in this repository say the word
        # "password" inside their message — including the very setup-AP line the allowance permits,
        # which ends "the password is not logged in any build". Reporting the sentence that documents
        # the rule as a breach of it would make the gate unusable.
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Warning, kTag,\n"
            '                     "setup qr not drawn; the panel still shows the network name, "\n'
            '                     "password and address as text");\n'
        )
        self.assertEqual(findings(source), [])

    def test_a_comment_inside_a_call_is_not_credential_code(self) -> None:
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            "                     // the password is deliberately absent here\n"
            '                     "up");\n'
        )
        self.assertEqual(findings(source), [])


class CoversEveryProjectOwnedSink(unittest.TestCase):
    """A gate that watched one entry point would be bypassed by the other."""

    def test_the_timeline_wrapper_is_covered(self) -> None:
        # wifi.cpp's file-local timeline() reaches ESP_LOGI just as diagnostics::log does.
        self.assertEqual(len(findings('    timeline("joined", "ssid", g_ssid);\n')), 1)

    def test_the_qualified_timeline_is_covered(self) -> None:
        source = (
            '    diagnostics::timeline(diagnostics::tag::kNetwork, "joined",\n'
            "                          std::string{credentials.ssid});\n"
        )
        self.assertEqual(len(findings(source)), 1)

    def test_an_api_key_value_is_covered(self) -> None:
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Info, kTag,\n"
            '                     "key=" + std::string{target.api_key.view()});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_a_wifi_password_value_is_covered(self) -> None:
        source = (
            "    diagnostics::log(diagnostics::LogLevel::Error, kTag,\n"
            '                     "pw=" + std::string{submitted.wifi_password.view()});\n'
        )
        self.assertEqual(len(findings(source)), 1)

    def test_code_outside_a_log_call_is_not_reported(self) -> None:
        # The gate governs logging, not use. wifi.cpp must still be able to hand a credential to the
        # driver, and a gate that forbade that would be unusable.
        source = (
            "    if (!driver_credential::copy_ssid(credentials.ssid, config.sta.ssid,\n"
            "                                      sizeof(config.sta.ssid))) {\n"
            "        return false;\n"
            "    }\n"
        )
        self.assertEqual(findings(source), [])


class HoldsAgainstTheRealTree(unittest.TestCase):
    """The gate's own verdict on the working source, asserted rather than eyeballed."""

    def test_the_repository_is_clean(self) -> None:
        roots = [REPO / "firmware" / "components", REPO / "firmware" / "main"]
        for root in roots:
            self.assertTrue(root.is_dir(), f"{root} not found")

        reported = credential_log_scan.scan_paths(credential_log_scan.sources(roots))
        self.assertEqual(
            reported,
            [],
            "a project-owned log call takes a credential value:\n"
            + "\n".join(f"  {f.path}:{f.line} {f.reason} {f.call}" for f in reported),
        )

    def test_the_scan_actually_reaches_the_adapters(self) -> None:
        # ⛔ THE CONTROL FOR THE TEST ABOVE. "No findings" is also what a scan that read no files
        # would report, so the files the defects lived in must be provably in the scanned set.
        scanned = credential_log_scan.sources(
            [REPO / "firmware" / "components", REPO / "firmware" / "main"]
        )
        names = {path.as_posix() for path in scanned}

        for required in (
            "components/network/src/wifi.cpp",
            "components/network/src/setup_ap.cpp",
            "components/provisioning/src/server.cpp",
        ):
            self.assertTrue(
                any(name.endswith(required) for name in names),
                f"{required} must be inside the scanned set",
            )

        self.assertGreater(len(scanned), 100)

    def test_the_real_setup_ap_line_is_permitted_by_its_own_marker(self) -> None:
        # ⬅️ END TO END ON THE REAL FILE, not on a snippet: the marker as committed, the call as
        # committed, and the gate's verdict on the two together.
        path = REPO / "firmware" / "components" / "network" / "src" / "setup_ap.cpp"
        text = path.read_text(encoding="utf-8")

        self.assertIn(credential_log_scan.ALLOWANCE_MARKER, text)
        self.assertIn('"setup ap up ssid="', text)
        self.assertEqual(credential_log_scan.scan_text(text), [])

    def test_removing_the_marker_from_the_real_file_fails_the_gate(self) -> None:
        # ⛔ THE STRONGEST FORM OF CONTROL 1 AND 7 TOGETHER. The permitted line is only permitted
        # BECAUSE of its marker — proved by deleting the marker from the real source text in memory
        # and watching the real call be reported.
        path = REPO / "firmware" / "components" / "network" / "src" / "setup_ap.cpp"
        text = path.read_text(encoding="utf-8")
        without = text.replace(credential_log_scan.ALLOWANCE_MARKER, "removed for this test")

        reported = credential_log_scan.scan_text(without)
        self.assertEqual(len(reported), 1, reported)
        self.assertEqual(reported[0][2], "logs a network name without an allowance marker")

    def test_adding_the_setup_password_to_the_real_line_fails_the_gate(self) -> None:
        # ⛔ CONTROL 2 ON THE REAL FILE. This is the case the old path-wide exemption allowed.
        path = REPO / "firmware" / "components" / "network" / "src" / "setup_ap.cpp"
        text = path.read_text(encoding="utf-8")
        leaking = text.replace(
            '"setup ap up ssid=" + std::string{ssid} +',
            '"setup ap up ssid=" + std::string{ssid} + " pw=" + std::string{password} +',
        )
        self.assertNotEqual(leaking, text, "the substitution must actually apply")

        reported = credential_log_scan.scan_text(leaking)
        self.assertEqual(len(reported), 1, reported)
        self.assertEqual(reported[0][2], "logs a credential value")


if __name__ == "__main__":
    unittest.main()
