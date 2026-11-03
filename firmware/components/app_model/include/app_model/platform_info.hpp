// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace prd::app_model {

// Runtime-detected platform facts, populated by the board component from stable
// ESP-IDF APIs. Values that cannot be determined reliably are left empty or
// nullopt rather than guessed; this type carries no ESP-IDF dependency so it
// stays host-testable.
struct PlatformInfo {
    std::string chip_model; // e.g. "ESP32-S3"; empty when unrecognized
    std::uint16_t chip_revision_major{0};
    std::uint16_t chip_revision_minor{0};
    std::uint8_t cpu_cores{0};

    std::string reset_reason; // human-readable reset cause

    std::optional<std::uint32_t> flash_size_bytes; // detected physical flash size
    bool psram_present{false};
    std::optional<std::size_t> psram_size_bytes; // detected PSRAM size when present
};

} // namespace prd::app_model
