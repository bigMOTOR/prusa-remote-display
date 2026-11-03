// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "board/board.hpp"

#include <cstdint>
#include <optional>

#include "board_config.h"

// This TU is the ESP-IDF hardware-adapter boundary. The ESP-IDF C headers below
// are not clean under the project's strict flags (undefined macros -> -Wundef;
// anonymous structs and #include_next -> -Wpedantic). -Wundef/-Wpedantic are
// relaxed for this single file in CMakeLists.txt (see the note there) per
// architecture spec §12.3; all other project warnings still apply here.
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"

namespace prd::board {
namespace {

const char* chip_model_name(const esp_chip_model_t model) {
    switch (model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C2:
        return "ESP32-C2";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    case CHIP_ESP32P4:
        return "ESP32-P4";
    default:
        return ""; // unrecognized: reported as unavailable
    }
}

const char* reset_reason_name(const esp_reset_reason_t reason) {
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external-pin";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "other-watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep-wake";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    case ESP_RST_USB:
        return "usb";
    case ESP_RST_JTAG:
        return "jtag";
    case ESP_RST_EFUSE:
        return "efuse-error";
    case ESP_RST_PWR_GLITCH:
        return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
        return "cpu-lockup";
    case ESP_RST_UNKNOWN:
        return "unknown";
    default:
        return "unknown";
    }
}

} // namespace

app_model::PlatformInfo platform_info() {
    app_model::PlatformInfo info;

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    info.chip_model = chip_model_name(chip.model);
    info.chip_revision_major = static_cast<std::uint16_t>(chip.revision / 100);
    info.chip_revision_minor = static_cast<std::uint16_t>(chip.revision % 100);
    info.cpu_cores = chip.cores;

    info.reset_reason = reset_reason_name(esp_reset_reason());

    std::uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(esp_flash_default_chip, &flash_size) == ESP_OK) {
        info.flash_size_bytes = flash_size;
    }

    info.psram_present = esp_psram_is_initialized();
    if (info.psram_present) {
        info.psram_size_bytes = esp_psram_get_size();
    }

    return info;
}

HardwareProbe probe_hardware() {
    HardwareProbe probe;
    probe.platform = platform_info();

    std::optional<std::uint64_t> flash_detected;
    if (probe.platform.flash_size_bytes.has_value()) {
        flash_detected = *probe.platform.flash_size_bytes;
    }
    probe.flash = app_model::check_capacity("flash", config::kExpectedFlashBytes, flash_detected);

    std::optional<std::uint64_t> psram_detected;
    if (probe.platform.psram_present) {
        if (probe.platform.psram_size_bytes.has_value()) {
            psram_detected = *probe.platform.psram_size_bytes;
        }
    } else {
        // PSRAM reported absent is a definitive detection result, not a
        // detection failure: compare 0 bytes against the expectation.
        psram_detected = 0;
    }
    probe.psram = app_model::check_capacity("psram", config::kExpectedPsramBytes, psram_detected);

    probe.identity = identity();
    probe.identity.flash_validation = probe.flash.result;
    probe.identity.psram_validation = probe.psram.result;

    // Compile-time console assumption; only a human observing USB-C flashing
    // and logs can validate it (Bootstrap Spec §4.7), so it reports
    // unvalidated until the Phase B record says otherwise.
    probe.usb_console = config::kUsbConsole;
    probe.usb_console_result = app_model::ValidationState::Unknown;

    probe.validation_status = config::kValidationStatus;

    return probe;
}

} // namespace prd::board
