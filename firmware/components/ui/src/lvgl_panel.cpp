// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "lvgl_panel.hpp"

#include <cstddef>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture spec
// §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#pragma GCC diagnostic pop

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "display/display.hpp"
#include "platform/clock.hpp"

namespace prd::ui::panel {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

// Deadline for one band's transfer to report completion.
//
// A 40-row band at 320 px and the validated 40 MHz SPI clock is about 5.1 ms, so
// this is roughly a hundred times the expected time. It is a lost-completion
// detector, not a latency budget: nothing legitimate takes this long, and a
// renderer that waits forever on a notification that will never arrive is a
// deadlock rather than a diagnosable fault.
constexpr std::uint32_t kFlushDeadlineMs = 500;

// Deadline windows the wait callback will sit through before giving up. Two, so
// the worst case is one second — comfortably inside the 5 s task watchdog, which
// means a wedged bus is reported by this component rather than by a panic.
constexpr int kMaxDeadlineWaits = 2;

// Draw buffers. Raw pointers rather than a container because their addresses are
// handed to LVGL and to the SPI DMA; nothing may reallocate them.
void* g_buffers[2] = {nullptr, nullptr};

lv_display_t* g_display = nullptr;

ui::DrawBufferReport g_buffer_report{};

// --- Flush state -----------------------------------------------------------
//
// OWNERSHIP, stated exactly because this is the only place two contexts meet:
//
//   g_flush_outstanding, g_flush_started_us  — render task only.
//   g_flush_completed_us — written by the ISR before it gives the semaphore, read
//       by the render task only after taking it. The semaphore is the ordering; no
//       lock is needed and none would help.
//   g_stats, g_memory — written by the render task. The spinlock exists purely so
//       another task can read a consistent snapshot for a report.
//
// The ISR touches no LVGL state whatsoever, which is what makes the whole path
// unable to race against LVGL.
portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t g_flush_done = nullptr;

bool g_flush_outstanding = false;
std::int64_t g_flush_started_us = 0;

volatile std::int64_t g_flush_completed_us = 0;

ui::FlushStatistics g_stats{};
ui::MemoryReport g_memory{};

[[nodiscard]] std::int64_t now_us() {
    return platform::monotonic_time().count();
}

// --- Interrupt-context completion handoff ----------------------------------

// Runs in the SPI transaction-complete interrupt, via
// display::set_transfer_complete_callback().
//
// It deliberately does NOT call any LVGL function. In LVGL 9.5
// lv_display_flush_ready() happens to be a single flag store and would very
// likely be safe here, but LVGL documents no ISR-safety guarantee for any of its
// API, and an earlier 9.x release did more work in it. Handing off to the task
// costs one context switch per band — noise against a ~5 ms transfer — and buys a
// rule that does not depend on reading LVGL's implementation, or re-reading it at
// every upgrade.
//
// The timestamp is taken here rather than when the task gets round to noticing,
// so the reported flush duration is the transfer's, not the scheduler's. It comes
// from the project clock rather than esp_timer directly, so ui keeps exactly one
// notion of time; the underlying esp_timer_get_time() is ISR-safe.
void IRAM_ATTR on_transfer_complete(void* /*context*/) {
    g_flush_completed_us = now_us();

    BaseType_t higher_priority_woken = pdFALSE;
    (void)xSemaphoreGiveFromISR(g_flush_done, &higher_priority_woken);

    // The display component's callback signature cannot propagate a yield
    // request, so request it here. This is the ordinary ESP-IDF idiom: it sets a
    // flag the interrupt epilogue acts on, it does not switch context inline.
    if (higher_priority_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// --- Completion accounting (render task only) ------------------------------

void record_completion() {
    const auto elapsed = g_flush_completed_us - g_flush_started_us;
    const auto us = static_cast<std::uint32_t>(elapsed < 0 ? 0 : elapsed);

    g_flush_outstanding = false;

    portENTER_CRITICAL(&g_lock);
    ++g_stats.completed;
    g_stats.duration_last_us = us;
    g_stats.duration_total_us += us;
    if (g_stats.completed == 1 || us < g_stats.duration_min_us) {
        g_stats.duration_min_us = us;
    }
    if (us > g_stats.duration_max_us) {
        g_stats.duration_max_us = us;
    }
    portEXIT_CRITICAL(&g_lock);
}

// Consumes one already-signalled completion. Returns true if it resolved a flush.
bool take_signalled_completion() {
    if (xSemaphoreTake(g_flush_done, 0) != pdTRUE) {
        return false;
    }

    if (!g_flush_outstanding) {
        // A completion with nothing outstanding. Nothing else in this build shares
        // the panel, so this would mean the one-callback-per-blit assumption the
        // ownership contract rests on is wrong. Counted, never acted on.
        portENTER_CRITICAL(&g_lock);
        ++g_stats.spurious_signals;
        portEXIT_CRITICAL(&g_lock);
        return false;
    }

    record_completion();
    return true;
}

// --- Flush callback --------------------------------------------------------

// Called by LVGL from lv_timer_handler(), i.e. on the render task. Never from an
// interrupt. Returns immediately: the transfer is asynchronous, which is what lets
// LVGL render the next band into the other buffer while this one is in flight.
void flush_callback(lv_display_t* const display_handle, const lv_area_t* const area,
                    std::uint8_t* const px_map) {
    // display::blit() takes exclusive end coordinates; lv_area_t is inclusive.
    const int x_end = static_cast<int>(area->x2) + 1;
    const int y_end = static_cast<int>(area->y2) + 1;

    // Convert to the panel's byte order, in place and before the buffer is lent
    // out. Safe here and nowhere else: LVGL has finished rendering this band and
    // DMA does not own it yet, so this is the only window in which the bytes may be
    // touched. Timed, because the ADR rejected the alternative on cost grounds.
    const std::int64_t swap_started_us = now_us();
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    const std::int64_t swap_finished_us = now_us();

    // Marked outstanding BEFORE submission: the completion interrupt can fire
    // before blit() returns, and the wait callback must already know a flush is in
    // progress when it observes that signal.
    g_flush_outstanding = true;
    g_flush_started_us = swap_finished_us;

    portENTER_CRITICAL(&g_lock);
    ++g_stats.submitted;
    g_stats.swap_total_us += static_cast<std::uint64_t>(swap_finished_us - swap_started_us);
    portEXIT_CRITICAL(&g_lock);

    // px_map is LVGL's draw buffer. blit() borrows it until completion is
    // signalled, and LVGL will not render into it again before the wait callback
    // below has returned — which only happens once the interrupt has fired.
    if (display::blit(static_cast<int>(area->x1), static_cast<int>(area->y1), x_end, y_end,
                      reinterpret_cast<const std::uint16_t*>(px_map))) {
        return;
    }

    // Submission failed, so the buffer was never lent out. Clear LVGL's flushing
    // flag immediately: leaving it set would send LVGL into the wait callback for a
    // completion that can never arrive, turning a recoverable transfer error into a
    // stalled renderer.
    g_flush_outstanding = false;

    portENTER_CRITICAL(&g_lock);
    --g_stats.submitted;
    ++g_stats.submit_failures;
    portEXIT_CRITICAL(&g_lock);

    diagnostics::log(diagnostics::LogLevel::Error, kTag, "flush submit failed");
    lv_display_flush_ready(display_handle);
}

// --- Flush-wait callback ---------------------------------------------------

// Called by LVGL when it must not proceed until the outstanding transfer has
// finished. Runs on the render task. LVGL clears its own `flushing` flag when this
// returns, so lv_display_flush_ready() must NOT be called here.
//
// Registering this is not an optimisation. Without it LVGL spins on
// `while(disp->flushing);`, and the only code that could clear that flag is this
// very task — a deadlock that starves the idle task and trips the watchdog.
void flush_wait_callback(lv_display_t* /*display_handle*/) {
    // Already resolved by the render loop between refreshes. Normal, not an error:
    // LVGL waits before each flush, so it also asks about the last flush of the
    // previous refresh, which the loop has usually settled by now.
    if (!g_flush_outstanding) {
        (void)take_signalled_completion();
        return;
    }

    for (int attempt = 0; attempt < kMaxDeadlineWaits; ++attempt) {
        if (xSemaphoreTake(g_flush_done, pdMS_TO_TICKS(kFlushDeadlineMs)) == pdTRUE) {
            record_completion();
            portENTER_CRITICAL(&g_lock);
            ++g_stats.waits_blocked;
            portEXIT_CRITICAL(&g_lock);
            return;
        }

        portENTER_CRITICAL(&g_lock);
        ++g_stats.deadline_misses;
        portEXIT_CRITICAL(&g_lock);

        // Ask the display component whether the transfer actually finished. If it
        // did, the completion signal was lost and the buffer is genuinely free, so
        // recovering is correct. If it did not, the bus is still busy — wait
        // another window rather than release a buffer that is under DMA.
        if (display::transfers_in_flight() == 0) {
            g_flush_completed_us = now_us();
            record_completion();

            portENTER_CRITICAL(&g_lock);
            ++g_stats.recovered_flushes;
            portEXIT_CRITICAL(&g_lock);

            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "lost flush completion recovered — transfer had finished");
            return;
        }

        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "flush still in flight past deadline in_flight=" +
                             std::to_string(display::transfers_in_flight()));
    }

    // The bus has not completed a transfer in a second. Blocking further would
    // starve the watchdog and turn a bus fault into a panic with no explanation,
    // so give up, count it, and let LVGL continue. Both this counter and the
    // deadline counter must be zero for a run to pass.
    g_flush_completed_us = now_us();
    record_completion();

    portENTER_CRITICAL(&g_lock);
    ++g_stats.abandoned_flushes;
    portEXIT_CRITICAL(&g_lock);

    diagnostics::log(diagnostics::LogLevel::Error, kTag,
                     "flush abandoned after deadline windows — transfer never completed");
}

void free_buffers() {
    for (auto*& buffer : g_buffers) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
}

} // namespace

