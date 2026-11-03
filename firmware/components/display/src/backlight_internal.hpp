// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "board/board.hpp"

// Internal boundary between the panel driver (display.cpp) and the backlight
// driver (backlight.cpp). Both belong to the display component; display::init()
// brings the backlight up as part of panel initialisation so the panel can never
// be lit before a frame exists.
//
// Not part of the component's public interface: callers use the backlight
// functions declared in display/display.hpp.

namespace prd::display {

// Configures the LEDC timer and channel with the output disabled and duty 0.
// `startup_percent` is loaded as the requested brightness but is not applied
// until set_backlight_enabled(true).
//
// Returns true when the target declares no backlight pin, having done nothing.
[[nodiscard]] bool backlight_init(const board::DisplayPins& pins, int frequency_hz,
                                  std::uint8_t startup_percent);

// Parks the pin at the inactive level and releases the LEDC timer and channel.
void backlight_deinit();

} // namespace prd::display
