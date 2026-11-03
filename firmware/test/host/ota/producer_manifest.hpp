// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string_view>
// Generated from the deterministic ManifestTests fixture through build_manifest.
// test_package_manifest verifies equality against the real producer every run.
inline constexpr std::string_view kProducerManifest = R"json({
  "boardTarget": "test-board",
  "buildEpoch": "0",
  "buildProfile": "debug",
  "chipTarget": "esp32s3",
  "compatibility": {
    "minBootloaderSchema": 1,
    "otaSlotCount": 2,
    "otaSlotSizeBytes": 2097152
  },
  "dirtyTree": true,
  "gitCommit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "gitDescribe": "test-build",
  "idfVersion": "v5.5.4",
  "image": {
    "name": "application.bin",
    "sha256": "f6145b86504aa0b54096c22071ba0b5b786fc735e21d853fbe25ead5db1cecc8",
    "sizeBytes": 1280000
  },
  "productVersion": "0.1.0",
  "releaseTag": "",
  "schemaVersion": 1,
  "signing": {
    "algorithm": null,
    "note": "unsigned; OTA signature format is a deferred decision",
    "signature": null
  }
})json";
