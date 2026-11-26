// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

using TickType_t = std::uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;

template <typename Milliseconds> constexpr TickType_t pdMS_TO_TICKS(Milliseconds duration) {
    return static_cast<TickType_t>(duration);
}

inline constexpr BaseType_t pdPASS = 1;
