// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>

#include "app_model/board_identity.hpp"
#include "app_model/hardware_check.hpp"
#include "app_model/platform_info.hpp"

namespace prd::board {

// Compile-time identity of the selected board target. Validation states are
// Unknown here; probe_hardware() populates them from detected values.
app_model::BoardIdentity identity();

// Runtime-detected platform facts (chip, reset reason, flash, PSRAM) queried
// from stable ESP-IDF APIs. Hardware/platform queries live here, not in the
// application layer.
app_model::PlatformInfo platform_info();

// Display wiring for the selected board target. Pin knowledge stays inside
// board (Architecture Spec §24.13); the display component consumes this
// accessor instead of the private target headers.
//
// All values are confirmed by the official schematic (Phase C0); their
// electrical behaviour is validated in Milestone 1 Phases C/D. See
// hardware/validation/waveshare-esp32-s3-lcd-2.8.md.
struct DisplayPins {
    int sclk{-1};
    int mosi{-1};
    int miso{-1}; // panel SDO; -1 when the panel is driven write-only
    int cs{-1};
    int data_command{-1};
    int reset{-1};
    int backlight{-1};
    bool backlight_active_high{true};
};

DisplayPins display_pins();

// Native panel geometry, before any rotation transform.
struct DisplayGeometry {
    int native_width_px{0};
    int native_height_px{0};
};

DisplayGeometry display_geometry();

// Multifunction button wiring for the selected board target. As with the
// display, pin knowledge stays inside board (Architecture Spec §24.13): the
// button component consumes this accessor and contains no GPIO number and no
// polarity assumption of its own.
//
// The electrical facts travel with the pin because the driver's behaviour
// depends on them — whether to enable an internal pull-up, and whether firmware
// debouncing is required at all.
struct ButtonPins {
    int gpio{-1};

    // True when the switch pulls the net towards ground, so a low level means
    // pressed.
    bool active_low{true};

    // True when the board already provides a pull resistor on the net. The
    // driver then leaves the internal one disabled.
    bool has_external_pull_up{false};

    // True when the board provides an RC debounce network. False means every bit
    // of debouncing has to happen in firmware.
    bool has_hardware_debounce{false};
};

ButtonPins button_pins();

// Expected-vs-detected hardware validation snapshot (Bootstrap Spec §15
// items 5-9): platform facts plus per-capacity pass/fail/unvalidated results
// and the board identity with its validation states populated.
struct HardwareProbe {
    app_model::BoardIdentity identity;
    app_model::PlatformInfo platform;
    app_model::CapacityCheck flash;
    app_model::CapacityCheck psram;
    std::string usb_console;
    app_model::ValidationState usb_console_result{app_model::ValidationState::Unknown};
    std::string validation_status;
};

HardwareProbe probe_hardware();

} // namespace prd::board
