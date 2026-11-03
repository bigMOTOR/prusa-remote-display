// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// GPIO assignments for the Waveshare ESP32-S3-LCD-2.8 (non-touch) target.
//
// Source of truth: the official Waveshare schematic for the shared PCB
// platform, ESP32-S3-Touch-LCD-2.8.pdf, SHA-256
// 837c1bd1abeab2bcc12575177ffb9c41ed58e82dfe3fc96d3fed8a21cdd73ace. It carries
// an explicit GPIO allocation table; every value below was read from that
// table and independently cross-checked against the schematic net labels.
// Full analysis: hardware/validation/waveshare-esp32-s3-lcd-2.8.md.
//
// Variant: SKU 30223, non-touch. The PCB platform and schematic are shared
// with the touch variant. Touch-related circuitry is not populated on
// SKU 30223.
//
// CLASSIFICATION of every value in this file (Bootstrap Spec §2, §17):
//
//   SCHEMATIC   - pin number confirmed by the official schematic. The
//                 allocation is authoritative; the electrical behaviour of the
//                 net has NOT been exercised on hardware yet.
//   VALIDATED   - confirmed by physical validation on the purchased unit.
//   UNVALIDATED - not yet established.
//   NOT POPULATED - net exists on the shared platform but its components are
//                 not fitted on SKU 30223.
//
// The LCD pins and the backlight pin have been driven and validated on the
// physical unit (Milestone 1 Phases C and D). Every other pin below is still
// undriven, and Bootstrap Spec §6.4 (no unvalidated GPIO driven) continues to
// hold for them — the button pins arrive in Phase E.

