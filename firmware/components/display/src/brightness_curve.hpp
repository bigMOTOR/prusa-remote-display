// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "display/display.hpp"

// Perceptual brightness curve for the LCD backlight (Milestone 1 Phase D).
//
// Deliberately free of ESP-IDF so the mapping can be exercised by the host test
// suite: "0 % is off", "100 % is full scale" and "brightness never decreases as
// the percentage rises" are correctness properties of the code, not things that
// should only ever be checked by looking at the panel.

namespace prd::display {

// Entries in the percent -> duty table: 0..100 inclusive.
inline constexpr std::size_t kBrightnessTableSize = 101;

// Perceived lightness is roughly the cube root of luminance (CIE L*), while the
// luminance of a PWM-switched LED string is close to linear in duty cycle.
// Mapping percent straight onto duty therefore makes the bottom of the range
// change violently and the top barely change at all.
//
// Pre-distorting with duty = (percent/100)^2.2 inverts the perceptual curve, so
// equal percent steps look like equal brightness steps. 2.2 is the sRGB display
// gamma, the same exponent the accepted UI colour tokens are already encoded
// against, which keeps brightness control and pixel encoding on one curve
// instead of two.
inline constexpr float kBrightnessGamma = 2.2F;

// Percent expressed in 1/256ths, so a fade can move between two table entries
// without visible stepping.
inline constexpr std::uint32_t kPercentQ8One = 256;
inline constexpr std::uint32_t kMaxPercentQ8 = 100 * kPercentQ8One;

// Fills `table` (kBrightnessTableSize entries) with duty values for 0..100 %.
//
//   table[0]   == 0        - true off; the LED string is not driven at all
//   table[100] == max_duty - true full scale
//   table[1..] >= 1        - a non-zero request never silently produces
//                            darkness, however coarse the duty resolution is
//
// The result never decreases as the percentage rises. Where the duty resolution
// runs out at the bottom of the range, consecutive entries are equal rather
// than decreasing.
void build_brightness_table(std::uint32_t* table, std::uint32_t max_duty);

// Duty for a fractional percentage in Q8, linearly interpolating between the
// two neighbouring table entries. Input is clamped to [0, kMaxPercentQ8].
[[nodiscard]] std::uint32_t duty_for_percent_q8(const std::uint32_t* table,
                                                std::uint32_t percent_q8);

// ⬅️ percent_for_light_fraction() IS DECLARED IN display/display.hpp AND DEFINED
// IN THIS MODULE'S .cpp, BESIDE THE GAMMA IT INVERTS. The declaration is public
// because app_core calls it; the definition is here because kBrightnessGamma is
// here, and the two must never be able to drift apart into separate constants.

} // namespace prd::display
