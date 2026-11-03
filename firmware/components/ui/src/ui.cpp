// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly. This translation unit has CONFIG_PRD_UI_*
// conditionals, and in Milestone 2 Phase B five gated files silently compiled to
// nothing because every #if sat above the include that would have defined its macro.
// The build was clean and the image was 460 kB short. Never rely on another header
// to pull this in.
#include "sdkconfig.h"

#include "ui/ui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture spec
// §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#pragma GCC diagnostic pop

#include "controller.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "display/display.hpp"
#include "fonts/fonts.hpp"
#include "lvgl_panel.hpp"
#include "setup_session_mailbox.hpp"
#if defined(CONFIG_PRD_BOARD_WAVESHARE_ESP32_S3_LCD_2_8)
#include "lvgl_pool.hpp"
#endif
#include "platform/clock.hpp"
#include "platform/memory.hpp"

#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
#include "diagnostic_overlay.hpp"
#include "validation_scene.hpp"
#endif

// The render task (Architecture Spec §10.3 `ui_task`).
//
// HARD RULE, from Architecture Spec §1 decision 6 and §10.3: only this task
// calls LVGL. That includes lv_init() itself, which is why init() below creates
// the task and then waits for it rather than initialising LVGL on the caller's
// task. Nothing else in the firmware links against LVGL, so the rule is enforced
// by the build as well as by convention.

namespace prd::ui {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kUi;

constexpr char kTaskName[] = "prd_ui";

// Longest the task will block when LVGL has nothing pending. It bounds how
// quickly a lost flush completion is noticed, so it is a responsiveness floor
// rather than a refresh rate: LVGL's own timers decide when to redraw.
constexpr std::uint32_t kIdleWaitMs = 100;

// Loop cadence while a backlight ramp is running — about 125 steps across a two-second fade.
constexpr std::uint32_t kRampStepMs = 16;

// init() waits this long for the task to report its LVGL setup outcome. Setup is
// a handful of allocations and a scene build; a timeout here means the task never
// ran, which is a failure to report rather than a delay to absorb.
constexpr std::uint32_t kSetupTimeoutMs = 5000;

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
// Measurement cadence for the soak. Frequent enough to show a heap or LVGL-pool
// trend over half an hour, sparse enough that the serial log stays readable
// alongside the button flow.
constexpr std::int64_t kReportIntervalUs = 15 * 1000 * 1000;
#endif

TaskHandle_t g_task = nullptr;
SemaphoreHandle_t g_setup_done = nullptr;
bool g_setup_ok = false;
bool g_running = false;
Config g_config{};

std::string g_version{};

bool g_tabular_ok = false;

// The §10.7 presentation queue: one element, overwritten. Created before the task so
// a submit() that races start-up is counted rather than lost to a null handle.
//
// Never deleted. The Milestone 2 Phase B button work established why: FreeRTOS cannot
// tell you whether a reader is blocked inside a receive, so there is no safe ordering
// for deleting a queue a task may be waiting on. Keeping it alive removes the hazard
// by construction, and the cost is one 412-byte element for the process lifetime.
struct PresentationEnvelope {
    app_model::Presentation presentation;
    std::uint32_t revision{0};
};

QueueHandle_t g_presentations = nullptr;
std::atomic<std::uint32_t> g_last_flushed_revision{0};
std::uint32_t g_pending_revision{0};
std::uint32_t g_pending_flush_floor{0};

// The provisioning session's own one-deep mailbox — Milestone 3.6.
//
// A SEPARATE QUEUE, NOT A FIELD ON Presentation. §24.3 keeps secrets out of AppState, and
// Presentation is projected from AppState; this is what keeps that true while still
// getting a password onto a panel. Its payload never reaches the store, a log or a
// snapshot.
//
// A sentinel rather than a second queue for the clear: an empty session IS the cleared
// state, and one channel cannot deliver the two out of order.
QueueHandle_t g_setup_sessions = nullptr;
SemaphoreHandle_t g_setup_sessions_lock = nullptr;
QueueHandle_t g_display_intents = nullptr;

struct SetupMailboxOps {
    using Payload = SetupSession;

