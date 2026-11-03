// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h FIRST, and explicitly. The CONFIG_* macro this file is gated on comes
// from there, and a #if on an undefined macro is silently false: without this include
// the whole flow below compiles to an empty stub, the image shrinks, and nothing
// warns. That happened once during Phase B and was caught only by comparing the image
// size against the committed figure.
#include "sdkconfig.h"

#include "panel_flows.hpp"

// The renderer cross-check flow — Milestone 1 Phase F, retained and narrowed in
// Milestone 2 Phase C.
//
// WHAT CHANGED IN PHASE C. This file used to bring the panel up: it created a task,
// called display::init() and ui::init(), reported the draw buffers and the heap cost,
// waited for the first flush and lit the backlight. All of that is product behaviour
// and moved into app_core's boot sequence, where Architecture Spec §15.1 steps 8 and
// 14 put it — until Phase C the display only ever came up in the hardware-validation
// profile, so debug and release built a renderer and never started it.
//
// WHAT REMAINS is the part that is genuinely a bench instrument: the independent
// cross-check between what LVGL asked for and what the panel did, and the driver-log
// quietening the continuous-redraw soak needs. Both are unchanged.
//
// ORCHESTRATION ONLY. No LVGL type, header or widget appears in this file.

#if defined(CONFIG_PRD_UI_RENDER_REPORT)

#include <cstdint>
#include <string>
#include <string_view>

// ESP-IDF headers are not -Wundef clean. Suppressed for this TU in CMakeLists.txt
// per architecture spec §12.3.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "display/display.hpp"
#include "ui/ui.hpp"

namespace prd::validation::internal {
namespace {

constexpr char kRenderReportTaskName[] = "prd_render_rep";

// 8192, carried from the Phase F figure rather than reduced on argument.
//
// This task no longer calls display::init(), which was the deep path that made 4096
// abort on hardware inside backlight_init()'s std::string log line. 4096 is very
// likely now sufficient — but "likely" is not a measurement, and this project sizes
// stacks on measured high-water marks. The task reports its own, so the figure below
// can be reduced against evidence rather than against reasoning.
constexpr std::uint32_t kRenderReportTaskStackBytes = 8192;
constexpr UBaseType_t kRenderReportTaskPriority = 4;

// Cross-check cadence. Slower than the render task's own reporting: this line exists
// to compare two independent views of the same transfers, which only needs to be
// sampled often enough to catch a divergence.
constexpr TickType_t kUiCrossCheckInterval = pdMS_TO_TICKS(30000);

// The proof that no flush completion was invented or lost.
//
// ui counts what LVGL asked for; display counts what the panel actually did. One
// blit per flush means the two must agree exactly. They are produced by different
// components through different code paths, so agreement is evidence rather than a
// tautology — and a divergence localises the fault immediately.
void log_ui_cross_check() {
    // ui first, panel second, and the order matters. Both counters only ever rise
    // and the panel's always leads, so reading the trailing one first guarantees the
    // difference below cannot come out negative from the sampling itself.
    const auto flush = ui::flush_statistics();
    const auto transfers = display::transfer_statistics();

    // Submissions must match exactly: both are counted on the render task, one
    // inside the flush callback and one inside blit(), with nothing in between.
    const bool submitted_agree = flush.submitted == transfers.submitted;

    // Completions may differ by one, and regularly do. The panel's counter is
    // incremented in the transfer interrupt; ui's is incremented when the render
    // task next runs and consumes the signal. A sample taken from this third task
    // can land between the two. One is the ceiling because only one flush is ever
    // outstanding; two would mean the wait callback had failed to serialise them.
    const std::uint32_t completion_lag =
        transfers.completed >= flush.completed ? transfers.completed - flush.completed : 0;
    const bool completed_agree = transfers.completed >= flush.completed && completion_lag <= 1;

    const bool agree = submitted_agree && completed_agree;
    const bool healthy = agree && flush.submit_failures == 0 && flush.spurious_signals == 0 &&
                         flush.recovered_flushes == 0 && flush.abandoned_flushes == 0 &&
                         transfers.spurious_completions == 0 && transfers.wait_timeouts == 0;

    diagnostics::log(healthy ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
                     diagnostics::tag::kApp,
                     std::string{"ui/display cross-check "} + (agree ? "agree" : "DIVERGED") +
                         " ui_submitted=" + std::to_string(flush.submitted) +
                         " ui_completed=" + std::to_string(flush.completed) +
                         " panel_submitted=" + std::to_string(transfers.submitted) +
                         " panel_completed=" + std::to_string(transfers.completed) +
                         " completion_lag=" + std::to_string(completion_lag) +
                         " in_flight=" + std::to_string(display::transfers_in_flight()) +
                         " peak_in_flight=" + std::to_string(transfers.peak_in_flight) +
                         " panel_spurious=" + std::to_string(transfers.spurious_completions) +
                         " panel_wait_timeouts=" + std::to_string(transfers.wait_timeouts));
}

// ESP-IDF driver tags whose verbose output has to be quietened for this flow.
//
// The hardware-validation profile logs at verbose, which was fine for every earlier
// phase: they redrew the panel every few seconds. The renderer redraws continuously,
// and at that rate esp_driver_spi and spi_bus_lock emit roughly 570 lines a second —
// 99 % of the serial output, enough to bury the measurement and, over USB CDC, to
// perturb the thing being measured.
//
// Raised to warning rather than silenced: a genuine SPI fault still reports. Every
// project tag stays verbose.
//
// Measured effect: this removes every ESP_LOGV/ESP_LOGD line from both tags and
// takes the rate to roughly 68 lines a second. What remains is one line per transfer
// from spi_master.c's ESP_EARLY_LOGV — the ESP_EARLY_LOG* family is compile-time
// only and ignores runtime tag levels by design, so it can be removed only by
// lowering CONFIG_LOG_MAXIMUM_LEVEL for the whole profile. That is an accepted
// profile setting from an earlier phase and is deliberately left alone; the
// committed evidence log is filtered instead.
constexpr std::string_view kNoisyDriverTags[] = {"spi_master", "bus_lock"};

void quieten_driver_logging() {
    for (const auto tag : kNoisyDriverTags) {
        diagnostics::set_external_tag_level(tag, diagnostics::LogLevel::Warning);
    }

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "raised esp_driver_spi log level to warning for the render soak");
}

void render_report_task(void* /*argument*/) {
    quieten_driver_logging();

    while (true) {
        // Reported before the delay so the first cross-check appears promptly rather
        // than half a minute into the run.
        log_ui_cross_check();
        diagnostics::log_task_stack(kRenderReportTaskName, kRenderReportTaskStackBytes);
        vTaskDelay(kUiCrossCheckInterval);
    }
}

} // namespace

void start_render_report() {
    const BaseType_t created =
        xTaskCreate(render_report_task, kRenderReportTaskName, kRenderReportTaskStackBytes, nullptr,
                    kRenderReportTaskPriority, nullptr);

    if (created != pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to create render report task");
    }
}

bool render_report_compiled() {
    return true;
}

} // namespace prd::validation::internal

#else // the render report is not compiled

namespace prd::validation::internal {

void start_render_report() {}

bool render_report_compiled() {
    return false;
}

} // namespace prd::validation::internal

#endif
