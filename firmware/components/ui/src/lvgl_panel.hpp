// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// ESP-IDF and LVGL headers are not clean under the project warning policy:
// undefined macros, and inline functions in LVGL's own headers that narrow and
// change signedness. Suppressed around the includes only (per architecture spec
// §12.3), so the full policy still applies to every line of our own code — the
// alternative, relaxing these warnings for the whole translation unit, would
// stop checking our conversions in exactly the files that do pixel arithmetic.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#pragma GCC diagnostic pop

#include "ui/ui.hpp"

// INTERNAL HEADER of the ui component. It names LVGL types, so it must never be
// reachable from outside ui: it lives under src/ and is not on the public
// include path (Bootstrap Spec §5.1).
//
// This is the seam between LVGL and the display component: it owns the
// lv_display_t, the draw buffers and the flush path. It is the only place where
// an LVGL concept and a display:: call meet.
//
// OWNERSHIP: every function here must be called from the render task. The only
// code that runs elsewhere is the transfer-complete callback, which executes in
// interrupt context, touches no LVGL state at all, and is registered internally.
//
// FLUSH MODEL: LVGL is given both a flush callback and a flush-WAIT callback.
// Without the wait callback, LVGL 9.5 spins on `while(disp->flushing);` inside
// its refresh (lv_refr.c) — and since the render task is the only code that could
// ever clear that flag, the spin would be a deadlock that starves the idle task
// and trips the watchdog. The wait callback replaces that spin with a real block.
//
// LVGL calls the wait callback BEFORE each flush, not after the last one, which is
// what makes the buffers overlap: band N is submitted, band N+1 is rendered into
// the other buffer, and only then does LVGL wait for band N.

namespace prd::ui::panel {

struct Plan {
    int rows{kDefaultDrawBufferRows};
    int count{kDefaultDrawBufferCount};
};

// Creates the LVGL display, allocates the draw buffers and registers the flush
// path with the display component.
//
// Must be called from the render task, after lv_init().
[[nodiscard]] bool create(const Plan& plan);

[[nodiscard]] lv_display_t* handle();

// Consumes an already-signalled completion without blocking, and does nothing if
// none has arrived. Render task only.
//
// LVGL waits for a flush before starting the NEXT one, never after the last one,
// so when lv_timer_handler() returns the final band of a refresh may still be in
// flight. The render task calls this once per loop iteration so that accounting
// settles while the display is idle, instead of a completed transfer sitting
// unrecorded until the next redraw.
void resolve_completed_flush();

// Samples LVGL's allocator. Render task only.
void sample_memory();

[[nodiscard]] ui::FlushStatistics statistics();
[[nodiscard]] ui::DrawBufferReport buffers();
[[nodiscard]] ui::MemoryReport memory();

} // namespace prd::ui::panel