    bool try_lock() const {
        return xSemaphoreTake(g_setup_sessions_lock, 0) == pdTRUE;
    }
    void lock() const {
        (void)xSemaphoreTake(g_setup_sessions_lock, portMAX_DELAY);
    }
    void unlock() const {
        xSemaphoreGive(g_setup_sessions_lock);
    }
    bool try_receive(Payload& session) const {
        return xQueueReceive(g_setup_sessions, &session, 0) == pdTRUE;
    }
    void overwrite(const Payload& session) const {
        (void)xQueueOverwrite(g_setup_sessions, &session);
    }
    void reset() const {
        (void)xQueueReset(g_setup_sessions);
    }
};

#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)

// --- Owner-paced brightness harness label channel ---------------------------
//
// ⬅️ THE QUEUE STAYS HERE; THE ARTWORK DOES NOT. This is the task boundary — the harness posts from
// the button task and only the render task may touch LVGL — so the queue and its drain belong with
// every other channel in this file. The surface itself lives in diagnostic_overlay.cpp, which is
// bench instrumentation rather than product code and is excluded from the token gate on exactly
// that ground. See that file's header for the full reasoning.

QueueHandle_t g_diagnostic_labels = nullptr;

void drain_diagnostic_label() {
    if (g_diagnostic_labels == nullptr) {
        return;
    }

    DiagnosticLabel label;
    bool received = false;

    while (xQueueReceive(g_diagnostic_labels, &label, 0) == pdTRUE) {
        received = true;
    }

    if (!received) {
        return;
    }

    // A no-op until the overlay has been built — diagnostic_overlay::update() checks that itself,
    // so the drain does not have to know whether the surface exists yet.
    diagnostic_overlay::update(label.id, label.label);
}

#endif // CONFIG_PRD_UI_DIAGNOSTIC_SCENE

// The backlight ramp, owned entirely by the render task — see DisplayIntent in ui.hpp for why it
// is stepped rather than delegated to display::fade_brightness_to().
struct BacklightRamp {
    std::uint8_t from_percent{100};
    std::uint8_t to_percent{100};
    std::int64_t start_us{0};
    std::int64_t duration_us{0};
    bool active{false};
};

BacklightRamp g_ramp{};

IntakeStatistics g_intake{};

// --- LVGL glue -------------------------------------------------------------

// LVGL's time base. Taken from the project's monotonic clock rather than a
// periodic lv_tick_inc() timer: there is then no second timer to keep in step
// with, and no tick drift if the render task is ever delayed.
std::uint32_t lvgl_tick_ms() {
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(platform::monotonic_time());
    return static_cast<std::uint32_t>(milliseconds.count());
}

void lvgl_log_cb(const lv_log_level_t level, const char* const text) {
    if (text == nullptr) {
        return;
    }

    // LVGL terminates its lines; the project logger adds its own.
    std::string_view message{text};
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.remove_suffix(1);
    }

    auto mapped = diagnostics::LogLevel::Debug;
    switch (level) {
    case LV_LOG_LEVEL_ERROR:
        mapped = diagnostics::LogLevel::Error;
        break;
    case LV_LOG_LEVEL_WARN:
        mapped = diagnostics::LogLevel::Warning;
        break;
    case LV_LOG_LEVEL_USER:
    case LV_LOG_LEVEL_INFO:
        mapped = diagnostics::LogLevel::Info;
        break;
    default:
        break;
    }

    diagnostics::log(mapped, kTag, message);
}

// --- Task ------------------------------------------------------------------

bool setup() {
#if defined(CONFIG_PRD_BOARD_WAVESHARE_ESP32_S3_LCD_2_8)
    if (!pool::reserve()) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "LVGL PSRAM pool reservation failed");
        return false;
    }
    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "LVGL pool reserved in PSRAM bytes=" + std::to_string(LV_MEM_SIZE));
