// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Task stack measurement (Bootstrap Spec §6.6 — narrow framework-service
// wrappers). No FreeRTOS type appears here, so callers do not take on a
// FreeRTOS dependency to ask how much stack they have left.

namespace prd::platform {

// Smallest amount of stack the CALLING task has ever had free, in bytes.
//
// This is FreeRTOS's high-water mark: a running minimum since the task started,
// not an instantaneous reading, so it only has to be sampled occasionally to
// capture the deepest path the task has actually taken. It never rises.
//
// Must be called from inside the task being measured.
[[nodiscard]] std::uint32_t task_stack_free_low_water_bytes();

} // namespace prd::platform
