# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise the actual CMake guard; CMake required, no IDF, keys or hardware."""
from pathlib import Path
import subprocess
import unittest
import os
import shlex
import tempfile

FIRMWARE = Path(__file__).resolve().parents[2]
GUARD = FIRMWARE / 'cmake/ota_security.cmake'
REQUIRED = ('CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT',
            'CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME',
            'CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT',
            'CONFIG_SECURE_SIGNED_ON_UPDATE',
            'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE')
FORBIDDEN = ('CONFIG_SECURE_BOOT', 'CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES',
             'CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK', 'CONFIG_SECURE_FLASH_ENC_ENABLED',
             'CONFIG_NVS_ENCRYPTION')


class OtaSecurityConfigTests(unittest.TestCase):
    @staticmethod
    def ota_public_dependencies(text):
        registration = text.split('idf_component_register(', 1)[1].split(
            ')\ntarget_compile_features', 1)[0]
        tokens = shlex.split(registration)
        public = tokens[tokens.index('REQUIRES') + 1:tokens.index('PRIV_REQUIRES')]
        return set(public)

    def preprocess_adapter(self, setting, target=True):
        # Execute the real adapter's preprocessor guard. Empty SDK headers are
        # sufficient because this is not a C++ type/link or cryptographic test.
        with tempfile.TemporaryDirectory(prefix='prd-ota-config-') as directory:
            support = Path(directory)
            (support / 'mbedtls').mkdir()
            (support / 'esp_ota_ops.h').write_text('')
            (support / 'mbedtls/sha256.h').write_text('')
            (support / 'sdkconfig.h').write_text(
                '' if setting is None else f'#define CONFIG_SECURE_SIGNED_ON_UPDATE {setting}\n')
            command = [*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-E',
                       '-I', str(support), '-I', str(FIRMWARE / 'components/ota/include')]
            if target:
                command.append('-DESP_PLATFORM=1')
            return subprocess.run(
                [*command, str(FIRMWARE / 'components/ota/src/device_ingestion.cpp')],
                capture_output=True, text=True)

    def test_adapter_target_guard_accepts_enabled_signed_updates(self):
        result = self.preprocess_adapter(1)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_installation_public_header_compiles_with_only_ota_includes(self):
        # A consumer of ota does not inherit PRIV_REQUIRES app_model includes.
        # This is a real C++ type check, separate from the SDK preprocessor guard.
        source = ('#include "ota/installation.hpp"\n'
                  'static_assert(sizeof(prd::ota::Installation) > 0);\n'
                  'bool observe(prd::ota::Installation& transaction, '
                  'prd::app_model::OtaObservation& observation) {\n'
                  '  return transaction.observation(observation);\n}\n')
        environment = dict(os.environ)
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH'):
            environment.pop(name, None)
        result = subprocess.run(
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-Werror',
             '-x', 'c++', '-fsyntax-only', '-I', str(FIRMWARE / 'components/ota/include'), '-'],
            input=source, capture_output=True, text=True, env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_public_headers_compile_with_declared_public_dependencies(self):
        source = ('#include "ota/live_update.hpp"\n'
                  '#include "ota/release_client.hpp"\n'
                  'static_assert(sizeof(prd::ota::LiveReport) > 0);\n'
                  'static_assert(sizeof(prd::ota::ReleaseFetchResult) > 0);\n')
        environment = dict(os.environ)
        for name in ('CPATH', 'CPLUS_INCLUDE_PATH', 'C_INCLUDE_PATH'):
            environment.pop(name, None)
        include_roots = ('ota', 'app_model', 'network')
        command = [*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', '-Werror',
                   '-x', 'c++', '-fsyntax-only']
        for component in include_roots:
            command.extend(('-I', str(FIRMWARE / f'components/{component}/include')))
        result = subprocess.run([*command, '-'], input=source, capture_output=True, text=True,
                                env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_public_header_dependencies_are_exported_and_guard_fires(self):
        cmake = (FIRMWARE / 'components/ota/CMakeLists.txt').read_text()
        self.assertTrue({'app_model', 'network'} <= self.ota_public_dependencies(cmake))

        private_only = cmake.replace('REQUIRES app_model network\n    PRIV_REQUIRES',
                                     'REQUIRES\n    PRIV_REQUIRES app_model network')
        self.assertFalse({'app_model', 'network'} <= self.ota_public_dependencies(private_only))

    def test_adapter_target_guard_rejects_disabled_or_missing_signed_updates(self):
        for setting in (None, 0):
            with self.subTest(setting=setting):
                result = self.preprocess_adapter(setting)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('OTA device ingestion requires CONFIG_SECURE_SIGNED_ON_UPDATE',
                              result.stderr)

    def test_native_mock_does_not_claim_target_signature_configuration(self):
        result = self.preprocess_adapter(None, target=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_guard(self, change=None, omit=None):
        values = dict.fromkeys(REQUIRED, 'ON') | dict.fromkeys(FORBIDDEN, 'OFF')
        if change:
            values.update(change)
        if omit:
            values.pop(omit)
        return subprocess.run(['cmake', *(f'-D{k}={v}' for k, v in values.items()),
                               '-P', str(GUARD)], capture_output=True, text=True)

    def test_software_only_manual_signing_configuration_passes(self):
        self.assertEqual(self.run_guard().returncode, 0)

    def test_every_missing_required_setting_fails(self):
        for option in REQUIRED:
            with self.subTest(option=option):
                result = self.run_guard(omit=option)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(option, result.stderr)

    def test_every_disabled_required_setting_fails(self):
        for option in REQUIRED:
            with self.subTest(option=option):
                result = self.run_guard({option: 'OFF'})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(option, result.stderr)

    def test_every_forbidden_setting_fails(self):
        for option in FORBIDDEN:
            with self.subTest(option=option):
                result = self.run_guard({option: 'ON'})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(option, result.stderr)

    def test_absent_forbidden_settings_are_allowed(self):
        for option in FORBIDDEN:
            with self.subTest(option=option):
                self.assertEqual(self.run_guard(omit=option).returncode, 0)

    def test_guard_runs_after_idf_project_configuration(self):
        text = (FIRMWARE / 'CMakeLists.txt').read_text()
        self.assertGreater(text.index('include(${CMAKE_CURRENT_LIST_DIR}/cmake/ota_security.cmake)'),
                           text.index('project(prusa_remote_display)'))


if __name__ == '__main__':
    unittest.main()