#endif
    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);
    lv_log_register_print_cb(lvgl_log_cb);

    g_version = std::to_string(LVGL_VERSION_MAJOR) + "." + std::to_string(LVGL_VERSION_MINOR) +
                "." + std::to_string(LVGL_VERSION_PATCH);

    const panel::Plan plan{g_config.draw_buffer_rows, g_config.draw_buffer_count};
    if (!panel::create(plan)) {
        return false;
    }

    // Production UI §4's tabular-figure requirement, measured rather than asserted.
    // Reported before the scene is built so the figures are in the log even if the
    // build below fails, and non-fatal: proportional digits are a visual defect, not
    // a reason to run with no display at all.
    g_tabular_ok = fonts::verify();

    // COMPILE-TIME MUTUAL EXCLUSION. Exactly one of these owns the active screen, and
    // the choice is made by the build rather than at run time — two scene builders
    // that both call lv_obj_remove_style_all() on the same screen would interleave,
    // and no run-time precedence rule can be checked by a test that does not run.
#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    if (!validation::build(panel::handle())) {
        return false;
    }

    // Opaque, full-bleed, and built last so it owns what is visible. See diagnostic_overlay.hpp.
    if (!diagnostic_overlay::build()) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "diagnostic overlay failed");
        return false;
    }
#else
    if (!controller::build(panel::handle())) {
        return false;
    }
#endif

    panel::sample_memory();

    diagnostics::log(diagnostics::LogLevel::Info, kTag, "lvgl " + g_version + " ready");
    return true;
}

// Blocks up to `wait_ms` for a presentation, then applies the newest one available.
//
// Doubles as the loop's delay: the render task has to yield for `wait_ms` anyway, and
// spending that time blocked on the queue rather than in vTaskDelay is what makes a
// state change visible immediately instead of on the next tick.
//
// The trailing drain matters. A submission can arrive while the previous frame is
// being flushed, and the queue overwrites — so more than one may have been counted as
// submitted while only the last is present. Draining without waiting leaves the loop
// holding the newest, never a backlog it has already been told is out of date.
// Takes whatever session is waiting, if any, and hands it to the composition.
//
// Drained without blocking and BEFORE the presentation wait, so a session that arrives
// while no presentation follows still reaches the panel. Draining to empty rather than
// once, for the same reason the presentation queue is: the mailbox overwrites, so more
// than one may have been submitted while only the last is present.
void drain_setup_session() {
    if (g_setup_sessions == nullptr || g_setup_sessions_lock == nullptr) {
        return;
    }

    SetupMailboxOps ops;
    setup_session_mailbox::drain(ops, [](const SetupSession& session) {
#if !defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
        if (session.ssid.empty() && session.password.empty()) {
            controller::clear_setup_session();
        } else {
            controller::set_setup_session(session);
        }
#else
        (void)session;
#endif
    });
}

// Takes the newest intent, if any, and arms a ramp toward it.
//
// Drained to empty rather than once, for the same reason the two mailboxes above are: the queue
// overwrites, so several may have been posted while only the last is present.
void drain_display_intent() {
    if (g_display_intents == nullptr) {
        return;
    }

    DisplayIntent intent;
    bool received = false;

    while (xQueueReceive(g_display_intents, &intent, 0) == pdTRUE) {
        received = true;
    }

    if (!received) {
        return;
    }

    const std::uint8_t current = display::brightness_percent();

    if (intent.target_percent == current) {
        g_ramp.active = false;
        return;
    }

    // ⬅️ FROM WHERE THE PANEL ACTUALLY IS, NOT FROM WHERE A PREVIOUS RAMP WAS HEADED. An intent
    // arriving mid-ramp starts from the brightness now on the panel, so a wake during a fade-down
    // rises from the level the user can see instead of jumping first.
    g_ramp.from_percent = current;
    g_ramp.to_percent = intent.target_percent;
    g_ramp.start_us = platform::monotonic_time().count();
    g_ramp.duration_us = static_cast<std::int64_t>(intent.fade_ms) * 1000;
    g_ramp.active = true;

    if (g_ramp.duration_us <= 0) {
        display::set_brightness_percent(g_ramp.to_percent);
        g_ramp.active = false;
    }
}

