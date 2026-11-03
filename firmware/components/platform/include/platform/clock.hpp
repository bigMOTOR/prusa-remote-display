// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>

namespace prd::platform {

// Time since boot from the ESP monotonic timer (Bootstrap Spec §6.6).
// Monotonic, never decreases, unaffected by wall-clock or SNTP changes.
//
// SAFE FROM AN INTERRUPT, INCLUDING WHILE THE FLASH CACHE IS DISABLED. The
// implementation is IRAM-resident and the ESP-IDF function underneath it is too
// (CONFIG_ESP_TIMER_IN_IRAM, on by default). The button's edge interrupt timestamps
// every edge from here, and that interrupt stays enabled during flash writes, so
// this call must not be a jump into flash.
[[nodiscard]] std::chrono::microseconds monotonic_time();

} // namespace prd::platform
