# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Offline release-asset gate with firing negative controls."""

import hashlib
import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "prd_release_assets", FIRMWARE / "tools/check-release-assets.py"
)
assert SPEC is not None and SPEC.loader is not None
release_assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_assets)


class ReleaseAssetsTests(unittest.TestCase):
    TAG = "v1.2.3"
    COMMIT = "a" * 40
    LOCAL_ORIGIN = "https://192.168.1.40:8443"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="prd-release-assets-test-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.image = self.directory / release_assets.IMAGE_NAME
        self.image.write_bytes(b"signed-image-fixture")
        self.manifest = {
            "schemaVersion": 1,
            "productVersion": "1.2.3",
            "releaseTag": self.TAG,
            "gitCommit": self.COMMIT[:12],
            "buildProfile": "release",
            "dirtyTree": False,
            "boardTarget": release_assets.BOARD_TARGET,
            "chipTarget": release_assets.CHIP_TARGET,
            "image": {
                "name": release_assets.IMAGE_NAME,
                "sizeBytes": self.image.stat().st_size,
                "sha256": self.digest(self.image),
            },
            "compatibility": {
                "minBootloaderSchema": 1,
                "otaSlotSizeBytes": 1024,
                "otaSlotCount": 2,
            },
            "signing": {
                "algorithm": "RSA-PSS-SHA256",
                "signature": "embedded-esp-secure-boot-v2",
                "publicKeySpkiSha256": "b" * 64,
            },
        }
        self.write_bundle()

    @staticmethod
    def digest(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def write_bundle(self):
        manifest_path = self.directory / release_assets.MANIFEST_NAME
        manifest_path.write_text(json.dumps(self.manifest, sort_keys=True) + "\n", encoding="utf-8")
        checksums = {
            release_assets.MANIFEST_NAME: self.digest(manifest_path),
            release_assets.IMAGE_NAME: self.digest(self.image),
        }
        (self.directory / release_assets.CHECKSUMS_NAME).write_text(
            "".join(f"{digest}  {name}\n" for name, digest in sorted(checksums.items())),
            encoding="utf-8",
        )

    def verify(self):
        return release_assets.verify(self.directory, self.TAG, self.COMMIT)

    def assert_rejected(self, message: str):
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, message):
            self.verify()

    def test_exact_signed_release_bundle_passes(self):
        summary = self.verify()
        self.assertEqual(summary["tag"], self.TAG)
        self.assertEqual(summary["imageBytes"], self.image.stat().st_size)
        self.assertEqual(summary["imageSha256"], self.digest(self.image))

    def validation_bundle(self):
        url = f"{self.LOCAL_ORIGIN}/releases/latest"
        self.image.write_bytes(b"signed-image-fixture:" + url.encode("ascii"))
        self.manifest["buildProfile"] = "hardware-validation"
        self.manifest["image"]["sizeBytes"] = self.image.stat().st_size
        self.manifest["image"]["sha256"] = self.digest(self.image)
        self.write_bundle()

    def test_validation_requires_explicit_local_origin_and_matching_compiled_url(self):
        self.validation_bundle()
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "release profile"):
            self.verify()
        summary = release_assets.verify(
            self.directory, self.TAG, self.COMMIT, self.LOCAL_ORIGIN
        )
        self.assertEqual(summary["imageSha256"], self.digest(self.image))

        command = [
            str(FIRMWARE / "tools/check-release-assets.py"),
            str(self.directory),
            "--tag", self.TAG,
            "--commit", self.COMMIT,
        ]
        self.assertNotEqual(subprocess.run(command, capture_output=True, check=False).returncode, 0)
        self.assertEqual(
            subprocess.run(
                command + ["--local-origin", self.LOCAL_ORIGIN],
                capture_output=True, check=False,
            ).returncode,
            0,
        )

        self.image.write_bytes(self.image.read_bytes() + b":" + (
            f"https://api.github.com/repos/{release_assets.OFFICIAL_REPOSITORY}/releases/latest"
        ).encode("ascii"))
        self.manifest["image"]["sizeBytes"] = self.image.stat().st_size
        self.manifest["image"]["sha256"] = self.digest(self.image)
        self.write_bundle()
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "pinned exclusively"):
            release_assets.verify(self.directory, self.TAG, self.COMMIT, self.LOCAL_ORIGIN)

    def test_validation_rejects_wrong_origin_profile_and_dirty_tree(self):
        self.validation_bundle()
        for origin in (
            "bad", "https://api.github.com:443", "http://192.168.1.40:8443",
            "https://192.168.1.40:8443/extra", "https://192.168.1.40:0",
        ):
            with self.subTest(origin=origin), self.assertRaisesRegex(
                release_assets.ReleaseAssetError, "exact private IPv4 HTTPS origin"
            ):
                release_assets.verify(self.directory, self.TAG, self.COMMIT, origin)
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "pinned exclusively"):
            release_assets.verify(self.directory, self.TAG, self.COMMIT, "https://192.168.1.41:8443")
        self.manifest["buildProfile"] = "release"
        self.write_bundle()
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "hardware-validation profile"):
            release_assets.verify(self.directory, self.TAG, self.COMMIT, self.LOCAL_ORIGIN)
        self.manifest["buildProfile"] = "hardware-validation"
        self.manifest["dirtyTree"] = True
        self.write_bundle()
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "dirty-tree"):
            release_assets.verify(self.directory, self.TAG, self.COMMIT, self.LOCAL_ORIGIN)

    def test_intended_tag_and_commit_are_independent_inputs(self):
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "productVersion"):
            release_assets.verify(self.directory, "v1.2.4", self.COMMIT)
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "gitCommit"):
            release_assets.verify(self.directory, self.TAG, "c" * 40)

    def test_expected_tag_and_full_commit_shapes_are_rejected(self):
        for tag in ("1.2.3", "v"):
            with self.subTest(tag=tag), self.assertRaisesRegex(
                release_assets.ReleaseAssetError, "expected tag"
            ):
                release_assets.verify(self.directory, tag, self.COMMIT)
        for commit in ("a" * 39, "a" * 41, "A" * 40):
            with self.subTest(commit=commit), self.assertRaisesRegex(
                release_assets.ReleaseAssetError, "full lowercase 40-character SHA"
            ):
                release_assets.verify(self.directory, self.TAG, commit)

    def test_manifest_commit_must_use_at_least_the_production_prefix_width(self):
        for manifest_commit in (self.COMMIT[:11], self.COMMIT + "a", "A" * 12):
            with self.subTest(manifest_commit=manifest_commit):
                self.manifest["gitCommit"] = manifest_commit
                self.write_bundle()
                self.assert_rejected("SHA prefix of 12 to 40 characters")
        self.manifest["gitCommit"] = self.COMMIT[:13]
        self.write_bundle()
        self.verify()

    def test_image_content_size_and_name_are_bound(self):
        self.image.write_bytes(b"tampered")
        self.assert_rejected("size|digest")
        self.image.write_bytes(b"signed-image-fixture")
        self.manifest["image"]["name"] = "other.bin"
        self.write_bundle()
        self.assert_rejected("live OTA asset name")

    def test_same_size_content_swap_cannot_hide_a_stale_manifest_digest(self):
        changed = bytearray(self.image.read_bytes())
        changed[0] ^= 1
        self.image.write_bytes(changed)
        self.write_bundle()
        self.assertEqual(self.manifest["image"]["sizeBytes"], self.image.stat().st_size)
        self.assert_rejected("manifest image digest")

    def test_unsigned_or_wrong_key_identity_is_rejected(self):
        self.manifest["signing"]["algorithm"] = None
        self.write_bundle()
        self.assert_rejected("wrong type|required signature")
        self.manifest["signing"]["algorithm"] = "RSA-PSS-SHA256"
        self.manifest["signing"]["publicKeySpkiSha256"] = "ABC"
        self.write_bundle()
        self.assert_rejected("public-key identity")

    def test_required_signing_values_are_rejected_independently(self):
        cases = (
            ("algorithm", "ROT13", "required signature algorithm"),
            ("signature", "detached", "embedded verified signature"),
        )
        for name, value, message in cases:
            with self.subTest(name=name):
                original = self.manifest["signing"][name]
                self.manifest["signing"][name] = value
                self.write_bundle()
                self.assert_rejected(message)
                self.manifest["signing"][name] = original

    def test_debug_dirty_wrong_target_and_slot_layout_are_rejected(self):
        cases = (
            ("buildProfile", "debug", "release profile"),
            ("dirtyTree", True, "dirty-tree"),
            ("boardTarget", "other", "board target"),
            ("chipTarget", "esp32", "chip target"),
        )
        for name, value, message in cases:
            with self.subTest(name=name):
                original = self.manifest[name]
                self.manifest[name] = value
                self.write_bundle()
                self.assert_rejected(message)
                self.manifest[name] = original
        self.manifest["compatibility"]["otaSlotCount"] = 1
        self.write_bundle()
        self.assert_rejected("dual-slot")

    def test_schema_bootloader_and_slot_size_bounds_are_rejected(self):
        self.manifest["schemaVersion"] = 2
        self.write_bundle()
        self.assert_rejected("unsupported manifest schema")
        self.manifest["schemaVersion"] = 1
        self.manifest["compatibility"]["minBootloaderSchema"] = 0
        self.write_bundle()
        self.assert_rejected("minimum bootloader schema")
        self.manifest["compatibility"]["minBootloaderSchema"] = 1
        self.manifest["compatibility"]["otaSlotSizeBytes"] = self.image.stat().st_size - 1
        self.write_bundle()
        self.assert_rejected("dual-slot")

    def test_checksums_must_bind_both_live_assets(self):
        checksums = self.directory / release_assets.CHECKSUMS_NAME
        checksums.write_text(f"{'0' * 64}  {release_assets.IMAGE_NAME}\n", encoding="utf-8")
        self.assert_rejected("does not bind manifest.json|does not bind prusa_remote_display.bin")

    def test_checksum_lines_must_use_the_strict_release_format(self):
        checksums = self.directory / release_assets.CHECKSUMS_NAME
        invalid_lines = (
            f"{self.digest(self.image)} {release_assets.IMAGE_NAME}\n",
            f"{self.digest(self.image)}  nested/{release_assets.IMAGE_NAME}\n",
            "not-a-digest  manifest.json\n",
        )
        for line in invalid_lines:
            with self.subTest(line=line):
                checksums.write_text(line, encoding="utf-8")
                self.assert_rejected("invalid SHA256SUMS line")

    def test_invalid_manifest_json_and_root_are_rejected(self):
        manifest_path = self.directory / release_assets.MANIFEST_NAME
        manifest_path.write_text("{", encoding="utf-8")
        self.assert_rejected("invalid manifest JSON")
        manifest_path.write_text("[]\n", encoding="utf-8")
        self.assert_rejected("manifest root must be an object")

    def test_release_files_must_be_regular_nonempty_and_bounded(self):
        manifest_path = self.directory / release_assets.MANIFEST_NAME
        manifest_path.write_bytes(b"")
        self.assert_rejected("empty release file: manifest.json")
        manifest_path.write_bytes(b" " * (release_assets.MANIFEST_LIMIT + 1))
        self.assert_rejected("manifest.json exceeds the device limit")
        self.write_bundle()
        real_image = self.directory / "real-image.bin"
        self.image.replace(real_image)
        self.image.symlink_to(real_image)
        self.assert_rejected("missing regular file: prusa_remote_display.bin")

    def test_release_image_limit_is_checked_before_reading_content(self):
        with self.image.open("wb") as handle:
            handle.truncate(release_assets.IMAGE_LIMIT + 1)
        self.assert_rejected("prusa_remote_display.bin exceeds the device limit")

    def test_missing_artifact_directory_is_rejected(self):
        missing = self.directory / "missing"
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "not an artifact directory"):
            release_assets.verify(missing, self.TAG, self.COMMIT)

    def test_duplicate_json_members_and_checksum_entries_are_rejected(self):
        manifest_path = self.directory / release_assets.MANIFEST_NAME
        text = manifest_path.read_text(encoding="utf-8")
        manifest_path.write_text(text.replace("{", '{"schemaVersion":1,', 1), encoding="utf-8")
        with self.assertRaisesRegex(release_assets.ReleaseAssetError, "duplicate JSON member"):
            self.verify()
        self.write_bundle()
        checksums = self.directory / release_assets.CHECKSUMS_NAME
        line = f"{self.digest(self.image)}  {release_assets.IMAGE_NAME}\n"
        checksums.write_text(line + line, encoding="utf-8")
        self.assert_rejected("duplicate SHA256SUMS")


if __name__ == "__main__":
    unittest.main()