// Advances the ramp by one loop iteration. Returns true while one is still running, so the loop
// can shorten its wait and keep the ramp smooth.
//
// Interpolated in PERCENT space on purpose: display::set_brightness_percent() already maps percent
// through the perceptual gamma curve, so equal percent steps are equal apparent steps and the ramp
// needs no curve of its own.
[[nodiscard]] bool step_backlight() {
    if (!g_ramp.active) {
        return false;
    }

    const std::int64_t elapsed = platform::monotonic_time().count() - g_ramp.start_us;

    if (elapsed >= g_ramp.duration_us || elapsed < 0) {
        display::set_brightness_percent(g_ramp.to_percent);
        g_ramp.active = false;
        return false;
    }

    const std::int32_t span = static_cast<std::int32_t>(g_ramp.to_percent) -
                              static_cast<std::int32_t>(g_ramp.from_percent);
    const std::int32_t moved =
        static_cast<std::int32_t>((static_cast<std::int64_t>(span) * elapsed) / g_ramp.duration_us);

    display::set_brightness_percent(
        static_cast<std::uint8_t>(static_cast<std::int32_t>(g_ramp.from_percent) + moved));

    return true;
}

void wait_and_apply(const std::uint32_t wait_ms) {
    drain_setup_session();

    PresentationEnvelope envelope;

    if (xQueueReceive(g_presentations, &envelope, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return;
    }

    ++g_intake.received;

    while (xQueueReceive(g_presentations, &envelope, 0) == pdTRUE) {
        ++g_intake.received;
    }

#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    // The diagnostic scene owns the screen in this build, so a presentation has
    // nowhere to be drawn. It is still received and counted rather than left in the
    // queue, which would make submit() report supersessions that never happened.
    (void)envelope;
#else
    controller::apply(envelope.presentation);
    g_pending_revision = envelope.revision;
    g_pending_flush_floor = panel::statistics().completed;
#endif
}

#if defined(CONFIG_PRD_UI_RENDER_REPORT)

void log_report() {
    const auto flush = panel::statistics();
    const auto memory = panel::memory();
    const auto heap = platform::heap_report();

    // Every failure counter must read zero. Logged at error level when one does
    // not, so a soak cannot pass while quietly losing or inventing completions.
    //
    // The submitted/completed invariant is a difference of at most one, not
    // equality: LVGL waits before each flush rather than after the last one, so a
    // sample taken while the final band of a refresh is still on the wire will
    // legitimately see one outstanding. More than one would mean two buffers were
    // lent out at once, which the wait callback exists to prevent.
    const std::uint32_t outstanding = flush.submitted - flush.completed;
    const bool healthy = flush.submit_failures == 0 && flush.spurious_signals == 0 &&
                         flush.recovered_flushes == 0 && flush.deadline_misses == 0 &&
                         flush.abandoned_flushes == 0 && outstanding <= 1;

    const std::uint32_t mean_us =
        flush.completed == 0
            ? 0
            : static_cast<std::uint32_t>(flush.duration_total_us / flush.completed);

    const std::uint32_t swap_mean_us =
        flush.submitted == 0 ? 0
                             : static_cast<std::uint32_t>(flush.swap_total_us / flush.submitted);

    diagnostics::log(healthy ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error, kTag,
                     "flush submitted=" + std::to_string(flush.submitted) +
                         " completed=" + std::to_string(flush.completed) +
                         " outstanding=" + std::to_string(outstanding) +
                         " waits_blocked=" + std::to_string(flush.waits_blocked) +
                         " failures=" + std::to_string(flush.submit_failures) +
                         " spurious=" + std::to_string(flush.spurious_signals) +
                         " recovered=" + std::to_string(flush.recovered_flushes) +
                         " deadline_misses=" + std::to_string(flush.deadline_misses) +
                         " abandoned=" + std::to_string(flush.abandoned_flushes) +
                         " us_min=" + std::to_string(flush.duration_min_us) +
                         " us_mean=" + std::to_string(mean_us) +
                         " us_max=" + std::to_string(flush.duration_max_us) +
                         " swap_us_mean=" + std::to_string(swap_mean_us));

    diagnostics::log(
        diagnostics::LogLevel::Info, kTag,
        "memory lvgl_used=" + std::to_string(memory.lvgl_used_bytes) + "/" +
            std::to_string(memory.lvgl_total_bytes) +
            " lvgl_peak=" + std::to_string(memory.lvgl_max_used_bytes) +
            " lvgl_frag=" + std::to_string(memory.lvgl_fragmentation_percent) +
            "% internal_free=" + std::to_string(heap.free_internal_bytes) +
            " internal_min=" + std::to_string(heap.minimum_free_internal_bytes) +
            " internal_largest=" + std::to_string(heap.largest_free_internal_block_bytes) +
            " psram_free=" + std::to_string(heap.free_psram_bytes));

    // Presentation intake and scene resolution. Kept in the same report as the flush
    // figures so one log line pair says both what the renderer was asked to draw and
    // what it cost.
    const auto scene = scene_report();
    diagnostics::log(
        diagnostics::LogLevel::Info, kTag,
        "scene=" + std::string{scene.scene} + " applied=" + std::to_string(scene.applied) +
            " redundant=" + std::to_string(scene.redundant) +
            " changes=" + std::to_string(scene.scene_changes) +
            " operational=" + std::to_string(scene.operational) +
            " unimplemented=" + std::to_string(scene.unimplemented) + " submitted=" +
            std::to_string(g_intake.submitted) + " received=" + std::to_string(g_intake.received) +
            " superseded=" + std::to_string(g_intake.superseded) + " rejected=" +
            std::to_string(g_intake.rejected) + " tabular=" + (g_tabular_ok ? "yes" : "NO"));

    diagnostics::log_task_stack(kTaskName, g_config.task_stack_bytes);
}

#endif // CONFIG_PRD_UI_RENDER_REPORT

void run_loop() {
#if defined(CONFIG_PRD_UI_RENDER_REPORT)
    std::int64_t next_report_us = platform::monotonic_time().count();
#endif

    while (true) {
        // Runs LVGL's timers and, when something is dirty, renders and flushes.
        // Only ever called from here. Transfer completions are consumed inside
        // LVGL's flush-wait callback, which runs within this call — so by the time
        // it returns, every band but possibly the last has been accounted for.
        const std::uint32_t next_ms = lv_timer_handler();

        // Settles the last band of the refresh. LVGL waits before a flush, never
        // after the final one, so without this a completed transfer would sit
        // unrecorded until the next redraw and the counters would read one short.
        panel::resolve_completed_flush();

        if (g_pending_revision != 0 && panel::statistics().completed > g_pending_flush_floor) {
            g_last_flushed_revision.store(g_pending_revision, std::memory_order_release);
            g_pending_revision = 0;
        }

        drain_display_intent();

#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
        drain_diagnostic_label();
#endif

        std::uint32_t wait_ms =
            (next_ms == LV_NO_TIMER_READY) ? kIdleWaitMs : std::min(next_ms, kIdleWaitMs);

        // ⬅️ A RAMP TIGHTENS THE LOOP, AND ONLY WHILE IT RUNS. At the 100 ms idle wait a two-second
        // fade would land in twenty steps and band visibly. This is not a refresh rate: the loop
        // returns to its ordinary wait the moment the ramp ends, so a resting device is never spun
        // up to animate anything.
        if (step_backlight()) {
            wait_ms = std::min<std::uint32_t>(wait_ms, kRampStepMs);
        }

        // ⬅️ AND SO DOES THE ACKNOWLEDGEMENT'S MOTION, ON EXACTLY THE SAME TERMS. Two seconds of
        // travel at the 100 ms idle wait would land in twenty frames and read as a slide show. It
        // returns false the moment the form has settled — which is the whole of the motion, because
        // there is no pulse and no ambient loop to keep running — so the loop returns to its
        // ordinary wait with the composition still on the panel.
        // ⬅️ THE VISIBILITY SIGNAL, EMITTED AFTER THE BACKLIGHT HAS BEEN APPLIED FOR THIS TICK.
        // Read from the display component's own applied brightness — the same seam
        // drain_display_intent() consults — so there is no second source of truth. Both halves
        // matter: the percentage is REMEMBERED while the output is disabled, so a non-zero
        // percentage alone does not mean light is reaching the glass.
        controller::note_panel_visible(display::backlight_enabled() &&
                                       display::brightness_percent() > 0);

        if (controller::step_acknowledgement()) {
            wait_ms = std::min<std::uint32_t>(wait_ms, kRampStepMs);
        }

        // Never zero: a zero-tick delay is a spin, and this task must not consume
        // the core the idle task needs to feed the watchdog.
        wait_ms = std::max<std::uint32_t>(wait_ms, 1);

        // Waits on the presentation queue rather than delaying blindly, so a state
        // change is drawn as soon as it is submitted instead of at the next tick —
        // §10 Interruptibility: "The interface shall immediately represent newly
        // received printer state."
        //
        // The flush-completion semaphore is deliberately NOT waited on here. It
        // belongs to LVGL's flush-wait callback, and taking it from this loop as well
        // would steal the signal that callback is blocked on and send it into its
        // lost-completion path on every frame.
        wait_and_apply(wait_ms);

#if defined(CONFIG_PRD_UI_RENDER_REPORT)
        const auto now_us = platform::monotonic_time().count();
        if (now_us >= next_report_us) {
            next_report_us = now_us + kReportIntervalUs;

            // Sampled here, at the deepest point of the loop, so the stack
            // high-water mark reflects a full render-and-flush iteration.
            panel::sample_memory();
            log_report();
        }
#endif
    }
}

void render_task(void* /*argument*/) {
    g_setup_ok = setup();
    g_running = g_setup_ok;

    // Released only after g_setup_ok is written, so init() cannot observe a
    // half-finished setup.
    (void)xSemaphoreGive(g_setup_done);

    if (!g_setup_ok) {
        g_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    run_loop();
}

} // namespace

bool init(const Config& config) {
    if (g_task != nullptr) {
        return g_running;
    }

    if (config.draw_buffer_rows <= 0 || config.draw_buffer_count < 1 ||
        config.draw_buffer_count > 2 || config.task_stack_bytes == 0) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "invalid ui configuration");
        return false;
    }

    g_config = config;
    g_setup_ok = false;

    // Created before the task, so a submit() racing start-up is overwritten into the
    // queue rather than counted as rejected. One element: §10.7's overwrite/latest-
    // value semantics require length one, because xQueueOverwrite is only defined for
    // a queue of length one.
    if (g_presentations == nullptr) {
        g_presentations = xQueueCreate(1, sizeof(PresentationEnvelope));
        if (g_presentations == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create the presentation queue");
            return false;
        }
    }

    if (g_setup_sessions_lock == nullptr) {
        g_setup_sessions_lock = xSemaphoreCreateMutex();
        if (g_setup_sessions_lock == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create the setup session lock");
            return false;
        }
    }

    if (g_setup_sessions == nullptr) {
        g_setup_sessions = xQueueCreate(1, sizeof(SetupSession));
        if (g_setup_sessions == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create the setup session queue");
            return false;
        }
    }

    if (g_display_intents == nullptr) {
        g_display_intents = xQueueCreate(1, sizeof(DisplayIntent));
        if (g_display_intents == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create the display intent queue");
            return false;
        }
    }

