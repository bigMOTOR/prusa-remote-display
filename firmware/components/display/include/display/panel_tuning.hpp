// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Panel tuning modes for the Milestone 1 Phase C1 colour A/B investigation.
//
// ESP-IDF's generic ST7789 driver configures no gamma and no analogue drive
// registers, leaving the ST7789T3 on power-on defaults. Waveshare ships its own
// driver (Vernon_ST7789T.c) that programs both. These modes let the two be
// compared on the physical panel.
//
// Hardware-validation only. Nothing here changes any accepted Project colour
// token; the tuning acts on the panel, not on the pixel values we send.

namespace prd::display {

enum class TuningMode {
    // ACCEPTED BOARD DEFAULT (Phase C1). Vendor gamma tables E0h/E1h only.
    // Improves the accent colour and black level with no side effects; applies
    // none of the rejected analogue/power registers.
    VendorGammaOnly,

    // A — control, hardware-validation only. esp_lcd's generic init, nothing
    // added. Leaves a dull accent and an elevated black level.
    Generic,

    // B — REJECTED, hardware-validation only. The Waveshare sequence
    // reproduced byte for byte, including their 0xD0 length-1 call that drops
    // the second parameter. Corrupts the accent colour and flips orientation.
    VendorExact,

    // D — REJECTED, hardware-validation only. Vendor analogue/power/VCOM plus
    // gamma, with 0xD0 corrected to the two parameters the ST7789T3 datasheet
    // specifies. Same corruption as mode B, so the defect is not the 0xD0
    // truncation. Excludes MADCTL, COLMOD and RAMCTRL.
    VendorAnalogueAndGamma,
};

[[nodiscard]] const char* tuning_mode_name(TuningMode mode);

} // namespace prd::display