namespace prd::board::pins {

inline constexpr int kUnassignedPin = -1;

// ---------------------------------------------------------------------------
// LCD panel — 4-wire SPI to FPC connector L4 (0.5 mm, 18-pin).
// SCHEMATIC, and VALIDATED electrically in Phase C — the panel renders correctly
// at 40 MHz over SPI2. MISO is the exception: it is schematic-confirmed but
// deliberately never configured, so IO46 stays undriven.
//
// L4 pinout: 1 GND, 2 RST, 3 SCK, 4 RS(D/C), 5 CS, 6 SDA(MOSI), 7 SDO(MISO),
// 8 GND, 9 VDD(3V3), 10 LEDA, 11-14 LEDK, 15-18 XL/YU/XR/YD (resistive-touch
// pins, unused on this product).
// ---------------------------------------------------------------------------
inline constexpr int kLcdSpiSclk = 40;     // SCHEMATIC — LCD_SCK  -> L4.3
inline constexpr int kLcdSpiMosi = 45;     // SCHEMATIC — LCD_MOSI -> L4.6 (strapping pin)
inline constexpr int kLcdSpiMiso = 46;     // SCHEMATIC — LCD_MISO -> L4.7 (strapping pin)
inline constexpr int kLcdSpiCs = 42;       // SCHEMATIC — LCD_CS   -> L4.5
inline constexpr int kLcdDataCommand = 41; // SCHEMATIC — LCD_D/C  -> L4.4
inline constexpr int kLcdReset = 39;       // SCHEMATIC — LCD_RST  -> L4.2

// Backlight drive. SCHEMATIC: LCD_BL -> R19 (1 kOhm) -> base of T3 (S8050
// NPN). Emitter to GND, collector to VLED-; the LED anode side (VLED+) is fed
// from 3V3 through R18 (0 Ohm) and the four LEDK cathodes return through 10
// Ohm ballast resistors. A low-side NPN switch driven through a base resistor
// conducts when the base is driven high, so the control is ACTIVE HIGH.
//
// VALIDATED in Phase D: brightness rises monotonically with PWM duty, 0 % duty
// is true full-off with no residual glow, and 1 % is still visible in a dark
// room. The PWM frequency and duty resolution are firmware choices rather than
// board facts and live in the display component.
inline constexpr int kLcdBacklight = 5;               // SCHEMATIC + VALIDATED — LCD_BL
inline constexpr bool kLcdBacklightActiveHigh = true; // VALIDATED (Phase D)

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
// BOOT button (silkscreen "BOOT"), schematic Key2: IO0 with a 10 kOhm pull-up
// to 3V3; the button shorts to GND, so it reads ACTIVE LOW. IO0 is an ESP32-S3
// strapping pin but is safe as an input after boot. Debounce cap C28 is NOT
// fitted, so debouncing must be done in firmware (Phase E).
inline constexpr int kBootButton = 0; // SCHEMATIC — Key2, active low, 10k pull-up

// BAT_PWR button (silkscreen "BAT_PWR"), schematic Key1: pulls the Key_BAT net
// (IO6, 10 kOhm pull-up via R1) low through Schottky D2, so it also reads
// ACTIVE LOW. The same node gates the battery-path P-FET (Q1, AO3401); with no
// battery fitted and USB-C supplying VCC through Q2, reading it as an input is
// expected to be harmless — but that is UNVALIDATED and must be measured
// before use (Phase E).
inline constexpr int kBatteryButton = 6; // SCHEMATIC — Key_BAT, active low

// The product's single multifunction button.
//
// SELECTED by owner decision (2026-08-03): the BOOT button. kBatteryButton is
// explicitly NOT used as a product input — the same net gates the battery-path
// P-FET, so it stays documented board power-control circuitry only.
//
// VALIDATED in Phase E. IO0 was already proven as a working input by every
// esptool auto-reset in Phase B; Phase E validated it as an application input
// with firmware debounce and gesture classification.
inline constexpr int kUserButton = kBootButton;

// Electrical facts the driver needs, exposed through board::button_pins() so no
// other component has to know them.
inline constexpr bool kUserButtonActiveLow = true;            // SCHEMATIC + VALIDATED
inline constexpr bool kUserButtonHasExternalPullUp = true;    // SCHEMATIC — 10 kOhm to 3V3
inline constexpr bool kUserButtonHasHardwareDebounce = false; // SCHEMATIC — C28 not fitted

// ---------------------------------------------------------------------------
// Peripherals present on the shared platform but outside MVP scope
// (Architecture Spec §2.2). Listed so their pins are never treated as free.
// All SCHEMATIC.
// ---------------------------------------------------------------------------
inline constexpr int kI2cScl = 10;        // shared: IMU_SCL / RTC_SCL / J8 header
inline constexpr int kI2cSda = 11;        // shared: IMU_SDA / RTC_SDA / J8 header
inline constexpr int kRtcInterrupt = 9;   // RTC_INT  (PCF85063)
inline constexpr int kImuInterrupt2 = 12; // IMU_INT2 (QMI8658)
inline constexpr int kImuInterrupt1 = 13; // IMU_INT1 (QMI8658)

inline constexpr int kSdClk = 14;   // SD_SCLK
inline constexpr int kSdData2 = 15; // SD_D2  (also exposed on J8 as GPIO15)
inline constexpr int kSdMiso = 16;  // SD_MISO / SDIO_D0
inline constexpr int kSdMosi = 17;  // SD_MOSI / SDIO_CMD
inline constexpr int kSdData1 = 18; // SD_D1  (also exposed on J8 as GPIO18)
inline constexpr int kSdCs = 21;    // SD_CS  / SDIO_D3

inline constexpr int kI2sLrck = 38; // I2S_LRCK (PCM5101A audio DAC)
inline constexpr int kI2sDin = 47;  // I2S_DIN
inline constexpr int kI2sBck = 48;  // I2S_BCK

inline constexpr int kBatteryControl = 7; // BAT_Control — drives the power latch; do not drive
inline constexpr int kBatteryAdc = 8;     // BAT_ADC

inline constexpr int kUsbDMinus = 19; // USB D_N (native USB Serial/JTAG) — VALIDATED in Phase B
inline constexpr int kUsbDPlus = 20;  // USB D_P (native USB Serial/JTAG) — VALIDATED in Phase B

inline constexpr int kUartTxd = 43; // UART_TXD, exposed on J8 and the UART header
inline constexpr int kUartRxd = 44; // UART_RXD, exposed on J8 and the UART header

// ---------------------------------------------------------------------------
// Touch nets — NOT POPULATED on SKU 30223.
// These route to the 6-pin FPC connector L5 (GND, RST, INT, SDA, SCL, VDD),
// which is fitted but empty on this unit; no touch controller and no touch
// lens are present. The pins are therefore electrically idle and are the most
// readily available GPIOs on this board, subject to the stub trace to L5.
// Note IO3 is an ESP32-S3 strapping pin.
// ---------------------------------------------------------------------------
inline constexpr int kTouchSda = 1;       // NOT POPULATED — TP_SDA -> L5.4
inline constexpr int kTouchReset = 2;     // NOT POPULATED — TP_RST -> L5.2
inline constexpr int kTouchScl = 3;       // NOT POPULATED — TP_SCL -> L5.5 (strapping pin)
inline constexpr int kTouchInterrupt = 4; // NOT POPULATED — TP_INT -> L5.3

} // namespace prd::board::pins