#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    if (g_diagnostic_labels == nullptr) {
        g_diagnostic_labels = xQueueCreate(1, sizeof(DiagnosticLabel));
        if (g_diagnostic_labels == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create the diagnostic label queue");
            return false;
        }
    }
#endif

    if (g_setup_done == nullptr) {
        g_setup_done = xSemaphoreCreateBinary();
        if (g_setup_done == nullptr) {
            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "failed to create ui setup semaphore");
            return false;
        }
    }

    if (xTaskCreate(render_task, kTaskName, config.task_stack_bytes, nullptr,
                    static_cast<UBaseType_t>(config.task_priority), &g_task) != pdPASS) {
        g_task = nullptr;
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "failed to create ui task");
        return false;
    }

    if (xSemaphoreTake(g_setup_done, pdMS_TO_TICKS(kSetupTimeoutMs)) != pdTRUE) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "ui task setup timed out");
        return false;
    }

    return g_setup_ok;
}

bool is_running() {
    return g_running;
}

std::string_view task_name() {
    return kTaskName;
}

std::uint32_t task_stack_bytes() {
    return g_config.task_stack_bytes;
}

FlushStatistics flush_statistics() {
    return panel::statistics();
}

DrawBufferReport draw_buffers() {
    return panel::buffers();
}

