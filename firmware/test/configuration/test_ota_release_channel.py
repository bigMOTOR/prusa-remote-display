# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Firing controls for the private-LAN OTA compile-time channel boundary."""

import importlib.util
import os
from pathlib import Path
import json
import shlex
import subprocess
import tempfile
import textwrap
import unittest


FIRMWARE = Path(__file__).resolve().parents[2]
GUARD = FIRMWARE / "cmake/ota_release_channel.cmake"
ORIGIN = "https://192.168.1.40:8443"
CERTIFICATE = "-----BEGIN CERTIFICATE-----\n" + "A" * 128 + "\n-----END CERTIFICATE-----\n"
FIXTURE_SPEC = importlib.util.spec_from_file_location(
    "prd_lan_fixture_for_decoder", FIRMWARE / "tools/serve-ota-validation.py"
)
assert FIXTURE_SPEC is not None and FIXTURE_SPEC.loader is not None
fixture = importlib.util.module_from_spec(FIXTURE_SPEC)
FIXTURE_SPEC.loader.exec_module(fixture)


class OtaReleaseChannelTests(unittest.TestCase):
    def run_guard(self, profile: str, origin: str, certificate: str | None = CERTIFICATE):
        with tempfile.TemporaryDirectory(prefix="prd-ota-channel-") as directory:
            root = Path(directory)
            pem = root / "ca.pem"
            if certificate is not None:
                pem.write_text(certificate, encoding="ascii")
            script = root / "select.cmake"
            script.write_text(
                textwrap.dedent(
                    f"""\
                    include("{GUARD}")
                    prd_select_ota_validation_lan(
                        "${{TEST_PROFILE}}" "${{TEST_ORIGIN}}" "${{TEST_CERT}}" SELECTED CERT)
                    message(STATUS "selected=${{SELECTED}}")
                    """
                ),
                encoding="utf-8",
            )
            return subprocess.run(
                [
                    "cmake",
                    f"-DTEST_PROFILE={profile}",
                    f"-DTEST_ORIGIN={origin}",
                    f"-DTEST_CERT={pem if certificate is not None else ''}",
                    "-P",
                    str(script),
                ],
                capture_output=True,
                text=True,
            )

    def test_empty_override_keeps_every_profile_on_official_channel(self):
        for profile in ("debug", "hardware-validation", "release"):
            with self.subTest(profile=profile):
                result = self.run_guard(profile, "", None)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("selected=\n", result.stdout)

    def test_private_lan_requires_hardware_validation_and_both_inputs(self):
        result = self.run_guard("hardware-validation", ORIGIN)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"selected={ORIGIN}", result.stdout)
        for profile in ("debug", "release"):
            with self.subTest(profile=profile):
                result = self.run_guard(profile, ORIGIN)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("only in hardware-validation", result.stderr)
        self.assertIn("requires both", self.run_guard("hardware-validation", ORIGIN, None).stderr)
        self.assertIn("requires both", self.run_guard("hardware-validation", "").stderr)

    def test_only_exact_private_https_ipv4_and_a_public_certificate_are_accepted(self):
        invalid = (
            "http://192.168.1.40:8443",
            "https://192.168.1.40:8443/extra",
            "https://user@192.168.1.40:8443",
            "https://192.168.1.40:8443?x=1",
            "https://192.168.1.999:8443",
            "https://192.168.001.40:8443",
            "https://8.8.8.8:8443",
            "https://127.0.0.1:8443",
            "https://192.168.1.40:0",
            "https://192.168.1.40:65536",
            "https://192.168.1.40:08443",
            "https://api.github.com:443",
        )
        for origin in invalid:
            with self.subTest(origin=origin):
                self.assertNotEqual(self.run_guard("hardware-validation", origin).returncode, 0)
        self.assertNotEqual(
            self.run_guard("hardware-validation", ORIGIN, "-----BEGIN PRIVATE KEY-----\n" + "A" * 128).returncode,
            0,
        )

    def test_compiled_default_and_validation_urls_cannot_cross(self):
        source = (
            '#include "ota/release_discovery.hpp"\n'
            "#include <iostream>\n"
            "int main() { std::cout << prd::ota::release_discovery_url(); }\n"
        )
        with tempfile.TemporaryDirectory(prefix="prd-ota-channel-compile-") as directory:
            executable = Path(directory) / "channel"
            base = [
                *shlex.split(os.environ.get("CXX", "c++")),
                "-std=c++17",
                "-Werror",
                "-x",
                "c++",
                "-I",
                str(FIRMWARE / "components/ota/include"),
                "-I",
                str(FIRMWARE / "components/json_reader/include"),
            ]
            for definitions, expected in (
                ([], "https://api.github.com/repos/bigMOTOR/prusa-remote-display/releases/latest"),
                (["-DPRD_OTA_LAN_MODE=1", f'-DPRD_OTA_LAN_ORIGIN="{ORIGIN}"'], f"{ORIGIN}/releases/latest"),
            ):
                with self.subTest(expected=expected):
                    result = subprocess.run(
                        [
                            *base,
                            *definitions,
                            "-",
                            str(FIRMWARE / "components/ota/src/release_discovery.cpp"),
                            str(FIRMWARE / "components/ota/src/release_policy.cpp"),
                            str(FIRMWARE / "components/json_reader/src/reader.cpp"),
                            "-o",
                            str(executable),
                        ],
                        input=source,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    output = subprocess.run([str(executable)], capture_output=True, text=True, check=True)
                    self.assertEqual(output.stdout, expected)

    def test_hardware_defaults_cannot_contact_deleted_validation_repository(self):
        hardware = (FIRMWARE / "sdkconfig.defaults.hardware-validation").read_text()
        self.assertIn('CONFIG_PRD_OTA_LAN_ORIGIN=""', hardware)
        self.assertIn('CONFIG_PRD_OTA_LAN_CA_CERT_PATH=""', hardware)
        self.assertNotIn("prusa-remote-display-validation", hardware)

    def test_lan_decoder_accepts_fixture_shape_but_rejects_cross_origin_assets(self):
        source = textwrap.dedent(
            """\
            #include "ota/release_discovery.hpp"
            #include "ota/release_policy.hpp"
            #include <iostream>
            #include <string>

            int main() {
                std::string body;
                std::getline(std::cin, body);
                prd::ota::ReleaseCandidate candidate;
                const auto status = prd::ota::decode_release(body, candidate);
                if (status != prd::ota::ReleaseStatus::Decoded) return 2;
                if (candidate.release_id != 11 || candidate.tag_view() != "v0.0.2") return 3;
                if (candidate.manifest.id != 111 || candidate.image.id != 112) return 4;
                prd::ota::ReleaseRoute route{prd::ota::ReleaseResource::Image};
                if (!prd::ota::release_url_policy(
                        &route, {}, candidate.image.url_view(), 0)) return 5;
                if (prd::ota::release_url_policy(
                        &route, candidate.image.url_view(),
                        "https://release-assets.githubusercontent.com/github-production-release-asset/1/x?y=1",
                        1)) return 6;
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory(prefix="prd-ota-lan-decode-") as directory:
            root = Path(directory)
            executable = root / "decode"
            (root / fixture.MANIFEST_NAME).write_text(
                json.dumps({"releaseTag": "v0.0.2"}), encoding="utf-8"
            )
            (root / fixture.IMAGE_NAME).write_bytes(b"disposable-test-image")
            release = json.loads(fixture.load_candidate(root, "v0.0.2", 11, ORIGIN)["release.json"])
            command = [
                *shlex.split(os.environ.get("CXX", "c++")), "-std=c++17", "-Werror",
                "-DPRD_OTA_LAN_MODE=1", f'-DPRD_OTA_LAN_ORIGIN="{ORIGIN}"',
                "-I", str(FIRMWARE / "components/ota/include"),
                "-I", str(FIRMWARE / "components/json_reader/include"),
                "-x", "c++", "-",
                str(FIRMWARE / "components/ota/src/release_discovery.cpp"),
                str(FIRMWARE / "components/ota/src/release_policy.cpp"),
                str(FIRMWARE / "components/json_reader/src/reader.cpp"),
                "-o", str(executable),
            ]
            compiled = subprocess.run(command, input=source, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)

            def run(candidate):
                return subprocess.run(
                    [str(executable)], input=json.dumps(candidate), capture_output=True, text=True
                )

            self.assertEqual(run(release).returncode, 0)
            release["assets"][1]["browser_download_url"] = (
                "https://github.com/bigMOTOR/prusa-remote-display/releases/download/"
                "v0.0.2/prusa_remote_display.bin"
            )
            self.assertNotEqual(run(release).returncode, 0)
            release["assets"][1]["browser_download_url"] = (
                f"{ORIGIN}/releases/download/v0.0.2/prusa_remote_display.bin"
            )
            release["url"] = "https://api.github.com/repos/bigMOTOR/prusa-remote-display/releases/11"
            self.assertNotEqual(run(release).returncode, 0)


if __name__ == "__main__":
    unittest.main()
