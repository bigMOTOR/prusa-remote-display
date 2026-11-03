// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

// Task stack usage reporting.
//
// One reusable call instead of ad-hoc logging per task, so every project-owned
// task reports the same fields in the same shape and the log stays greppable:
//
//   prd.app: task=prd_display_val stack_bytes=8192 free_low_bytes=4132
//            used_peak_bytes=4060 headroom=50%
//
// Each task reports its own stack, because FreeRTOS's high-water mark is a
// property of the calling task. Call it from a point the task reaches after its
// deepest work, not at start-up, where the mark has not yet been driven down.

namespace prd::diagnostics {

// Logs the calling task's stack usage at info level.
//
// `configured_bytes` is the size passed to xTaskCreate — FreeRTOS does not expose
// it, so the creator has to supply it. Passing the wrong value produces a wrong
// headroom figure, which is why the constant should be shared with the
// xTaskCreate call rather than retyped.
void log_task_stack(std::string_view task_name, std::uint32_t configured_bytes);

} // namespace prd::diagnostics