MemoryReport memory_report() {
    return panel::memory();
}

std::string_view renderer_version() {
    return g_version;
}

void submit(const app_model::Presentation& presentation, const std::uint32_t revision) {
    if (g_presentations == nullptr) {
        // init() has not run. Counted rather than silent: a presentation submitted
        // before the renderer exists means the composition root started the store
        // before the display, which is a boot-order defect worth seeing.
        ++g_intake.rejected;
        return;
    }

    ++g_intake.submitted;

    // How many submissions were replaced before the render task could take them. Read
    // before the overwrite, because after it the queue always holds exactly one.
    if (uxQueueMessagesWaiting(g_presentations) != 0) {
        ++g_intake.superseded;
    }

    // Overwrite, never send. xQueueSend on a full one-element queue would either block
    // the dispatching task — which is the application task, and blocking it would stall
    // every event in the system behind a redraw — or fail and drop the newest state in
    // favour of an older one already queued. Overwriting keeps the newest, which is the
    // only presentation worth drawing.
    const PresentationEnvelope envelope{presentation, revision};
    (void)xQueueOverwrite(g_presentations, &envelope);
}

std::uint32_t last_flushed_revision() {
    return g_last_flushed_revision.load(std::memory_order_acquire);
}

void submit_setup_session(const SetupSession& session) {
    if (g_setup_sessions == nullptr || g_setup_sessions_lock == nullptr) {
        return;
    }

    // ⛔ NOTHING IS LOGGED HERE, IN ANY PROFILE. The whole point of this path is that the
    // password reaches a panel and no other surface (owner ruling D1).
    SetupMailboxOps ops;
    setup_session_mailbox::publish(ops, session);
}

