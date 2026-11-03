// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "display/panel_tuning.hpp"

// Internal interface between the tuning tables and the display adapter.
// Not a public header: only display.cpp consumes it.

namespace prd::display {

inline constexpr std::size_t kMaxTuningParams = 5;

struct TuningCommand {
    std::uint8_t command;
    std::uint8_t params[kMaxTuningParams];
    std::size_t length;
};

struct TuningSequence {
    const TuningCommand* commands;
    std::size_t count;
    // True when the sequence writes MADCTL / RAMCTRL and the caller must
    // restore our orientation and big-endian data path afterwards.
    bool overwrites_orientation_and_endianness;
};

[[nodiscard]] TuningSequence tuning_sequence(TuningMode mode);
[[nodiscard]] bool tuning_sends_gamma(TuningMode mode);

[[nodiscard]] const std::uint8_t* gamma_positive(std::size_t& length);
[[nodiscard]] const std::uint8_t* gamma_negative(std::size_t& length);

void log_tuning_command(std::uint8_t command, const std::uint8_t* params, std::size_t length);

} // namespace prd::display