bool create(const Plan& plan) {
    if (g_display != nullptr) {
        return true;
    }

    if (!display::is_initialised()) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "display not initialised — ui::init() requires it first");
        return false;
    }

    const auto size = display::geometry();
    if (size.width_px <= 0 || size.height_px <= 0 || plan.rows <= 0 || plan.count < 1 ||
        plan.count > 2) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "invalid draw buffer plan");
        return false;
    }

    // Counting, not binary: two buffers means two transfers can be outstanding in
    // principle, and a binary semaphore would silently collapse two completions
    // into one. kMaxTransfersInFlight is the display component's own ceiling.
    if (g_flush_done == nullptr) {
        g_flush_done = xSemaphoreCreateCounting(display::kMaxTransfersInFlight, 0);
        if (g_flush_done == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag, "flush semaphore creation failed");
            return false;
        }
    }

    const auto pixels =
        static_cast<std::size_t>(size.width_px) * static_cast<std::size_t>(plan.rows);
    const std::size_t bytes = pixels * sizeof(std::uint16_t);

    // MALLOC_CAP_INTERNAL is explicit, not redundant. The board enables PSRAM
    // through ordinary malloc and its allocation threshold can change; these
    // buffers are handed straight to the SPI DMA, so they must remain internal
    // and DMA-capable regardless of the default allocator's preference.
    for (int index = 0; index < plan.count; ++index) {
        g_buffers[index] = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (g_buffers[index] == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "draw buffer allocation failed bytes=" + std::to_string(bytes));
            free_buffers();
            return false;
        }
    }

    // Placement is verified rather than assumed: a report that only echoed the
    // requested capabilities could not tell a successful internal allocation from
    // a silent fallback.
    const bool internal_dma = esp_ptr_internal(g_buffers[0]) && esp_ptr_dma_capable(g_buffers[0]) &&
                              (g_buffers[1] == nullptr || (esp_ptr_internal(g_buffers[1]) &&
                                                           esp_ptr_dma_capable(g_buffers[1])));

    g_display = lv_display_create(size.width_px, size.height_px);
    if (g_display == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "lv_display_create failed");
        free_buffers();
        return false;
    }

    // RGB565, with the byte order corrected in the flush callback.
    //
    // This panel expects big-endian colour data — established in Phase C, which is
    // why the display component's validated pattern generator byte-swaps at
    // construction. LVGL's answer is NOT a swapped display colour format:
    // lv_display_set_color_format() documents RGB565 / RGB888 / XRGB8888 /
    // ARGB8888 as the valid values and directs callers to lv_draw_sw_rgb565_swap()
    // in the flush callback for endianness. LV_COLOR_FORMAT_RGB565_SWAPPED exists
    // as an image and blend format, and setting it here renders visibly wrong
    // colours — greys and whites come out blue. Measured on hardware, not deduced.
    //
    // Set before the buffers, because set_buffers() derives the band height from
    // the colour format's stride.
    lv_display_set_color_format(g_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_display, g_buffers[0], g_buffers[1], static_cast<std::uint32_t>(bytes),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(g_display, flush_callback);

    // Both callbacks, not just the flush one. See the header: without the wait
    // callback LVGL busy-waits on its own flushing flag, which this task is the
    // only code able to clear.
    lv_display_set_flush_wait_cb(g_display, flush_wait_callback);

    // Registered after LVGL is ready to be told about completions and before any
    // pixel can be submitted, per the display component's contract. No context is
    // needed: the callback signals a semaphore rather than a named task, so it
    // stays correct if task ownership ever moves.
    display::set_transfer_complete_callback(on_transfer_complete, nullptr);

    g_buffer_report.bytes_each = static_cast<std::uint32_t>(bytes);
    g_buffer_report.rows = plan.rows;
    g_buffer_report.count = plan.count;
    g_buffer_report.internal_dma = internal_dma;

    diagnostics::log(
        internal_dma ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Warning, kTag,
        "lvgl display " + std::to_string(size.width_px) + "x" + std::to_string(size.height_px) +
            " rgb565+swap partial buffers=" + std::to_string(plan.count) + "x" +
            std::to_string(bytes) + "B rows=" + std::to_string(plan.rows) +
            " placement=" + (internal_dma ? "internal_dma" : "NOT_INTERNAL_DMA"));

    return true;
}

