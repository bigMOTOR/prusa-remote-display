// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Compile-time configuration for the Waveshare ESP32-S3-LCD-2.8 (non-touch)
// reference target (Prototype Hardware Selection v0.1; Bootstrap Spec §6.4).
// Values marked BOARD VALIDATION REQUIRED are provisional until exercised on
// the physical unit and recorded in
// hardware/validation/waveshare-esp32-s3-lcd-2.8.md (Bootstrap Spec §2, §17).

namespace prd::board::config {

inline constexpr char kBoardTarget[] = "waveshare_esp32_s3_lcd_2_8";
inline constexpr char kBoardModel[] = "Waveshare ESP32-S3-LCD-2.8";

// The board remains a provisional reference platform until the Bootstrap
// Spec §19.8 bring-up and the Prototype Hardware Selection acceptance
// criteria pass on the physical unit.
inline constexpr char kValidationStatus[] = "provisional";

// BOARD VALIDATION REQUIRED — FLASH CAPACITY (Bootstrap Spec §8.3, §17).
inline constexpr std::uint64_t kExpectedFlashBytes = 16ULL * 1024ULL * 1024ULL;

// BOARD VALIDATION REQUIRED — PSRAM CAPACITY (Bootstrap Spec §8.3, §17).
inline constexpr std::uint64_t kExpectedPsramBytes = 8ULL * 1024ULL * 1024ULL;

// BOARD VALIDATION REQUIRED — USB CONSOLE (Bootstrap Spec §4.7, §8.3):
// assumed native USB Serial/JTAG on the exposed USB-C port; unvalidated until
// flashing and serial logs are exercised on the physical unit.
inline constexpr char kUsbConsole[] = "native_usb_serial_jtag";

// DISPLAY. Declarative only; no display driver exists yet (Phase C).
//
// Controller: vendor documentation for SKU 30223 links an ST7789T3 datasheet.
// That is documentation evidence, not schematic or physical evidence — the
// controller sits on the panel FPC and is not drawn in the board schematic —
// so it stays BOARD VALIDATION REQUIRED until a real initialisation succeeds.
inline constexpr char kDisplayController[] = "st7789t3-documented";

// Interface: 4-wire SPI. Confirmed by the schematic's LCD connector L4, whose
// pinout is RST / SCK / RS(D/C) / CS / SDA / SDO.
inline constexpr char kDisplayInterface[] = "spi-4wire";

// BOARD VALIDATION REQUIRED — panel geometry. 240x320 portrait-native per
// Prototype Hardware Selection v0.1 and the PCB silkscreen ("240x320 Pixels");
// the landscape transform and any offsets are unvalidated until a test
// pattern is rendered (Phase C).
inline constexpr int kDisplayNativeWidthPx = 240;
inline constexpr int kDisplayNativeHeightPx = 320;

// The ESP32-S3 SPI host is a firmware choice, not a board fact: none of the
// LCD pins are dedicated FSPI pads, so the signals route through the GPIO
// matrix and any general-purpose host can drive them. Selected in Phase C.

} // namespace prd::board::config
