// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "board/board.hpp"

#include "board_config.h"
#include "board_pins.h"

namespace prd::board {

DisplayPins display_pins() {
    DisplayPins wiring;
    wiring.sclk = pins::kLcdSpiSclk;
    wiring.mosi = pins::kLcdSpiMosi;

    // The panel is driven write-only, so MISO is deliberately left
    // unconfigured. That also keeps GPIO46 — an ESP32-S3 strapping pin —
    // completely undriven by this firmware.
    wiring.miso = pins::kUnassignedPin;

    wiring.cs = pins::kLcdSpiCs;
    wiring.data_command = pins::kLcdDataCommand;
    wiring.reset = pins::kLcdReset;
    wiring.backlight = pins::kLcdBacklight;
    wiring.backlight_active_high = pins::kLcdBacklightActiveHigh;

    return wiring;
}

ButtonPins button_pins() {
    ButtonPins wiring;

    // Owner decision (2026-08-03): the MVP multifunction button is the BOOT
    // button. BAT_PWR / IO6 is deliberately not used as a product input — it
    // also gates the battery-path P-FET — and remains documented board
    // power-control circuitry only.
    wiring.gpio = pins::kUserButton;

    wiring.active_low = pins::kUserButtonActiveLow;
    wiring.has_external_pull_up = pins::kUserButtonHasExternalPullUp;

    // C28 is not fitted on this board, so all debouncing is the driver's job.
    wiring.has_hardware_debounce = pins::kUserButtonHasHardwareDebounce;

    return wiring;
}

DisplayGeometry display_geometry() {
    DisplayGeometry geometry;
    geometry.native_width_px = config::kDisplayNativeWidthPx;
    geometry.native_height_px = config::kDisplayNativeHeightPx;
    return geometry;
}

app_model::BoardIdentity identity() {
    app_model::BoardIdentity board_identity;
    board_identity.target = config::kBoardTarget;
    board_identity.model = config::kBoardModel;

    // Validation states stay Unknown at the compile-time identity level;
    // probe_hardware() populates them from detected values.
    return board_identity;
}

} // namespace prd::board