void submit_display(const DisplayIntent& intent) {
    if (g_display_intents == nullptr) {
        return;
    }

    (void)xQueueOverwrite(g_display_intents, &intent);
}

void submit_diagnostic_label(const DiagnosticLabel& label) {
#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    if (g_diagnostic_labels == nullptr) {
        return;
    }

    (void)xQueueOverwrite(g_diagnostic_labels, &label);
#else
    // ⬅️ A NO-OP IN A PRODUCT IMAGE, AND NOT AN OVERSIGHT. No diagnostic surface exists to write
    // on, and the harness that would call this is not compiled either. Accepting and discarding
    // keeps the public header free of sdkconfig — the same trade validation::log_button_gesture()
    // makes.
    (void)label;
#endif
}

void clear_setup_session() {
    // The empty session is the sentinel; drain_setup_session() reads it as a clear.
    const SetupSession empty{};
    submit_setup_session(empty);
}

IntakeStatistics intake_statistics() {
    return g_intake;
}

SceneReport scene_report() {
#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    // No product scene exists in this build. Reported as such rather than as zeroes
    // from a controller that was never built.
    SceneReport report;
    report.scene = "diagnostic";
    return report;
#else
    const auto internal = controller::report();

    SceneReport report;
    report.applied = internal.applied;
    report.redundant = internal.redundant;
    report.scene_changes = internal.scene_changes;
    report.operational = internal.operational;
    report.unimplemented = internal.unimplemented;
    report.scene = internal.scene;
    return report;
#endif
}

OperationalFormReport operational_form_report() {
#if defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
    // No product composition exists in this build, so no operational form has ever been
    // drawn. Reported as "nothing drawn" rather than as zeroes read out of a controller
    // that was never built — the same distinction scene_report() makes above.
    return OperationalFormReport{};
#else
    return controller::operational_form_report();
#endif
}

bool tabular_figures_verified() {
    return g_tabular_ok;
}

} // namespace prd::ui
