# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
import hashlib
import json
import unittest
from dataclasses import replace
from unittest.mock import Mock, patch
from urllib.error import HTTPError, URLError
import core


class ReleaseTests(unittest.TestCase):
    def metadata(self):
        return {"tag_name": "v0.1.0", "draft": False, "prerelease": False, "immutable": True,
                "assets": [{"name": n, "size": 1, "state": "uploaded",
                            "browser_download_url": f"https://github.com/{core.REPOSITORY}/releases/download/v0.1.0/{n}"}
                           for n in core.LIMITS]}

    def fetch(self, value):
        def get(url, limit):
            return json.dumps({"sha": "a" * 40} if "/commits/" in url else value).encode()
        return get

    def test_official_stable_release_and_full_commit(self):
        result = core.discover(self.fetch(self.metadata()))
        self.assertEqual((result.tag, result.commit), ("v0.1.0", "a" * 40))
        self.assertEqual(dict(result.assets).keys(), core.LIMITS.keys())

    def test_reject_incomplete_prerelease_main_and_app_tags(self):
        for tag in ("main", "v0.1.0-beta", "installer-v0.1.0", "", None):
            with self.subTest(tag=tag):
                value = self.metadata()
                value["tag_name"] = tag
                with self.assertRaises(core.InstallError):
                    core.discover(self.fetch(value))
        for flag in ("draft", "prerelease"):
            value = self.metadata()
            value[flag] = True
            with self.assertRaises(core.InstallError):
                core.discover(self.fetch(value))
        value = self.metadata()
        value["assets"].pop()
        with self.assertRaisesRegex(core.InstallError, "complete"):
            core.discover(self.fetch(value))

    def test_reject_asset_identity_duplicate_and_oversize(self):
        for change in ({"browser_download_url": "https://example.com/evil.bin"},
                       {"size": 20 * 1024 * 1024}, {"size": True}, {"state": "new"}):
            value = self.metadata()
            value["assets"][0].update(change)
            with self.assertRaises(core.InstallError):
                core.discover(self.fetch(value))
        value = self.metadata()
        value["assets"].append(value["assets"][0])
        with self.assertRaisesRegex(core.InstallError, "Duplicate"):
            core.discover(self.fetch(value))

    def test_no_untrusted_or_insecure_redirect(self):
        for url in ("http://github.com/x", "https://github.com.evil.org/x",
                    "https://user:pass@github.com/x", "https://localhost/x",
                    "https://github.com:8080/x", "file:///tmp/image", "https://github.com/x#y"):
            with self.subTest(url=url), self.assertRaises(core.InstallError):
                core.validate_url(url, download=True)
        core.validate_url("https://release-assets.githubusercontent.com/asset", download=True)

    def test_duplicate_json_does_not_override_contract(self):
        with self.assertRaises(core.InstallError):
            core.unique_json(b'{"draft":true,"draft":false}')

    def test_no_download_without_publisher_trust(self):
        fetch = Mock()
        with self.assertRaisesRegex(core.InstallError, "trust"):
            core.prepare(core.Release("v0.1.0", "a" * 40, ()), b"", None, fetch)
        fetch.assert_not_called()

    def test_mutable_missing_and_nonboolean_immutability_are_rejected(self):
        for immutable in (None, False, "true", 1):
            value = self.metadata()
            if immutable is None:
                del value["immutable"]
            else:
                value["immutable"] = immutable
            with self.subTest(immutable=immutable), self.assertRaises(core.InstallError):
                core.discover(self.fetch(value))

    def test_network_errors_are_readable_without_false_connectivity_claims(self):
        cases = [(HTTPError(core.API, 404, "Not Found", {}, None), "No firmware release"),
                 (HTTPError(core.API, 403, "Forbidden", {}, None), "GitHub"),
                 (HTTPError(core.API, 429, "Limit", {}, None), "later"),
                 (URLError("private diagnostic errno"), "connection"),
                 (TimeoutError("private timeout"), "timed out")]
        for error, message in cases:
            with self.subTest(error=error), patch.object(core, "build_opener") as opener:
                opener.return_value.open.side_effect = error
                with self.assertRaisesRegex(core.InstallError, message) as raised:
                    core.get_bytes(f"{core.API}/releases/latest", 8192)
                self.assertNotIn("private", str(raised.exception))
        with patch.object(core, "build_opener") as opener:
            opener.return_value.open.side_effect = HTTPError(core.API, 404, "Not Found", {}, None)
            with self.assertRaisesRegex(core.InstallError, "file is unavailable"):
                core.get_bytes(f"https://github.com/{core.REPOSITORY}/releases/download/v0.1.0/manifest.json",
                               8192, download=True)


class SessionTests(unittest.TestCase):
    def session(self):
        data = b"non-executable-test-fixture"
        image = core.VerifiedImage(core.Release("v0.1.0", "a" * 40, ()), data,
                                   hashlib.sha256(data).hexdigest())
        return core.Session(image, "test-port-not-a-device", True, True, False, True)

    def test_all_confirmations_required_without_serial_access(self):
        for name, value in (("image", None), ("port", ""), ("port", "   "),
                            ("board_confirmed", False), ("erase_confirmed", False),
                            ("enabled", False), ("busy", True)):
            with self.subTest(name=name):
                session = self.session()
                setattr(session, name, value)
                runner = Mock()
                self.assertFalse(session.can_install)
                with self.assertRaises(core.InstallError):
                    core.install(session, session.port, runner)
                runner.assert_not_called()

    def test_changed_port_and_image_are_rejected(self):
        runner = Mock()
        session = self.session()
        with self.assertRaises(core.InstallError):
            core.install(session, "different-port", runner)
        session.image = replace(session.image, data=b"tampered")
        with self.assertRaisesRegex(core.InstallError, "changed"):
            core.install(session, session.port, runner)
        runner.assert_not_called()

    def test_exact_explicit_port_no_force_options_and_cleanup(self):
        from pathlib import Path
        session = self.session()
        captured = []
        def runner(args):
            captured.append(args)
            self.assertTrue(session.busy)
            self.assertEqual(Path(args[-1]).read_bytes(), session.image.data)
        core.install(session, session.port, runner)
        args = captured[0]
        self.assertEqual(args[args.index("--port") + 1], session.port)
        self.assertIn("--erase-all", args)
        self.assertNotIn("--force", args)
        self.assertEqual(args[args.index("--chip") + 1], "esp32s3")
        self.assertFalse(Path(args[-1]).exists())
        self.assertFalse(session.busy)

    def test_error_resets_busy_without_false_success(self):
        session = self.session()
        runner = Mock(side_effect=core.InstallError("test disconnection"))
        with self.assertRaisesRegex(core.InstallError, "disconnection"):
            core.install(session, session.port, runner)
        self.assertFalse(session.busy)


if __name__ == "__main__":
    unittest.main()