lv_display_t* handle() {
    return g_display;
}

void resolve_completed_flush() {
    (void)take_signalled_completion();
}

void sample_memory() {
    lv_mem_monitor_t monitor{};
    lv_mem_monitor(&monitor);

    portENTER_CRITICAL(&g_lock);
    g_memory.lvgl_total_bytes = static_cast<std::uint32_t>(monitor.total_size);
    g_memory.lvgl_free_bytes = static_cast<std::uint32_t>(monitor.free_size);
    g_memory.lvgl_used_bytes = static_cast<std::uint32_t>(monitor.total_size - monitor.free_size);
    g_memory.lvgl_max_used_bytes = static_cast<std::uint32_t>(monitor.max_used);
    g_memory.lvgl_fragmentation_percent = monitor.frag_pct;
    portEXIT_CRITICAL(&g_lock);
}

ui::FlushStatistics statistics() {
    portENTER_CRITICAL(&g_lock);
    const auto snapshot = g_stats;
    portEXIT_CRITICAL(&g_lock);
    return snapshot;
}

ui::DrawBufferReport buffers() {
    return g_buffer_report;
}

ui::MemoryReport memory() {
    portENTER_CRITICAL(&g_lock);
    const auto snapshot = g_memory;
    portEXIT_CRITICAL(&g_lock);
    return snapshot;
}

} // namespace prd::ui::panel
