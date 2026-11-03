// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "app_core/app_core.hpp"

// sdkconfig.h explicitly, not by transitive luck. The CONFIG_* macro the input
// startup is gated on comes from there, and a #if on an undefined macro is silently
// false — which during Phase B quietly turned four gated flows into empty stubs.
#include "sdkconfig.h"

#include <atomic>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>

// ESP-IDF headers are not -Wundef clean. Suppress locally (per architecture
// spec §12.3) so the project -Wundef policy still applies to our own code.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#pragma GCC diagnostic pop

#include "app_model/build_info.hpp"
#include "app_model/device_data.hpp"
#include "app_model/display_policy.hpp"
#include "app_model/events.hpp"
#include "app_model/hardware_check.hpp"
#include "app_model/presentation.hpp"
#include "app_store/store.hpp"
#include "board/board.hpp"
#include "button/button.hpp"
#include "button_translation.hpp"
#include "candidate.hpp"
#include "config/config.hpp"
#include "configuration_observation.hpp"
#include "diagnostics/boot_summary.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "display/display.hpp"
#include "exclusive_operation.hpp"
#include "factory_reset.hpp"
#include "network/discovery.hpp"
#include "network/setup_ap.hpp"
#include "network/wifi.hpp"
#include "ota/first_boot.hpp"
#include "ota/live_update.hpp"
#include "ota/release_schedule.hpp"
#include "platform/clock.hpp"
#include "platform/memory.hpp"
#include "platform/system.hpp"
#include "printer_session.hpp"
#include "provisioning/server.hpp"
#include "provisioning_decision.hpp"
#include "provisioning_flow.hpp"
#include "prusalink/prusalink.hpp"
#include "reentry.hpp"
#include "reset_progress.hpp"
#include "secrets/secrets.hpp"
#include "setup_entry.hpp"
#include "storage/key_value.hpp"
#include "storage/storage.hpp"
#include "submission_handoff.hpp"
#include "ui/ui.hpp"
#include "validation/validation.hpp"
#include "wifi_restore.hpp"

// The composition root — Architecture Spec §4.1, §5.3.
//
// ORCHESTRATION ONLY. It starts services, connects boundaries, and owns the
// application task. It contains no PrusaLink parsing, no UI layout, no polling rule,
// no credential logic and no board pin. It also, as of Milestone 2 Phase B, contains
// no bring-up flow: those moved to the validation component, which is what took this
// file from roughly 880 lines to something a reader can hold in their head.
//
// THE ONE STRUCTURAL RULE HERE is §10.2's: a single application task is the only
// thing that dispatches into the store. Everything else — the button consumer, and
// later the network worker — posts a typed event to the bounded queue and lets that
// task apply it. The store takes no lock and does not need one, because there is
// exactly one writer by construction.

namespace prd::app_core {
namespace {

constexpr std::string_view kProductName = "Prusa Remote Display";

// §10.2's app_task: single consumer of the event queue, sole dispatcher into the
// store, runs the lifecycle timers.
//
// 12288, and the reason is arithmetic rather than habit. reduce() is a pure function
// over a 2 kB AppState, so a single dispatch costs roughly 2 kB for the reducer's own
// copy plus 2 kB for the returned temporary plus 360 bytes for the Event. The
// validation flow's fixture catalogue nests another AppState on top of that, and the
// boot sequence builds several std::string log lines through ESP-IDF's logging path,
// which is not shallow either.
//
// The task reports its own high-water mark twice during startup, so this figure is
// checked against measurement rather than trusted.
constexpr char kAppTaskName[] = "prd_app";
constexpr std::uint32_t kAppTaskStackBytes = 12288;
constexpr UBaseType_t kAppTaskPriority = 5;

// How often the task wakes when no event arrives. Sets the resolution with which a
// scheduled scenario step lands, and the granularity of the lifecycle timers that
// will live here later.
constexpr TickType_t kAppTickInterval = pdMS_TO_TICKS(100);

// §10.7's bounded application event queue. Twelve slots of 360 bytes: deep enough
// that a burst of button presses and a scenario step cannot collide, small enough
// that the memory is a rounding error against the draw buffers.
constexpr UBaseType_t kEventQueueLength = 12;

// Printer report slots. See g_printer_reports for why two and why not overwrite.
constexpr UBaseType_t kPrinterReportSlots = 2;

// The button consumer. Translates gestures and posts them; it never dispatches.
//
// It also performs button::init() itself, on its own stack. That is not a detail:
// the ESP-IDF main task has 3584 bytes, and button::init() — gpio_config, the ISR
// service, two queues, a task, and one long std::string log line — overflows it.
// Milestone 1 ran the same init on a dedicated 6144-byte task for the same reason,
// and moving it onto the main task during Phase B reproduced the overflow on the
// first boot.
//
// 6144 is the measured figure for exactly this work: at 4096 the Milestone 1 task
// doing init plus per-gesture string logging ran down to 640 bytes free — 15 %
// headroom — and 6144 restored it to roughly 44 %.
constexpr char kButtonTaskName[] = "prd_button_in";
constexpr std::uint32_t kButtonTaskStackBytes = 6144;
constexpr UBaseType_t kButtonTaskPriority = 4;

// PINNED TO CORE 0, and this one is a measured necessity rather than a preference.
//
// Architecture Spec §10.9 says not to pin tasks without a concrete benefit, and
// allows "a documented core-affinity decision" when measurement shows one. This is
// that measurement: button::init() installs the GPIO ISR service, and esp_intr_alloc
// runs the allocation on the core the interrupt belongs to. Called from an unpinned
// task that the scheduler happened to place on core 1, it therefore routes through
// ESP-IDF's ipc1 task — whose stack is 1024 bytes, and which overflowed on the first
// boot with the interrupt allocator's verbose logging on top of it.
//
// Worse than the crash was the shape of it: whether the IPC hop happens at all
// depends on which core the task started on, so the same image boots or dies
// according to scheduler luck. Milestone 1 installed the same service from an
// unpinned task and passed 25 consecutive boots, which means it had this latent all
// along and was fortunate.
//
// Pinning removes the hop entirely: the allocation happens on the core that is
// already running.
constexpr BaseType_t kButtonTaskCore = 0;

// Pinned, not merely "currently 0". An unpinned task made the IPC hop conditional on
// scheduler placement, so the same image booted or crashed on luck; this is what stops
// a future edit reverting to tskNO_AFFINITY without noticing what it costs.
static_assert(kButtonTaskCore != tskNO_AFFINITY,
              "the button task must be pinned: unpinned, the GPIO ISR allocation IPCs "
              "to whichever core the scheduler chose");
static_assert(kButtonTaskCore < portNUM_PROCESSORS, "button task core must exist");

// Long enough that an untouched button is quiet, short enough that a validation run
// shows the line periodically rather than looking hung.
constexpr int kButtonIdleReportMs = 10000;

// Longest to wait for the renderer's first completed flush before lighting the panel.
//
// Bounded, so a renderer that never draws produces a reported fault rather than a dark
// screen with no explanation. The backlight is held off until a frame has reached the
// panel so the first thing the eye sees is rendered content rather than uninitialised
// panel RAM — moved unchanged from the Milestone 1 Phase F flow, where it was proved
// on the unit.
constexpr TickType_t kFirstFrameTimeout = pdMS_TO_TICKS(3000);
constexpr TickType_t kFirstFramePoll = pdMS_TO_TICKS(20);

// Task stack reporting cadence for the application task. §21 tracks high-water
// marks; once a minute is enough to catch drift without becoming log noise.
constexpr TickType_t kStackReportInterval = pdMS_TO_TICKS(60000);

// Low-frequency liveness heartbeat. Emitted at debug level, so it is visible in the
// debug and hardware-validation profiles and filtered out in release.
constexpr TickType_t kHeartbeatInterval = pdMS_TO_TICKS(10000);

// --- Validated minimums -----------------------------------------------------
//
// Every one of these was a crash on the physical unit during Milestone 2 Phase B, and
// every one of them is a number that a well-meaning edit could quietly lower again. A
// host test cannot catch a target stack overflow — it does not run on the target and
// has no stack canary to trip — so the guard has to be at compile time, where it
// applies to every profile and every build.
//
// The figures are measured, not argued. Each task also reports its own high-water
// mark at run time, so the margin below stays checkable rather than becoming folklore.

// Measured peak 7996 bytes during the boot sequence and the fixture catalogue. 8192
// was the first value, chosen by argument, and it would have been 98 % used.
static_assert(kAppTaskStackBytes >= 12288,
              "prd_app measured 7996 bytes peak; below 12288 the margin is not credible");

// Measured peak 3268 bytes. Milestone 1 measured the same work at 4096 and had 15 %
// headroom left, which is why 6144 is the floor rather than 4096.
static_assert(kButtonTaskStackBytes >= 6144,
              "prd_button_in performs button::init(); 4096 measured only 15% headroom");

// Installing the GPIO ISR service with ESP_INTR_FLAG_IRAM runs work on the other
// core's IPC task. The ESP32-S3 default of 1280 overflowed it with the interrupt
// allocator's logging on top, and pinning the caller only moved the overflow from
// ipc1 to ipc0.
static_assert(CONFIG_ESP_IPC_TASK_STACK_SIZE >= 2560,
              "the IRAM GPIO interrupt allocation runs on the IPC task; 1280 overflows it");

// The interrupt is allocated with ESP_INTR_FLAG_IRAM so it survives a flash write,
// which requires everything it calls to be IRAM-resident. gpio_get_level() only is
// when this is set.
#if !defined(CONFIG_GPIO_CTRL_FUNC_IN_IRAM)
#error "CONFIG_GPIO_CTRL_FUNC_IN_IRAM is required: the button's IRAM ISR calls gpio_get_level()"
#endif

// The single authoritative application state (§24.11). File scope because the
// application task and the boot flow both reach it, and because a 2 kB object has no
// business on a task stack.
app_store::Store g_store;

QueueHandle_t g_events = nullptr;
std::uint32_t g_events_dropped = 0;

// The task allowed to dispatch. Recorded by that task itself at its own start, so it
// is set before the boot sequence — which dispatches — begins.
TaskHandle_t g_app_task = nullptr;
std::uint32_t g_foreign_dispatches = 0;

diagnostics::LogLevel level_for(const app_model::ValidationState result) {
    switch (result) {
    case app_model::ValidationState::Validated:
        return diagnostics::LogLevel::Info;

    case app_model::ValidationState::Unknown:
        return diagnostics::LogLevel::Warning;

    case app_model::ValidationState::Failed:
        return diagnostics::LogLevel::Error;
    }

    return diagnostics::LogLevel::Warning;
}

void log_capacity_check(const app_model::CapacityCheck& check) {
    diagnostics::log(level_for(check.result), diagnostics::tag::kBoard,
                     app_model::to_log_line(check));
}

// --- Event plumbing --------------------------------------------------------

// Applies an event to the store. Callable only from the application task, which is
// the store's single consumer (§10.2) — every other task uses post_event().
//
// The rule is enforced, not just documented. It is already structural at compile
// scope — this function is in an anonymous namespace and only the boot sequence and
// the application task's own loop call it — but a future edit could add a third
// caller from the wrong task, and the store takes no lock precisely because that
// cannot happen. Two consequences if it did: the reducer runs on a stack not sized
// for a 2 kB AppState copy, and two writers race the authoritative state.
//
// Reported rather than refused. Dropping the event would trade a diagnosable defect
// for a silently lost fact, and Phase B's own main-task overflow is the argument:
// what was missing then was a name for the failure, not a way to suppress it.
void dispatch(const app_model::Event& event) {
    if (g_app_task != nullptr && xTaskGetCurrentTaskHandle() != g_app_task) {
        ++g_foreign_dispatches;
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "dispatch from a task other than " + std::string{kAppTaskName} +
                             " — §10.2 requires a single consumer; event=" +
                             std::string{app_model::log_label(event.type)} +
                             " total=" + std::to_string(g_foreign_dispatches));
    }

    (void)g_store.dispatch(event);
}

// Posts an event for the application task to apply. Safe from any task.
bool post_event(const app_model::Event& event) {
    if (g_events == nullptr) {
        return false;
    }

    if (xQueueSend(g_events, &event, 0) == pdTRUE) {
        return true;
    }

    // §10.7: "critical events must not be silently dropped ... queue saturation must
    // increment a diagnostic counter". Nothing here is high-frequency, so a full
    // queue means the application task has stopped draining, which is a defect and
    // is reported as one.
    ++g_events_dropped;
    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                     "event queue full; dropped " + std::string{app_model::log_label(event.type)} +
                         " total_dropped=" + std::to_string(g_events_dropped));
    return false;
}

// Moves the boot phase and logs it from the resulting state, so the line reports
// what the store actually holds rather than what was intended.
void enter_boot_phase(const app_model::BootPhase phase) {
    dispatch(app_model::Event::boot_phase(platform::monotonic_time(), phase));
    diagnostics::log_boot_phase(app_model::log_label(g_store.state().boot.phase));
}

// --- Display power ---------------------------------------------------------
//
// The seam between the policy (app_model/display_policy.hpp) and the driver. It decides nothing:
// every rule lives in the pure unit, and everything the panel does is an event through the store.
//
// ⛔ IT NEVER TOUCHES THE BACKLIGHT. display.hpp requires one owner for the LEDC channel, and that
// owner is the render task. This posts a ui::DisplayIntent and the render task ramps to it, which
// is also what keeps the two-second fade from blocking this task and stalling polling, the
// station and the provisioning tick.

// The owner-fixed working baselines for this checkpoint.
constexpr app_model::display_policy::Timings kDisplayTimings{};

// The recession's activity moment, in the same monotonic milliseconds the tick is decided from.
std::uint32_t g_display_activity_ms = 0;

app_model::display_policy::Situation g_display_situation{};
bool g_display_situation_seen = false;

// §7.11's lastInteractionAt as this tick last saw it. A change means the reducer accepted a button
// gesture since the previous tick, which is activity — observed rather than intercepted, so the
// gesture keeps exactly one owner.
app_model::Timestamp g_display_interaction{};

// What was last asked of the panel, so an unchanged decision posts nothing.
//
// ⛔ THE WHOLE OUTPUT, NOT THE POWER LEVEL IT CAME FROM. On at a 50 % ceiling and On at 80 % are
// the same DisplayPower, so a cache keyed on the level would swallow every brightness change the
// user ever made while the panel stayed awake — and the startup-brightness observation below is
// exactly such a change, arriving on the first tick.
app_model::display_policy::Output g_display_output_posted{};
bool g_display_output_posted_valid = false;

// The owner-selected brightness baselines for this checkpoint.
constexpr app_model::display_policy::Brightness kDisplayBrightness{};

// The panel's three lit control levels, in the driver's own units.
//
// ⛔ ABSOLUTE PRODUCT DEFAULTS, NOT A SCALING OF THE OBSERVED LEVEL — AND THAT DISTINCTION IS THE
// WHOLE ANSWER TO "WHAT IS THE SOURCE OF TRUTH". Active and Quiet are the two levels the owner
// selected on the panel; they are copied here unchanged. If they were instead expressed as
// fractions of DisplayState::brightness_percent, then display::kDefaultBrightnessPercent — a
// driver bring-up constant nobody chose as a preference — would silently become the user's
// configuration, and every later level would inherit it. That is exactly the accidental
// configuration this checkpoint must not create, and it is why the startup observation is NOT an
// input here.
//
// ⬅️ THE STARTUP REPAIR STILL HOLDS, BY AGREEMENT RATHER THAN BY ARITHMETIC. Its purpose was that
// the policy must never raise the panel off its validated startup level a few milliseconds after
// boot. At boot the device is not held awake, so the tier is Quiet, and Quiet is 50 — the same
// value display::kDefaultBrightnessPercent applies. The first tick therefore asks for the level
// already on the panel. The observation is still dispatched and still recorded in DisplayState;
// what changed is that it is now a RECORD of the panel rather than a SETTING for it.
//
// ⚠️ RECORDED DEBT, NOT HIDDEN: with the levels explicit, DisplayState::brightness_percent has no
// remaining consumer in the policy. It belongs on the configuration/migration checkpoint's list
// beside config::Config::brightness, dim_delay_seconds and sleep_delay_seconds — deciding what it
// means is that checkpoint's job, not something to settle incidentally here.
//
// ⬅️ COMPUTED ONCE. The Dimmed conversion costs a std::pow and the inputs are compile-time
// constants, so the answer is built on first use and never recomputed.
app_model::display_policy::Levels g_display_levels{};
bool g_display_levels_valid = false;

const app_model::display_policy::Levels& display_levels() {
    if (!g_display_levels_valid) {
        g_display_levels.active_percent = kDisplayBrightness.active_percent;
        g_display_levels.quiet_percent = kDisplayBrightness.quiet_percent;

        // ⛔ THE DISPLAY COMPONENT CONVERTS, NEVER THE POLICY. Dimmed is a fraction of the LIGHT
        // emitted at QUIET; brightness percentages are perceptually pre-distorted, so the same
        // fraction applied to the control value emits far less than it names — the confirmed defect
        // the owner met as an unreadable Dimmed state. 55 % of Quiet's light is control 38, which is
        // duty 487: the output observed at harness step D2.
        g_display_levels.dimmed_percent = display::percent_for_light_fraction(
            kDisplayBrightness.quiet_percent,
            display::LightFraction{kDisplayBrightness.dimmed_light_percent_of_quiet});

        g_display_levels_valid = true;

        diagnostics::timeline(diagnostics::tag::kApp, "display-levels", "active",
                              g_display_levels.active_percent, "quiet",
                              g_display_levels.quiet_percent);
        diagnostics::timeline(diagnostics::tag::kApp, "display-dimmed", "percent",
                              g_display_levels.dimmed_percent, "of-quiet-light",
                              kDisplayBrightness.dimmed_light_percent_of_quiet);
    }

    return g_display_levels;
}

void display_tick(const std::uint32_t now_ms) {
    // ⛔ THE BENCH HARNESS OWNS THE BACKLIGHT WHEN IT IS COMPILED, and the policy stands down
    // entirely. Same question display_patterns_own_panel() already answers for the panel itself:
    // two writers on one LEDC channel would interleave, and an owner judging a brightness level
    // would be judging a race. False in every product image, where this flow is not compiled.
    if (validation::brightness_harness_owns_panel()) {
        validation::brightness_harness_tick(platform::monotonic_time());
        return;
    }

    const app_model::AppState& state = g_store.state();

    const auto situation = app_model::display_policy::situation_of(state);
    const bool situation_changed = !g_display_situation_seen || situation != g_display_situation;
    const bool interacted = state.display.last_interaction_at != g_display_interaction;

    // ⛔ THIS IS THE WHOLE DEFINITION OF ACTIVITY, AND IT HAS NO OTHER SOURCE. A poll, a retry, a
    // redundant presentation, a telemetry refresh and a render pass all leave the Situation
    // identical and touch no interaction moment, so none of them can reach this branch — UI
    // Behaviour Decision 001 §4's "routine polling, retries and small telemetry changes shall not
    // become visible", enforced by the shape of Situation rather than by a filter.
    if (situation_changed || interacted) {
        g_display_activity_ms = now_ms;
        g_display_situation = situation;
        g_display_situation_seen = true;
        g_display_interaction = state.display.last_interaction_at;
    }

    // Evaluate the awake hold once for recession and brightness. Completed's bounded Active
    // acknowledgement is selected separately from settled power below, without extending this
    // hold. The predicate reads no display field and is unchanged by the dispatches below.
    const bool held = app_model::display_policy::holds_awake(state);

    // A meaningful change on a panel that is not lit is what wakes it — §17.8's wake conditions,
    // and UIBD 001 §4's "communication restored". Nothing else here can raise the power.
    if (situation_changed && state.display.power != app_model::DisplayPower::On) {
        dispatch(app_model::Event::display_wake(platform::monotonic_time()));
    } else {
        const std::uint32_t idle_ms =
            app_model::display_policy::elapsed_ms(now_ms, g_display_activity_ms);

        switch (app_model::display_policy::next_step(
            state.display.power, idle_ms, held,
            app_model::display_policy::recession_timings_for(state.subject, state.confidence,
                                                             kDisplayTimings))) {
        case app_model::display_policy::Step::Dim:
            dispatch(app_model::Event::display_idle_timer_fired(platform::monotonic_time()));
            break;

        case app_model::display_policy::Step::Sleep:
            dispatch(app_model::Event::display_sleep_timer_fired(platform::monotonic_time()));
            break;

        case app_model::display_policy::Step::Hold:
            break;
        }
    }

    // Read back AFTER the dispatch, so a level decided this tick is applied this tick.
    const app_model::AppState& settled = g_store.state();
    const app_model::DisplayPower power = settled.display.power;

    const app_model::display_policy::Tier tier = app_model::display_policy::output_tier_for(
        held, settled.subject, settled.confidence, power);

    const app_model::display_policy::Output output =
#ifdef CONFIG_PRD_VALIDATION_ACCESS_REVIEW
        validation::access_review_output(power, tier, display_levels(), kDisplayTimings);
#else
        app_model::display_policy::output_for(power, tier, display_levels(), kDisplayTimings);
#endif

    if (g_display_output_posted_valid && output == g_display_output_posted) {
        return;
    }

    ui::DisplayIntent intent;
    intent.target_percent = output.target_percent;
    intent.fade_ms = output.fade_ms;

    ui::submit_display(intent);

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     std::string("display-power power=") +
                         std::string(app_model::log_label(power)) +
                         " tier=" + std::string(app_model::display_policy::log_label(tier)) +
                         " percent=" + std::to_string(output.target_percent) +
                         " fade_ms=" + std::to_string(output.fade_ms));

    diagnostics::timeline(diagnostics::tag::kApp, "display-power", "power",
                          static_cast<std::int64_t>(power), "percent", output.target_percent);

    g_display_output_posted = output;
    g_display_output_posted_valid = true;
}

// --- Presentation ----------------------------------------------------------

// The store observer that feeds the renderer — Architecture Spec §9.4's projection
// step, and §10.7's UI notification.
//
// Runs on the application task, inside the store's notification, so the projection
// happens on the only task that dispatches. That is deliberate and it is the cheaper
// direction: a Presentation is 412 bytes and an AppState is 2000, so projecting here
// and sending the result costs a fifth of what sending a snapshot for the render task
// to project would — and it keeps a 2 kB object off the render task's stack entirely.
//
// It must not dispatch and must not block. It does neither: project() is pure, and
// ui::submit() overwrites a one-deep queue and returns.
void publish_presentation(const app_model::AppState& state, void* /*context*/) {
    ui::submit(app_model::project(state), state.revision);
}

// Brings the panel and the renderer up, and connects the store to them.
//
// Architecture Spec §15.1 steps 8 and 14. Until Milestone 2 Phase C this happened
// inside a hardware-validation flow, which meant the debug and release images compiled
// a renderer and never started it — the display only ever came up in the one profile
// that set a bench flag.
bool start_presentation() {
    // The pattern flows write pixels straight to the panel, bypassing LVGL, and they
    // perform their own display::init(). Two writers to one panel interleave frames
    // that neither can be held responsible for, so when they are compiled in the
    // product renderer stays out of the way entirely. Asked of validation rather than
    // tested as a Kconfig symbol here, so the rule lives in one place.
    if (validation::display_patterns_own_panel()) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "display pattern flow owns the panel; product renderer not started");
        return true;
    }

    const auto heap_before = platform::heap_report();

    display::Config config; // validated SPI clock and accepted panel tuning
    if (!display::init(config)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "display init failed");
        return false;
    }

    if (!ui::init()) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp, "ui init failed");
        display::deinit();
        return false;
    }

    const auto heap_after = platform::heap_report();
    const auto buffers = ui::draw_buffers();

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "ui active renderer=lvgl-" + std::string{ui::renderer_version()} + " task=" +
            std::string{ui::task_name()} + " stack=" + std::to_string(ui::task_stack_bytes()) +
            " buffers=" + std::to_string(buffers.count) + "x" + std::to_string(buffers.bytes_each) +
            "B rows=" + std::to_string(buffers.rows) +
            " placement=" + (buffers.internal_dma ? "internal_dma" : "NOT_INTERNAL_DMA") +
            " tabular=" + (ui::tabular_figures_verified() ? "yes" : "NO"));

    // Internal RAM is what the draw buffers, LVGL's pool and the generated faces are
    // actually spent from, so the before/after pair is reported for that region.
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "heap internal_before=" + std::to_string(heap_before.free_internal_bytes) +
            " internal_after=" + std::to_string(heap_after.free_internal_bytes) + " consumed=" +
            std::to_string(static_cast<std::int32_t>(heap_before.free_internal_bytes) -
                           static_cast<std::int32_t>(heap_after.free_internal_bytes)) +
            " internal_largest=" + std::to_string(heap_after.largest_free_internal_block_bytes) +
            " psram_before=" + std::to_string(heap_before.free_psram_bytes) +
            " psram_after=" + std::to_string(heap_after.free_psram_bytes));

    if (!g_store.add_observer(publish_presentation, nullptr)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to register the presentation observer");
        return false;
    }

    // The store notifies only on an accepted mutation, so the state as it stands right
    // now would never be published. Submitted once explicitly, so the renderer always
    // holds a presentation rather than waiting for the next event to have one.
    ui::submit(app_model::project(g_store.state()), g_store.state().revision);

    // Light the panel only once a frame has actually reached it.
    TickType_t waited = 0;
    while (ui::flush_statistics().completed == 0 && waited < kFirstFrameTimeout) {
        vTaskDelay(kFirstFramePoll);
        waited += kFirstFramePoll;
    }

    const bool drew = ui::flush_statistics().completed != 0;
    diagnostics::log(drew ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
                     diagnostics::tag::kApp,
                     drew ? "first frame flushed — lighting panel"
                          : "no frame flushed before timeout — lighting panel anyway");

    display::set_backlight_enabled(true);

    // ⛔ AND THE MODEL IS TOLD WHAT THE PANEL IS ACTUALLY AT. Three numbers disagreed until this
    // line existed: display::Config::startup_brightness_percent (kDefaultBrightnessPercent = 50)
    // is what backlight_init() applied and what the panel is showing; config::Config::brightness
    // defaults to 80 and is persisted but not yet consumed by anything; and
    // DisplayState::brightness_percent defaults to 100. Nothing ever dispatched
    // DisplayBrightnessObserved, so the model's 100 was never corrected — the same class of gap
    // as the idle timers that had no dispatcher.
    //
    // ⛔ WITHOUT THIS THE DISPLAY POLICY SILENTLY RAISES THE PANEL. The first display_tick() finds
    // power On with an uninitialised cache, computes brightness_for(On, 100) and posts a 100 %
    // intent with no fade — overriding the validated 50 % startup level a few milliseconds after
    // boot, which is precisely the printer-state-independent brightness change nothing should be
    // making.
    //
    // ⬅️ AN OBSERVATION, WHICH IS WHY IT IS THE RIGHT SEAM. app_core reads the driver it already
    // owns and tells the model through the event the model already defines. app_model gains no
    // knowledge of display:: and no dependency on it, and no printer state reaches a PWM value.
    //
    // On the application task: app_task() records g_app_task before calling boot_sequence(), so
    // this is the single dispatcher §10.2 requires.
    dispatch(app_model::Event::display_brightness(platform::monotonic_time(),
                                                  display::brightness_percent()));

    return true;
}

// --- Boot sequence ---------------------------------------------------------

// Declared ahead of boot_sequence() so the boot reads top to bottom in the order it
// actually happens, rather than being written upside down to satisfy the compiler.
// What the configuration load produced, carried across the boot sequence.
//
// IT HOLDS THE STORED CREDENTIAL, AND ONLY UNTIL THE STATION HAS IT. §13.5 keeps
// credentials out of logs and AppState, and out of anything long-lived here: this is a
// stack local of boot_sequence(), it is handed to start_connectivity() by reference, and
// the password is cleared out of it before that returns. No static in this file ever holds
// a credential the user typed on a previous boot.
struct ConfigurationLoad {
    secrets::LoadReport secret;
    bool wifi_credentials_usable{false};
    ProvisioningDecision decision{ProvisioningDecision::NotRequired};
};

// The provisioning session's access-point credential, for as long as provisioning lasts.
//
// ⛔ THE ONE SECRET THIS FILE RETAINS, AND IT IS RETAINED DELIBERATELY. Owner ruling R1
// makes the setup password a per-session value that lives in RAM and is never persisted;
// M3.6 puts it on the panel, which is the only channel that will ever reveal it, so it has
// to outlive the call that started the access point. It is generated here, never read back
// from storage, and cleared when provisioning ends — which in Milestone 3.5 is never,
// because nothing can finish provisioning until M3.8 exists.
//
// It reaches no log in any profile (owner ruling D1) and it is not in AppState (§24.3).
network::setup_ap::Session g_setup_session;

// --- The provisioning transaction (Milestone 3.8) --------------------------
//
// ⛔ THE SUBMITTED CREDENTIAL LIVES HERE, AND ONLY FOR THE LENGTH OF ONE TRANSACTION.
// §13.5 and §24.3 stand: it never enters AppState, Presentation, a log or a GET response.
// It arrives on a queue from the server task, is owned by the application task while the
// station tries it, and is wiped the moment the transaction ends either way.
//
// A QUEUE RATHER THAN A SHARED STRUCT, because the two sides are different tasks. The
// server task may not dispatch (§10.2), so it does the one thing a foreign task may do:
// leave the submission where the application task will find it. Depth one, so a second
// submission replaces a first that has not been picked up rather than queueing behind it.
//
// ⛔ THE ITEM IS A POINTER, NOT A SUBMISSION, AND THAT IS THIS CHECKPOINT'S WHOLE SUBSTANCE. It
// used to be a whole provisioning_flow::Submission passed by value through xQueueOverwrite() —
// passphrase and API key included — and while both endpoint copies were wiped, the queue's own
// storage never was and could not be: FreeRTOS's prvCopyDataFromQueue() memcpy's OUT of that buffer
// and moves its read pointer, and xQueueGenericReset() touches only pointers and counters. So the
// credential persisted in kernel memory for the life of the queue, which §13.5's "embedded in crash
// dumps where avoidable" forbids with coredump-to-flash enabled. What crosses now is one
// non-secret ownership token; submission_handoff.hpp holds the ownership rules and its host tests.
//
// ⛔ AND xQueueOverwrite() IS GONE WITH IT. Overwriting a POINTER would drop the displaced one on
// the floor — a leaked heap object still holding a credential, with nothing left that could wipe or
// free it. Replacement is a take-then-put transaction instead; see submission_handoff::publish().
QueueHandle_t g_submissions = nullptr;

// How many times a submission had to be released without reaching the application task. §10.7:
// saturation and refusal are counted, never silent.
//
// ⛔ SATURATING, NOT WRAPPING, AND THE REASON IS THE ONE CASE THAT MATTERS. A counter that wrapped
// would read 0 after 2^32 refusals, and 0 is the value that means "this has never happened" — so
// the one reading a reader must be able to trust would become the one reading that lies. Reaching
// the ceiling needs 4.29 billion refused submissions, which cannot happen in a product lifetime;
// pinning there costs one comparison and removes the question entirely.
std::uint32_t g_submissions_unavailable = 0;

// ⛔ NO FORMATTING, NO ALLOCATION. Called on the allocation-failure path, where the heap has just
// refused, so it may do nothing but compare and increment.
void note_submission_unavailable() {
    if (g_submissions_unavailable != UINT32_MAX) {
        ++g_submissions_unavailable;
    }
}

// The queue's item size, named once so the creation call and the assertions below cannot disagree.
//
// ⛔ THIS IS THE CHECKPOINT'S CENTRAL INVARIANT, AND IT IS NOW MECHANICAL RATHER THAN A LITERAL
// SOMEBODY MUST REMEMBER. A future edit that put the submission back into the queue by value would
// have to defeat both assertions to do it, and the second one states the actual danger in the units
// that matter: the item is pointer-sized, so it cannot be large enough to hold a credential field.
constexpr std::size_t kSubmissionItemBytes = sizeof(provisioning_flow::Submission*);

static_assert(kSubmissionItemBytes == sizeof(void*),
              "the submission queue must carry a pointer, never a submission");
static_assert(kSubmissionItemBytes < sizeof(provisioning_flow::Submission),
              "a queue item large enough to hold a Submission would put credentials back into "
              "FreeRTOS storage that nothing can wipe");

// The printer worker's report channel, from the worker task back to this one.
//
// ⛔ ONE QUEUE, WHOLE REPORTS, AND BOTH PROPERTIES ARE LOAD-BEARING.
//
// WHOLE, because the previous design had the worker translate a Poll report into three separate
// application events on the worker task. That put the liveness decision on the producing side of a
// queue it could not see the far end of, so a supersession between the check and the posts left a
// dead worker's connection, identity and snapshot to be applied afterwards. Now the complete
// generation-stamped Report crosses the boundary and this task decides.
//
// ONE, because §10.4 permits exactly one sequential worker, so exactly one purpose is ever in
// flight. Two queues would invent a cross-queue ordering question that cannot arise, and would need
// a rule for which to drain first. Purpose separation is explicit at the receiver instead:
// printer_session::classify() compares the report's purpose against the session's, so a Poll report
// can never settle a provisioning transaction and a Validate report can never colour operational
// state.
//
// Depth two, xQueueSend rather than xQueueOverwrite: a report must never be silently replaced by
// its successor. One sequential worker publishing at §11.2's cadence cannot outrun a 100 ms tick,
// so the second slot is headroom for a stalled application task rather than an expected depth. A
// full queue is counted and logged (§10.7) and is not silent.
QueueHandle_t g_printer_reports = nullptr;
std::uint32_t g_printer_reports_dropped = 0;

// How many validation attempts this boot has made. Monotonic and never reset: it is what
// distinguishes two validations of the SAME profile, since a retry writes no new profile id.
std::uint32_t g_validation_attempts = 0;

// ⛔ THE WHOLE STATE OF ONE VALIDATION ATTEMPT, IN ONE OBJECT. It was four separate globals — the
// transaction, the attempt counter, the staged pair and the deadline — and every boundary that
// ended an attempt cleared a different subset of them. A new submission left the staged pair
// behind. Assigning a default Validation is now the only way to end an attempt, so supersession,
// settlement, timeout, retry and a new submission cannot disagree about what was forgotten.
printer_session::Validation g_printer_validation{};

// Which polling worker this task is listening to. Armed by printer_launch() from the generation
// prusalink::start() reports, disarmed the moment the worker is superseded.
// ⛔ THE SESSION AND THE IDENTITY IT WAS LAUNCHED FOR, IN ONE OBJECT. Audit item 4: a worker that is
// still ELIGIBLE to poll but pointed at a printer the device is no longer configured for was
// invisible to printer_supervise(), so its next report matched the armed session and refilled the
// state the identity boundary had just cleared. Pairing the two values means every site that forgets
// one forgets the other.
printer_session::Polling g_poll{};

provisioning_flow::Submission g_submission{};
provisioning_flow::WifiValidation g_validation;
bool g_transaction_running = false;

// --- Restoring the radio after an abandoned candidate (checkpoint 3e) ------
//
// §13.3 step 7 obtains its "retain" half by construction — a failed transaction never moved either
// activation pointer, so flash still holds the previous pair — but begin_transaction() hands the
// candidate's credential to the DRIVER before anything proves it. A candidate that fails, or a
// session that ends before it commits, therefore leaves the station joined to a network nothing
// persisted, which the next boot silently changes underneath the user.
//
// ⛔ AND STEP 7 SAYS *RESTORE*, WHICH IS A COMPLETED ACT. The first version of this checkpoint
// treated `submit() != kNoTag` as a restoration, which is acceptance of an asynchronous intent and
// nothing more: on the retained path not one driver call had been issued at that point. §8.2 states
// the mechanism the repair uses — "effects are executed by `app_core`, then completion or failure is
// returned as another event".

// ⬅️ THE EXACT ROLLBACK TARGET, CAPTURED BEFORE THE CANDIDATE FIRST DISTURBS THE RADIO. That instant
// is the last one at which the device is provably running the pair it would have to go back to, so
// it is the only honest moment to read the pointers — and reading them LATER is what let a store
// that degraded mid-transaction withdraw a device whose exact target had already been known.
//
// It survives Wi-Fi validation, printer validation and transaction teardown, and it is captured ONCE
// per disturbance: a second submission supersedes the candidate but not the target, because the
// original capture is the one taken while the previous pair was still demonstrably active.
struct RollbackTarget {
    // The driver holds a credential from this session that nothing has persisted, and `target` is
    // where the radio must be put back to. Set only once the station has ACCEPTED the intent — a
    // refused submit moved no radio.
    bool held{false};

    wifi_restore::Target target{};

    // ⛔ WHETHER THE BROWSER HAS ALREADY BEEN TOLD THE ROLLBACK FAILURE. Set only where the
    // candidate's own settlement published that exact typed failure — the staging disagreement — so
    // the asynchronous cleanup records the terminal outcome without announcing it a second time.
    // Every other route reaches the terminal with this false and the terminal is the one publication.
    bool reported{false};
};

RollbackTarget g_rollback{};

// A restoration is owed and will be driven from provisioning_tick(), on the application task, with
// that iteration's own clock reading.
//
// ⬅️ DEFERRED RATHER THAN DONE AT THE ABANDONMENT SITE, AND THE REASON IS R11. settle_transaction()
// is reached from eight places, several of them inside the Milestone 4 printer path where no tick
// reading is in scope; threading one down all of them would either widen this checkpoint into that
// path or invite the second clock reading R11 was the price of. The tick already owns an
// observation, and station_tick() has already run by the time this is consumed.
bool g_station_restore_due = false;

// ⛔ THE ASYNCHRONOUS HALF, AND IT HOLDS NO CLOCK. It retains the exact station tag of an accepted
// previous-pair submission, or the fact that a withdrawal is owed, until a TERMINAL station result
// arrives. Every bound it needs already exists inside the station — the completion deadline, the
// no-lease interval, §11.3's ladder and §16.2's give-up — so it adds no timer, no retry and no new
// working baseline. Pure and host-tested; this file only routes facts into it and acts on what comes
// back.
wifi_restore::Supervisor g_restore{};

// The station's most recent candidate evidence — checkpoint 3c.
//
// ⛔ ONE VALUE, PRODUCED ONCE AND CONSUMED ONCE. station_tick() writes it, provisioning_tick()
// takes it and clears it. Both run on this task, in that order, once per loop, so an answer can
// never settle two transactions and no answer is silently replaced before it is read.
//
// ⬅️ AND IT IS NOT A QUEUE. Operational truth and candidate truth are separate channels
// (§8.1's stale-answer rule applies to the second), and the second carries at most one fact per
// tick because the station takes at most one decision per tick.
struct StationEvidence {
    bool present{false};
    network::station::Tag tag{network::station::kNoTag};
    network::station::Outcome outcome{network::station::Outcome::Leased};

    // ⛔ WHO THIS RECORD IS FOR, DECIDED ONCE BY A TESTED FUNCTION AND CARRIED RATHER THAN
    // RE-DERIVED. The condition this replaces was three tokens of hand-written policy in this
    // translation unit — which the host suite cannot compile — and it dropped every record an
    // accepted restoration was waiting for. A value that travels with the evidence cannot disagree
    // with the decision that produced it.
    wifi_restore::Route route{wifi_restore::Route::Discard};
};

StationEvidence g_station_evidence{};

// ⬅️ STAGE 1 HAS SUCCEEDED AND NOTHING IS PERSISTED — owner ruling 1, 2026-08-16.
//
// True from the moment the submitted network is validated until the printer stage commits
// or the session ends. While it is true, g_submission holds the validated Wi-Fi credential
// in RAM, the station stays joined in APSTA, and the flash holds nothing new — so a power
// loss here boots the device un-provisioned, exactly as it would have before stage 1 ran.
//
// ⛔ NO SEPARATE IDLE POLICY (owner ruling 2). The credential is wiped at every terminal
// boundary that already exists — final persistence, replacement by another Wi-Fi stage,
// failure, and the access point going down — and provisioning idle lifetime remains M3.10's.
bool g_wifi_validated = false;

// Whether the 2 s dwell is currently open. Only so its begin and end appear in the timeline as
// their own interval — the owner asked for the dwell to be visible separately from association
// and DHCP, and a reader cannot separate them from the phase alone.
bool g_dwell_open = false;

// When the access point may stop, in microseconds since boot. 0 means "not scheduled".
//
// OWNER RULING D-2'S GRACE PERIOD. Validation succeeds, the transaction commits, the state
// reaches success, the browser gets one more poll — and only then does the access point go.
// Stopping it the instant the flow succeeded would take the network away from the page
// before it could show the result, and the ruling is explicit that the browser must not
// need another successful request after shutdown to learn that setup worked.
std::int64_t g_access_point_stop_due_us = 0;

constexpr std::int64_t kAccessPointGraceUs = 2'000'000;

// --- R6's persistent local interface (Milestone 3.9) ------------------------

// Whether this boot has already tried to start the local server on the LAN. One attempt per
// boot: a bind failure on a fixed port does not clear itself, and a tick that kept retrying
// would report the same failure for as long as the device stayed powered.
bool g_lan_server_attempted = false;

// The last reachable set handed to the provisioning server, so refresh_reachability() speaks
// only when something actually changed. Not a second source of truth — every value is read
// back from its owner on each tick and merely compared against these.
app_model::BoundedString<16> g_published_access_point;
app_model::BoundedString<16> g_published_station;

// --- The privileged setup session (Milestone 3.10) --------------------------

// The bounded idle lifetime of the access point a deliberate long press raised.
//
// ⛔ ONE SESSION AT A TIME, AND IT IS THE AUTHORIZATION. Owner ruling 2 of Milestone 3.9 makes
// the setup access point the only surface a sensitive mutation may arrive on; this object is how
// long that surface exists for. When it expires the access point stops and the persistent
// read-only LAN server carries on untouched.
reentry::PrivilegedSession g_privileged;

// Set when a deliberate long press asked for reconfiguration, cleared once the access point is
// up. A request rather than an immediate action: the gesture arrives on the application task
// through the event queue, and raising a radio there is work the tick already sequences.
bool g_reconfiguration_requested = false;

// What the most recent configuration load decided — Milestone 3.11 T18.
//
// ⬅️ WHY THE COMPOSITION ROOT REMEMBERS THIS AT ALL. A privileged session that expires has to know
// whether it was opened out of Recovery Mode, because owner ruling 2026-08-21 sends that case back
// to recovery rather than to SetupRequired. Nothing else carries the answer: the boot's
// ConfigurationLoad is a local, and AppState deliberately models the recovery STATE rather than the
// storage fault behind it — a state a long press is allowed to leave.
//
// ⛔ IT MIRRORS THE LOAD RATHER THAN BEING MAINTAINED BESIDE IT, which is what makes it hard to get
// wrong. load_configuration() assigns it on every call, and there are exactly two callers: the boot
// sequence and the re-observation a successful commit performs. So a commit that replaces the
// faulted record moves this to NotRequired as a consequence of the reload it already did, with no
// second invariant to remember — and "successful setup does not fall back into recovery" holds by
// construction instead of by a clearing statement somebody could forget to write.
ProvisioningDecision g_last_provisioning_decision = ProvisioningDecision::NotRequired;

// When a completed factory reset restarts the device, in microseconds since boot. 0 means not
// scheduled, and it doubles as the "already erased" marker so the tick runs the erase once.
//
// ⬅️ WHY A RESTART AT ALL. Owner Decision 4's semantic is that a factory reset "returns it to true
// first-boot behaviour". Erasing the records does not do that on its own: AppState still holds the
// configuration the device booted with, so the panel would keep presenting a printer that is no
// longer configured until somebody pulled the power. The boot sequence is what reads Absent
// configuration and enters setup, so reaching first-boot behaviour means booting.
//
// ⬅️ AND WHY IT IS HELD BRIEFLY. The erase is over in milliseconds. Restarting the instant it
// finished would mean Production UI §9's "Reset progress" presentation never reached the panel —
// the same defect as a confirmation nobody can see, one screen later. The hold is the render task's
// chance to draw it, on the same scheduled-deadline pattern owner ruling D-2's access-point grace
// already uses. Not a blocking delay: the tick owns it.
std::int64_t g_reset_restart_due_us = 0;

// A WORKING BASELINE, not product policy.
constexpr std::int64_t kFactoryResetHoldUs = 2'000'000;

// Overwrites `size` bytes through a volatile pointer so the compiler cannot elide the
// store. `network`'s radio::wipe() is the same three lines, but it lives in that
// component's private headers and §4.2 forbids reaching for it from here.
void wipe_bytes(void* const data, const std::size_t size) {
    auto* const bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = 0;
    }
}

// The four operations submission_handoff.hpp asks for, bound to this file's queue and heap.
//
// ⛔ IT DECIDES NOTHING. Every ordering rule — build before disturbing, put before releasing the
// predecessor, restore rather than destroy, release exactly once — lives in the pure unit where the
// host suite can reach it. What is here is the FreeRTOS and heap translation, and it is deliberately
// too small to hide a decision in.
//
// ⛔ NON-THROWING ALLOCATION, BECAUSE THE CALLER IS AN HTTP HANDLER. Exceptions are off
// (CONFIG_COMPILER_CXX_EXCEPTIONS is not set), so a plain `new` on a device with no heap left would
// abort the whole device over a form submission. std::nothrow makes exhaustion an ordinary 503 —
// §16.4's "abort the current request safely ... avoid immediate reboot loops".
struct SubmissionOps {
    using Payload = provisioning_flow::Submission;

    [[nodiscard]] Payload* allocate() const {
        return new (std::nothrow) Payload{};
    }

    void wipe_and_release(Payload* const owned) const {
        // Wiped before the free, never after: once it is freed the allocator owns those bytes and
        // may hand them to anything.
        wipe_bytes(owned, sizeof(*owned));
        delete owned;
    }

    [[nodiscard]] bool try_take(Payload*& out) const {
        return g_submissions != nullptr && xQueueReceive(g_submissions, &out, 0) == pdTRUE;
    }

    [[nodiscard]] bool try_put(Payload* const owned) const {
        return g_submissions != nullptr && xQueueSend(g_submissions, &owned, 0) == pdTRUE;
    }
};

bool report_hardware();
bool bring_up_storage();
[[nodiscard]] ConfigurationLoad load_configuration();
void start_connectivity(ConfigurationLoad& loaded);
void start_network(ConfigurationLoad& loaded);
// The millisecond clock, spelled once.
//
// ⛔ TWO READINGS OF THIS MUST NEVER BE COMPARED. Milestone 3.11 T4: local_interface_tick()
// read it, then start_setup_ap() read it again a hundred milliseconds later and recorded THAT
// as the session's activity moment — after which the tick's older reading was subtracted from
// the newer one. Unsigned, that underflowed to 4,294,967,176 ms, the privileged session was
// declared idle for 49 days, and the access point a user had just asked for was torn down
// 8 ms after it came up. The fix is structural: whoever owns a tick's time observation passes
// it down, and nothing on that path reads the clock a second time.
[[nodiscard]] std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

ota::FirstBoot g_first_boot;
ota::Readiness g_boot_readiness;
exclusive::FirmwareUpdateAdmission g_ota_admission;
ota::ReleaseCheckSchedule g_release_schedule;
// Published only after both the OTA worker and this queue exist. A failed startup
// deletes the unpublished queue, so a web callback can never use freed storage.
std::atomic<QueueHandle_t> g_update_requests{nullptr};
std::uint32_t g_live_ota_generation = 0;
std::uint32_t g_admitted_live_ota_generation = 0;
std::uint32_t g_reboot_after_revision = 0;
std::string g_ota_board;
std::string g_ota_version;
bool g_live_ota_started = false;

void first_boot_tick(const std::uint32_t now_ms) {
    const auto before = g_first_boot.health();
    auto readiness = g_boot_readiness;
    readiness.fatal_fault =
        readiness.fatal_fault || g_store.state().device_fault != app_model::DeviceFaultReason::None;
    g_first_boot.tick(ota::device_boot_port(), now_ms, readiness);
    if (g_first_boot.health() == before) {
        return;
    }
    app_model::OtaObservation observation;
    observation.pending_validation = g_first_boot.pending();
    observation.phase = g_first_boot.health() == ota::BootHealth::Confirmed
                            ? app_model::OtaPhase::Idle
                            : app_model::OtaPhase::Failed;
    if (observation.phase == app_model::OtaPhase::Failed) {
        observation.error.assign("First-boot validation failed");
    }
    dispatch(app_model::Event::ota_observed(platform::monotonic_time(), observation));
}

void start_setup_ap(std::uint32_t now_ms);
void start_input();
[[nodiscard]] provisioning::SubmissionHandoff
note_submission(const provisioning::Submitted& submitted, void* context);
void display_tick(std::uint32_t now_ms);
void provisioning_tick(std::uint32_t now_ms);
void station_tick(std::uint32_t now_ms);
void local_interface_tick();
void printer_supervise();
void printer_ingest();
void printer_launch();
void drain_printer_reports();
void quiesce_for_exclusive_operation();
[[nodiscard]] exclusive::Conditions exclusivity();
void begin_printer_validation();
void ensure_local_server();
void refresh_reachability();
void request_reconfiguration();
void run_factory_reset();
void halt_boot_faulted(app_model::DeviceFaultReason reason, std::string_view note);
void note_gesture(const app_model::Event& event);
void stop_setup_ap();
void complete_wifi_stage();
void commit_transaction();
void restore_station(std::uint32_t now_ms);
void start_live_ota();
void live_ota_tick();
[[nodiscard]] provisioning::UpdateHandoff note_update_request(void* context);

provisioning::UpdateHandoff note_update_request(void* /*context*/) {
    const std::uint8_t request = 1;
    const QueueHandle_t requests = g_update_requests.load(std::memory_order_acquire);
    return requests != nullptr && xQueueSend(requests, &request, 0) == pdTRUE
               ? provisioning::UpdateHandoff::Accepted
               : provisioning::UpdateHandoff::Unavailable;
}

void start_live_ota() {
    if (g_live_ota_started) {
        return;
    }
    const QueueHandle_t requests = xQueueCreate(1, sizeof(std::uint8_t));
    const auto build = app_model::current_build_info();
    g_ota_board = build.board_target;
    g_ota_version = build.version.to_string();
    ota::LiveConfig config;
    config.port = &ota::device_installation_port();
    config.admission = &g_ota_admission;
    config.device = {g_ota_board, "esp32s3", g_ota_version, ota::kMinimumBootloaderSchema,
                     ota::device_inactive_slot_size()};
    g_live_ota_started = requests != nullptr && ota::start_live_update(config);
    if (g_live_ota_started) {
        g_update_requests.store(requests, std::memory_order_release);
        const auto heap = platform::heap_report();
        diagnostics::log(
            diagnostics::LogLevel::Info, diagnostics::tag::kApp,
            "live OTA service ready internal_free=" + std::to_string(heap.free_internal_bytes) +
                " internal_largest=" + std::to_string(heap.largest_free_internal_block_bytes) +
                " psram_free=" + std::to_string(heap.free_psram_bytes));
        g_release_schedule.start(
            {static_cast<std::uint64_t>(esp_timer_get_time() / 1000), esp_random()});
    } else {
        if (requests != nullptr) {
            vQueueDelete(requests);
        }
        const auto heap = platform::heap_report();
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "live OTA service unavailable on this boot internal_free=" +
                             std::to_string(heap.free_internal_bytes) + " internal_largest=" +
                             std::to_string(heap.largest_free_internal_block_bytes) +
                             " psram_free=" + std::to_string(heap.free_psram_bytes));
    }
}

void live_ota_tick() {
    if (!g_live_ota_started) {
        return;
    }

    ota::LiveReport report;
    while (ota::take_live_report(report)) {
        if (!ota::live_report_belongs_to_session(g_live_ota_generation,
                                                 g_admitted_live_ota_generation, report)) {
            continue;
        }
        if (report.kind == ota::LiveReportKind::Available) {
            g_live_ota_generation = report.generation;
        }

        dispatch(app_model::Event::ota_observed(platform::monotonic_time(), report.observation));
        provisioning::publish_update(report.observation.available_version.view(),
                                     app_model::ota_holds_device(report.observation.phase));

        if (report.observation.phase == app_model::OtaPhase::RebootRequired) {
            g_reboot_after_revision = g_store.revision();
        } else if (report.observation.phase == app_model::OtaPhase::Failed) {
            g_live_ota_generation = 0;
            g_admitted_live_ota_generation = 0;
            provisioning::publish_update({}, false);
        }
    }

    if (g_reboot_after_revision != 0 && ui::last_flushed_revision() >= g_reboot_after_revision) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "Restarting presentation flushed; rebooting selected OTA image");
        platform::restart();
    }

    std::uint8_t request = 0;
    const QueueHandle_t requests = g_update_requests.load(std::memory_order_acquire);
    if (requests != nullptr && xQueueReceive(requests, &request, 0) == pdTRUE && request != 0 &&
        g_live_ota_generation != 0 && g_store.state().ota.phase == app_model::OtaPhase::Available) {
        auto conditions = exclusivity();
        conditions.firmware_update_authorized_surface = true;
        if (g_ota_admission.authorize(conditions) &&
            ota::request_live_install(g_live_ota_generation)) {
            // The worker may not report before the next event drain. Close that handoff window on
            // this sole-dispatcher task: once the request is queued successfully, the accepted
            // operation is truthfully Downloading at 0% and reducer mutation guards apply now.
            g_admitted_live_ota_generation = g_live_ota_generation;
            const auto version = g_store.state().ota.available_version;
            const auto accepted = ota::accepted_live_install(version.view());
            dispatch(app_model::Event::ota_observed(platform::monotonic_time(), accepted));
            provisioning::publish_update(version.view(), true);
        } else {
            g_ota_admission.release(false);
            provisioning::publish_update(g_store.state().ota.available_version.view(), false);
        }
    }

    const std::uint64_t now_ms = static_cast<std::uint64_t>(esp_timer_get_time() / 1000);
    if (g_release_schedule.due(now_ms) &&
        g_store.state().network.phase == app_model::NetworkPhase::Connected &&
        exclusive::firmware_update_permitted(exclusivity()) && ota::request_release_check() != 0) {
        g_release_schedule.checked({now_ms, esp_random()});
    }
}

// Runs on the application task. Order matters and is preserved from Milestone 1: the
// product line first, then the build summary, then the phases with the board report
// between them, so the two phase labels the committed evidence records —
// phase=initializing and phase=ready — appear where they always did.
void boot_sequence() {
    const auto build_info = app_model::current_build_info();

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     std::string{kProductName} + " " + build_info.version.to_string());

    diagnostics::log_boot_build_summary(build_info);

    // §7.2's phases, emitted as the work they name actually happens.
    enter_boot_phase(app_model::BootPhase::Starting);
    enter_boot_phase(app_model::BootPhase::HardwareInitializing);

    const bool hardware_ok = report_hardware();

    // §7.2's order, which puts hardwareInitializing before storageInitializing.
    //
    // §15.1's numbered boot sequence lists NVS as step 6 and the board as step 7, the
    // other way round. §7.2's phase list is what the BootPhase enum was derived from and
    // what the reducer already models, so it wins — and the order it gives is the better
    // one anyway: a board that failed its capacity checks should be reported before the
    // firmware starts writing to that board's flash, and nothing in NVS bring-up depends
    // on anything the probe discovers. Recorded because the two sections genuinely
    // disagree, rather than left for someone to find.
    enter_boot_phase(app_model::BootPhase::StorageInitializing);

    const bool storage_ok = bring_up_storage();

    // §15.1 step 10, and only when there is a store to read.
    //
    // Skipped rather than attempted on a storage failure, because every read would return
    // the same failure and the resulting ConfigurationStatus::StorageFailure would say
    // nothing the storage observation has not already said — while a boot line claiming
    // the phase would assert work that did not happen. The device fault raised by
    // bring_up_storage() is what the product presents in that case.
    ConfigurationLoad loaded;

    if (storage_ok) {
        // ⬅️ AN INTERRUPTED FACTORY RESET IS FINISHED BEFORE ANYTHING IS READ — Milestone 3.10.
        // The journal is present only because a user confirmed a reset that lost power partway,
        // and finishing it here means the configuration load below sees the completed result
        // rather than a half-cleared store.
        //
        // ⛔ THE JOURNAL IS THE ONLY AUTHORISATION. Without it this is silent: the store state an
        // interrupted reset leaves — records present, no activation marker — is exactly what an
        // abandoned first provisioning leaves, and erasing that automatically is the erasure
        // ruling R10 forbids.
        //
        // ⛔ AND THE OUTCOME IS THE OTHER HALF OF THAT — AUDIT ITEM 5, 2026-08-28. This used to call
        // run() and throw the answer away with a cast, so a resume that FAILED walked straight into
        // the configuration load the ordering exists to get in front of. There is no run() here any
        // more to discard: resume_if_pending() performs the one authorised attempt and returns the
        // terminal answer, and the switch below is exhaustive with no default, so a new answer
        // cannot arrive without a decision being made about it here.
        const factory_reset::BootAction action =
            factory_reset::resume_if_pending(storage::key_value());

        switch (action) {
        case factory_reset::BootAction::LoadConfiguration:
            enter_boot_phase(app_model::BootPhase::LoadingConfiguration);
            loaded = load_configuration();
            break;

        // ⛔ NOTHING IS LOADED AND NOTHING IS RECONCILED. `loaded` stays default-constructed, which
        // is ProvisioningDecision::NotRequired — so start_connectivity() below raises NO access
        // point and start_network() stops at station_absent(). Nothing mutable is started and then
        // withdrawn; it is never started.
        //
        // ⬅️ AND THE FAULT IS IN AppState BEFORE ANY OF THAT RUNS, because dispatch() applies to the
        // store synchronously on this task. exclusivity() reads AppState, so the barrier is held by
        // the first loop iteration, and request_reconfiguration() already refuses while a device
        // fault stands — a long press cannot raise setup behind this.
        //
        // ⬅️ THE REASON IS NOT RE-DERIVED HERE. fault_for() is host-compiled and host-tested; this
        // routes and names nothing.
        case factory_reset::BootAction::HaltFactoryResetIncomplete:
        case factory_reset::BootAction::HaltStorageFailure:
            halt_boot_faulted(factory_reset::fault_for(action), factory_reset::halt_note(action));
            break;
        }
    }

    enter_boot_phase(app_model::BootPhase::StartingServices);

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "application task queue_slots=" + std::to_string(static_cast<unsigned>(kEventQueueLength)) +
            " event_bytes=" + std::to_string(sizeof(app_model::Event)) +
            " scenarios=" + (validation::scenarios_compiled() ? "on" : "off") +
            " walk=" + (validation::walk_compiled() ? "on" : "off") +
            " printing-timeline=" + (validation::printing_timeline_compiled() ? "on" : "off"));

    // §15.1 steps 8 and 14: the panel, the renderer and the store-to-renderer link.
    // Product behaviour in every profile, and started before the boot resolves so the
    // panel lights as early as it did in Milestone 1.
    //
    // A failure remains diagnosable over USB rather than aborting the application.
    // However, the owner-approved first-boot policy does not confirm a pending OTA
    // image without successful display initialization.
    const bool presentation_ok = start_presentation();
    g_boot_readiness = {true, presentation_ok,
                        storage_ok && g_store.state().configuration.status !=
                                          app_model::ConfigurationStatus::StorageFailure,
                        !hardware_ok ||
                            g_store.state().device_fault != app_model::DeviceFaultReason::None};

    // §15.1 steps 13, 17 and 18. After the panel so a device with no network still lights
    // up, and before the boot resolves so the station is associating while the rest starts.
    const auto network_heap_before = platform::heap_report();
    start_connectivity(loaded);
    const auto network_heap_after = platform::heap_report();
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kApp,
        "network startup heap internal_before=" +
            std::to_string(network_heap_before.free_internal_bytes) + " internal_after=" +
            std::to_string(network_heap_after.free_internal_bytes) + " largest_before=" +
            std::to_string(network_heap_before.largest_free_internal_block_bytes) +
            " largest_after=" +
            std::to_string(network_heap_after.largest_free_internal_block_bytes) +
            " internal_min_after=" +
            std::to_string(network_heap_after.minimum_free_internal_bytes) +
            " dma_before=" + std::to_string(network_heap_before.free_dma_internal_bytes) +
            " dma_after=" + std::to_string(network_heap_after.free_dma_internal_bytes) +
            " dma_largest_before=" +
            std::to_string(network_heap_before.largest_free_dma_internal_block_bytes) +
            " dma_largest_after=" +
            std::to_string(network_heap_after.largest_free_dma_internal_block_bytes) +
            " dma_min_after=" + std::to_string(network_heap_after.minimum_free_dma_internal_bytes) +
            " psram_before=" + std::to_string(network_heap_before.free_psram_bytes) +
            " psram_after=" + std::to_string(network_heap_after.free_psram_bytes));
    start_live_ota();

    // Hardware-validation profiles only; a no-op elsewhere.
    validation::start_panel_flow();

    start_input();

    enter_boot_phase(hardware_ok ? app_model::BootPhase::Ready : app_model::BootPhase::FatalError);

    if (!presentation_ok) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "boot completed without a renderer; the panel will stay dark");
    }
}

// --- Tasks -----------------------------------------------------------------

void app_task(void* /*argument*/) {
    // Recorded before anything dispatches, so the single-consumer check in dispatch()
    // is armed for the boot sequence too — which is where the rule was broken.
    g_app_task = xTaskGetCurrentTaskHandle();

    // The boot sequence runs here, not on the ESP-IDF main task, because it
    // dispatches — and a dispatch is deep. reduce() is a pure function over a 2 kB
    // AppState, so it puts a copy on the caller's stack; two of those plus an Event
    // temporary is most of the main task's 3584 bytes, and Phase B overflowed it on
    // the first boot for exactly that reason.
    //
    // Sizing a task for it is the right answer rather than raising the main task's
    // stack, because it also makes §10.2 unconditional: this task is the only thing
    // that ever dispatches into the store, during boot as well as after it. There is
    // no "boot is single-threaded so direct dispatch is fine" special case left to
    // reason about.
    boot_sequence();

    g_first_boot.begin(ota::device_boot_port());
    if (g_first_boot.pending()) {
        app_model::OtaObservation observation;
        observation.phase = app_model::OtaPhase::Validating;
        observation.pending_validation = true;
        dispatch(app_model::Event::ota_observed(platform::monotonic_time(), observation));
    } else if (g_first_boot.health() == ota::BootHealth::Failed) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "OTA running-image state unavailable; no confirmation attempted");
    }

    // Reported once, straight after the deepest path this task has: the boot
    // sequence, and then the fixture catalogue inside scenarios_begin().
    diagnostics::log_task_stack(kAppTaskName, kAppTaskStackBytes);

    validation::scenarios_begin(g_store, platform::monotonic_time());
    validation::walk_begin(g_store, platform::monotonic_time());
    validation::printing_timeline_begin(g_store, platform::monotonic_time());
    diagnostics::log_task_stack(kAppTaskName, kAppTaskStackBytes);

    TickType_t since_stack_report = 0;

    while (true) {
        app_model::Event event;

        if (xQueueReceive(g_events, &event, kAppTickInterval) == pdTRUE) {
            (void)g_store.dispatch(event);
            note_gesture(event);

            // Drain whatever else is queued without blocking, so a burst is applied
            // in one pass rather than one event per tick.
            while (xQueueReceive(g_events, &event, 0) == pdTRUE) {
                (void)g_store.dispatch(event);
                note_gesture(event);
            }
        } else {
            since_stack_report += kAppTickInterval;
        }

        // ⛔ ONE CLOCK READING FOR THE WHOLE ITERATION, AND THAT IS WHAT MAKES THE ORDER MEAN
        // SOMETHING — R3. The station's completion deadline and the Wi-Fi validation budget are both
        // 30 s and are armed from the SAME reading in begin_transaction(), so they expire at the
        // same instant. Two separate readings here — even in the right order — leave the boundary
        // falling between them for a few microseconds each iteration, which is a smaller version of
        // exactly the race this replaces. One reading removes the window rather than narrowing it.
        const std::uint32_t now_ms = monotonic_ms();

        // ⛔ THE STATION STEPS FIRST, AND ITS EVIDENCE IS CONSUMED IN THIS SAME ITERATION — R3.
        // Previously station_tick() ran after provisioning_tick() and its evidence was read one
        // iteration later, so at the shared 30 s boundary provisioning settled the generic
        // WifiTimedOut before the station's DriverStalled could be seen — and the actionable outcome
        // the owner ruled on never reached the transaction that was waiting. Ordered this way, the
        // station's verdict is already in hand when the validation machine is asked, and
        // WifiValidation::observe() acts on a terminal outcome BEFORE it consults the clock, so
        // DriverStalled wins that instant deterministically.
        //
        // ⬅️ THE COMMENT THAT USED TO SIT HERE CLAIMED THE ONE-TICK LAG AFFECTED NO DEADLINE. It was
        // wrong in this precise sense: the lag moved no deadline, but it decided which of two
        // same-instant deadlines was allowed to settle first.
        //
        // ⛔ AND THE EXCLUSIVE-OPERATION BARRIER IS STILL THE FIRST AUTHORITY. Station work does not
        // advance when it is forbidden; the permit is checked here rather than inside
        // provisioning_tick(), which is where it used to be reachable only for provisioning work.
        // §10.1 does not have a Wi-Fi task any more, so this is the only place that decision lives.
        if (exclusive::provisioning_work_permitted(exclusivity())) {
            station_tick(now_ms);
        }

        // Stepped from here rather than from a task of its own: this task is the
        // store's only dispatcher, and the scenario source dispatches.
        provisioning_tick(now_ms);

        // R6's persistent surface, stepped from the same place and for a related reason: the
        // station address it needs lives in the store, and this is the only task that may
        // read it without racing the reducer.
        local_interface_tick();
        live_ota_tick();

        // Milestone 4, stepped from here for the third instance of the same reason: all three
        // decide on state this task alone may read, and two of them write configuration.
        //
        // ⛔ THE ORDER IS THE FIX, NOT A PREFERENCE. Every reason a worker must stop is a state
        // change the drain above has just applied, so supersession runs BEFORE consumption: a
        // report produced under conditions that have since ended is then judged against a session
        // that already knows they ended. Launching last means a worker started this tick cannot
        // have its own first report consumed before its session is armed.
        printer_supervise();
        printer_ingest();
        printer_launch();

        // ⬅️ LAST, BECAUSE EVERY STATE THIS ITERATION WILL PRODUCE HAS BEEN PRODUCED BY HERE. The
        // panel is decided from the settled picture rather than from a half-stepped one.
        //
        // ⛔ WHAT IT SHARES WITH THE TICK IS THE DECISION READING, WHICH IS THE INVARIANT THAT
        // MATTERS. monotonic_ms()'s rule is about elapsed intervals: whoever owns a tick's
        // observation passes it down, and nothing that will later be SUBTRACTED from it reads the
        // clock again. display_tick() honours that exactly — g_display_activity_ms is recorded
        // from this now_ms and compared against this now_ms, so the recession's arithmetic
        // involves one reading and cannot underflow the way the T4 access point did.
        //
        // ⬅️ ITS DISPATCHES DO READ THE CLOCK AGAIN, AND THAT IS THE FILE'S CONVENTION RATHER THAN
        // a lapse: every dispatch site here — boot_phase, hardware_probed, storage_observed,
        // device_fault, configuration_observed, provisioning_phase — stamps its event with
        // platform::monotonic_time() at the moment the event is created, because that is what the
        // moment MEANS. The reducer advances time from it deliberately ("a rejected snapshot still
        // leaves time advanced, and confidence depends on time"). Making the display path alone
        // stamp events with an older reading would put it out of step with eleven siblings and
        // would backdate the derivation, not protect it.
        first_boot_tick(now_ms);
        display_tick(now_ms);

        validation::scenarios_tick(g_store, platform::monotonic_time());
        validation::walk_tick(g_store, platform::monotonic_time());
        validation::printing_timeline_tick(g_store, platform::monotonic_time());

        if (since_stack_report >= kStackReportInterval) {
            since_stack_report = 0;
            diagnostics::log_task_stack(kAppTaskName, kAppTaskStackBytes);
        }
    }
}

void button_task(void* /*argument*/) {
    // Initialised here, on this task's own stack, and not by whoever created it.
    // See the stack note on kButtonTaskStackBytes: this is deep enough to overflow
    // the ESP-IDF main task, and it did.
    //
    // Ordering is preserved: the task is created after the board probe and the
    // boot-phase transition, so the BOOT button's GPIO is still configured only once
    // normal application startup has completed (Architecture Spec §12).
    if (!button::init()) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kApp,
                         "button unavailable; continuing without physical input");
        vTaskDelete(nullptr);
        return;
    }

    // ⬅️ STARTED HERE, ON THE TASK THAT WILL ADVANCE IT. Every press arrives on this task, so
    // beginning here gives the harness's step index exactly one owner and nothing to serialise.
    // A no-op unless PRD_VALIDATION_BRIGHTNESS_HARNESS.
    validation::brightness_harness_begin(platform::monotonic_time());

    while (true) {
        button::Event event = button::Event::None;

        // A timeout rather than an indefinite wait, so the untouched line is reported
        // periodically: a validation run has to be able to show that nothing was
        // classified while nobody touched the button.
        if (!button::wait_for_event(event, kButtonIdleReportMs)) {
            validation::log_button_idle();
            continue;
        }

        // Bring-up logging first, so the raw classification is on record before any
        // product meaning is attached to it. A no-op outside the validation profile.
        validation::log_button_gesture(event);

        const auto gesture = to_gesture(event);
        if (!gesture.has_value()) {
            continue;
        }

        // The product fact. What it means — wake, Settings, reset confirmation — is
        // the reducer's decision, not this task's.
        //
        // ⛔ UNLESS A DIAGNOSTIC FLOW HAS CONSUMED IT. The scenario sweep and the operational walk
        // only READ the model, so a press keeps its full product meaning there and the model sees
        // exactly what it would without a bench control. The Printing timeline's controls are
        // ACTIONS — step, pause, restart — and a diagnostic control must not also sleep the panel,
        // raise the setup surface, or arm a factory reset. So when it is compiled in, the gesture
        // never becomes a product fact at all: the reducer does not see it, and note_gesture()
        // below is not reached either, because nothing was dispatched.
        if (!validation::gesture_is_consumed(*gesture)) {
            (void)post_event(
                app_model::Event::button_gesture(platform::monotonic_time(), *gesture));
        }

        // And, in the validation profile only, the same press drives the scenario
        // flow's bench controls.
        validation::on_gesture(*gesture);
        validation::walk_on_gesture(*gesture);
        validation::printing_timeline_on_gesture(*gesture);
        validation::brightness_harness_on_gesture(*gesture, platform::monotonic_time());

        // Reported here rather than on a timer: this is the end of this task's
        // deepest path, and it is naturally rate-limited by a human finger.
        diagnostics::log_task_stack(kButtonTaskName, kButtonTaskStackBytes);
    }
}

// --- Service startup -------------------------------------------------------

void start_input() {
#if defined(CONFIG_PRD_BUTTON_ENABLED)
    // Creates the task and nothing else. Every deep call — the GPIO configuration,
    // the ISR service, the queues — happens on that task's stack rather than on the
    // main task's 3584 bytes. Pinned to core 0: see kButtonTaskCore.
    const BaseType_t created =
        xTaskCreatePinnedToCore(button_task, kButtonTaskName, kButtonTaskStackBytes, nullptr,
                                kButtonTaskPriority, nullptr, kButtonTaskCore);

    if (created != pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to create button consumer task");
    }
#endif
}

bool start_application_task() {
    g_events = xQueueCreate(kEventQueueLength, sizeof(app_model::Event));

    // Depth one, overwrite semantics: a second submission replaces a first the application
    // task has not taken yet rather than queueing behind it. The queue's storage holds a
    // credential between the two tasks and is wiped as soon as it is taken.
    // One POINTER-wide slot, from the constant the static_asserts guard. sizeof a Submission never
    // enters this call again — see g_submissions.
    g_submissions = xQueueCreate(1, kSubmissionItemBytes);
    g_printer_reports = xQueueCreate(kPrinterReportSlots, sizeof(prusalink::Report));

    // ⬅️ EVERY QUEUE IS CHECKED, AND THE MESSAGE NAMES WHICH ONE. §16.4 requires a typed report on
    // allocation failure rather than a silent degradation. An unchecked report queue was the worse
    // failure: the verdict would be dropped and every validation would then fail after its deadline
    // with "unreachable", blaming a printer that had answered perfectly.
    if (g_events == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to allocate the application event queue");
        return false;
    }

    if (g_submissions == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to allocate the provisioning submission queue");
        return false;
    }

    if (g_printer_reports == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to allocate the printer report queue");
        return false;
    }

    const BaseType_t created =
        xTaskCreate(app_task, kAppTaskName, kAppTaskStackBytes, nullptr, kAppTaskPriority, nullptr);

    if (created != pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to create the application task");
        return false;
    }

    // No log line here on purpose: the task itself reports, so the product line stays
    // the first thing in the boot log as it is in every committed evidence file.
    return true;
}

// Reports the detected hardware and records it as a fact. §12.4's mismatch handling
// lives in the reducer: a failed capacity check raises a device fault there, which
// decides what would be presented without aborting the boot — §19.8 acceptance is a
// bench decision, not a boot abort.
bool report_hardware() {
    const auto probe = board::probe_hardware();

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kBoard,
                     "target=" + probe.identity.target + " validation=" + probe.validation_status);

    const auto& platform_info = probe.platform;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kBoard,
                     "chip=" +
                         (platform_info.chip_model.empty() ? std::string{"unavailable"}
                                                           : platform_info.chip_model) +
                         " rev=" + std::to_string(platform_info.chip_revision_major) + "." +
                         std::to_string(platform_info.chip_revision_minor) +
                         " cores=" + std::to_string(platform_info.cpu_cores) +
                         " reset=" + platform_info.reset_reason);

    log_capacity_check(probe.flash);
    log_capacity_check(probe.psram);

    diagnostics::log(level_for(probe.usb_console_result), diagnostics::tag::kBoard,
                     "usb_console=" + probe.usb_console + " result=" +
                         std::string{app_model::validation_result_label(probe.usb_console_result)});

    app_model::HardwareProbeObservation observation;
    observation.board_target.assign(probe.identity.target);
    observation.reset_reason.assign(platform_info.reset_reason);
    observation.flash_validation = probe.flash.result;
    observation.psram_validation = probe.psram.result;

    dispatch(app_model::Event::hardware_probed(platform::monotonic_time(), observation));

    return probe.flash.result != app_model::ValidationState::Failed &&
           probe.psram.result != app_model::ValidationState::Failed;
}

// §15.1 step 6, and Milestone 3.1's whole product path.
//
// The component logs its own outcome; what happens here is the two dispatches, because
// only this task may touch the store.
//
// A STORAGE FAILURE DOES NOT FAIL THE BOOT, and that is the same argument the renderer
// gets a few lines above. §10 makes DeviceFault the highest presentation priority, so the
// condition is already stated through the modelled path and reaching FatalError as well
// would report one fact twice through two mechanisms. More practically, §15.3 and §15.4
// both turn on a device with a broken subsystem remaining diagnosable and updatable:
// a device that cannot persist configuration can still be read over USB, can still take
// an OTA, and can still be factory-reset — and stopping the boot would remove every one
// of those routes to fixing it. The hardware probe is treated differently on purpose:
// a board that is not the board we compiled for invalidates the rest of the boot.
bool bring_up_storage() {
    const storage::Report report = storage::initialise();

    dispatch(app_model::Event::storage_observed(platform::monotonic_time(), report.status));

    if (report.status == app_model::StorageStatus::Failed) {
        dispatch(app_model::Event::device_fault(platform::monotonic_time(),
                                                app_model::DeviceFaultReason::StorageFailure));
        return false;
    }

    return true;
}

// Ends the boot's storage phase in a device-owned fault, and reads nothing further.
//
// ⛔ ONE DISPATCH AND ONE LINE. There is deliberately no retry, no second probe and no restart: the
// store has just refused to answer or to finish a job, and both §16.2's "do not continuously retry
// confirmed permanent storage failures" and ADR-0008's no-automatic-reboot rule point the same way.
// The recovery is an explicit restart by the user, which is what the panel asks for.
void halt_boot_faulted(const app_model::DeviceFaultReason reason, const std::string_view note) {
    dispatch(app_model::Event::device_fault(platform::monotonic_time(), reason));

    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kConfig,
                     "boot halted before the configuration was read: fault=" +
                         std::string{app_model::log_label(reason)} + "; " + std::string{note});
}

// §15.1 step 10, and the step that completes Milestone 3's first vertical slice: with no
// record on the flash the observation carries ConfigurationStatus::Missing, the reducer
// turns that into ProvisioningPhase::SetupRequired (§14.1), and §12.3 resolves the
// presentation to RecoveryOrProvisioning. Every layer between already existed.
//
// The component logs nothing; the line below is the only account of what was loaded,
// because only this task may touch the store and the projection belongs beside the
// dispatch that carries it.
// Resolves a pair activation that a previous boot may have been interrupted inside.
//
// ⛔ RUNS BEFORE THE CONFIGURATION IS READ, AND THAT ORDER IS THE POINT. §13.3's activation is two
// pointer writes; a power loss between them leaves config naming the new profile and credentials
// naming the old, which every reader downstream would see as un-provisioned — the user's working
// configuration apparently gone. So the repair happens first, and load_configuration() reads a pair
// that has been proved whole.
//
// ⬅️ THE WHOLE TRANSACTION IS candidate::reconcile()'s, AND THAT IS DELIBERATE. It used to be a pure
// enum decision here plus the writes, the generation arithmetic and the clear inline in this
// untestable file — which is where all three of its defects lived. This function is now the binding
// and nothing else: it calls the executor, reports what happened, and returns whether the store may
// be read normally.
[[nodiscard]] bool reconcile_pair_activation() {
    const candidate::Reconciliation reconciliation = candidate::reconcile(storage::key_value());

    if (reconciliation.outcome == candidate::Outcome::Nothing) {
        return true;
    }

    const diagnostics::LogLevel level = candidate::resolved(reconciliation.outcome)
                                            ? diagnostics::LogLevel::Warning
                                            : diagnostics::LogLevel::Error;

    diagnostics::log(level, diagnostics::tag::kConfig,
                     "pair activation was interrupted: outcome=" +
                         std::string{candidate::log_label(reconciliation.outcome)} + " profile=" +
                         std::to_string(reconciliation.profile) + " recovery-record=" +
                         (reconciliation.marker_retained ? "retained" : "cleared"));

    if (candidate::resolved(reconciliation.outcome)) {
        return true;
    }

    // ⛔ NOTHING DOWNSTREAM MAY READ THE STORE. Neither the target pair nor the exact previous pair
    // could be proved whole, so what is in flash may be mixed — and a mixed pair presented as a
    // configuration is precisely the failure the recovery record exists to prevent. The evidence is
    // still there for a later boot; this one goes to the storage/recovery path.
    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kConfig,
                     "configuration not readable: an interrupted pair activation could not be "
                     "resolved and the recovery record is retained");

    return false;
}

ConfigurationLoad load_configuration() {
    // §13.3 step 7's repair, before anything reads the pair.
    const bool readable = reconcile_pair_activation();

    // ⬅️ AN UNRESOLVED ACTIVATION IS A STORAGE FAILURE, REPORTED THROUGH THE PATH THAT ALREADY
    // EXISTS. Substituting the outcome rather than reading the records is what keeps a mixed pair
    // from ever reaching compose_configuration(); StorageFailure then produces
    // ConfigurationStatus::StorageFailure and decide_provisioning() defers to recovery, which is the
    // accepted behaviour for a store that cannot be trusted. Nothing is erased (R10).
    config::LoadReport configuration;
    secrets::LoadReport secret;

    if (readable) {
        configuration = config::load(storage::key_value());
        secret = secrets::load(storage::key_value());
    } else {
        configuration.outcome = config::LoadOutcome::StorageFailure;
        secret.outcome = secrets::LoadOutcome::StorageFailure;
    }

    const app_model::ConfigurationObservation observation =
        compose_configuration(configuration, secret);

    // TWO LINES, AND NEITHER CAN CARRY A CREDENTIAL.
    //
    // The first reports the non-secret record and is built from values that are non-secret
    // by §5.9's definition — the printer address and alias are already in
    // ConfigurationState for the same reason.
    //
    // The second is built by secrets::describe(), which is handed an outcome, a generation
    // and five booleans and is given no access to any value. That is redaction at the call
    // site done structurally (owner ruling, 2026-08-15): there is nothing in scope to leak,
    // rather than a filter that has to catch it afterwards.
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kConfig,
        "configuration outcome=" + std::string{config::log_label(configuration.outcome)} +
            " status=" + std::string{app_model::log_label(observation.status)} +
            " generation=" + std::to_string(configuration.generation) +
            " schema=" + std::to_string(static_cast<unsigned>(observation.schema_version)) +
            " wifi=" + (observation.has_wifi_credentials ? "yes" : "no") + " printer=" +
            (observation.has_printer_configuration
                 ? "active"
                 : (observation.printer_configuration_pending ? "pending" : "none")));

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kSecrets,
        secrets::describe(secret.outcome, secret.generation, secrets::presence_of(secret.value)));

    dispatch(app_model::Event::configuration_observed(platform::monotonic_time(), observation));

    ConfigurationLoad loaded;
    loaded.secret = secret;
    loaded.wifi_credentials_usable = observation.has_wifi_credentials;
    loaded.decision = decide_provisioning(configuration.outcome, secret.outcome,
                                          observation.has_wifi_credentials);

    // The one assignment, so the remembered decision cannot disagree with the load it came from.
    g_last_provisioning_decision = loaded.decision;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "provisioning decision=" + std::string{log_label(loaded.decision)});

    return loaded;
}

// ⛔ THERE IS NO OBSERVATION SINK ANY MORE, AND ITS REMOVAL IS CHECKPOINT 3c. The station used to
// run on its own task, so it handed observations to a callback that put them on §10.7's bounded
// queue for this task to apply. Two consequences followed and both were real: Wi-Fi truth travelled
// through a queue that is lossy by design, and a candidate's verdict was decided from a phase this
// task read one or more ticks after the event that produced it — which is what made the previous
// WifiValidation guard necessary. The station now steps on THIS task inside station_tick(), and
// its observation is dispatched directly.

// §15.1 steps 17 and 18 — "if unconfigured: enter provisioning; if configured: connect".
//
// THE TWO ROLES ARE MUTUALLY EXCLUSIVE IN MILESTONE 3.5 (owner ruling D4, AP-only), and the
// exclusion is structural rather than a convention: decide_provisioning() returns StartSetup
// only when there is no usable credential, and network::start() with no credential returns
// at its empty-SSID guard without touching the radio. So the unconditional start_network()
// below cannot take the driver away from an access point that just claimed it.
//
// It is unconditional on purpose. With no usable credential it publishes NetworkPhase::
// Disabled — the correct observable state for a device that is provisioning or waiting for
// recovery, and better than a phase that was never published at all — and it is also what
// clears the stored password out of `loaded`.
void start_connectivity(ConfigurationLoad& loaded) {
    switch (loaded.decision) {
    case ProvisioningDecision::StartSetup:
        // The boot path owns no tick, so this is where its one observation is taken.
        start_setup_ap(monotonic_ms());
        break;

    case ProvisioningDecision::DeferredToRecovery:
        // ⬅️ MILESTONE 3.10, OWNER DECISION 3. §14.1's fourth entry condition and §13.4's
        // "unrecoverable non-secret corruption enters provisioning/recovery mode", reconciled
        // with the physical-presence boundary D2 and R1 were protecting: the RECOVERY STATE is
        // automatic, the privileged surface is not.
        //
        // ⛔ NO ACCESS POINT, NO MUTABLE SURFACE, AND NOTHING ERASED. A faulted record is not a
        // reason to start advertising a setup network, and on StorageFailure it is not a reason
        // to write to a store that has just failed. The panel says what happened and what to do;
        // a deliberate long press is what raises the access point.
        dispatch(app_model::Event::recovery_mode_entered(platform::monotonic_time()));

        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kConfig,
                         "stored configuration is faulted; entering recovery. nothing is erased "
                         "and no setup network is raised without a deliberate long press");
        break;

    case ProvisioningDecision::NotRequired:
        break;
    }

    start_network(loaded);
}

// §14.1's first entry condition and §14.2's setup access point.
//
// The session is generated here and kept in g_setup_session for the rest of the boot —
// R1's per-session credential, which M3.6 will display. Nothing else reads it, and by owner
// ruling D1 nothing prints it, so THIS CHECKPOINT CANNOT BE USED TO PROVISION A DEVICE:
// the access point is up and its password is unknowable until the panel shows it. That is
// the accepted shape of an infrastructure checkpoint, not an oversight.
// The five operations setup_entry::begin() drives, bound to the real adapters.
//
// ⛔ THEY ARE PASSED, NOT CALLED IN ORDER HERE, AND THAT IS THE POINT. Milestone 3.11 T4's F-B was
// this function performing them in the wrong order, with no local check that could catch it. The
// order now lives in setup_entry.cpp, where it is host-tested; what is left here is the binding.
bool step_start_access_point(void* /*context*/) {
    if (network::setup_ap::start(g_setup_session)) {
        return true;
    }

    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                     "setup access point did not start; the device cannot be provisioned "
                     "on this boot");
    return false;
}

void step_publish_addresses(void* /*context*/) {
    // refresh_reachability()'s job, and it stays refresh_reachability()'s job — the end-of-tick
    // call remains and becomes a no-op, because it only speaks when something changed.
    refresh_reachability();
}

bool step_start_server(void* /*context*/) {
    // §5.11's local configuration surface, on the address the access point actually answers on.
    // Started after the access point because it has nowhere to bind before that.
    provisioning::Config server;
    server.address = network::setup_ap::address();
    server.sink = &note_submission;
    server.update_sink = &note_update_request;

    if (provisioning::start(server)) {
        return true;
    }

    diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                     "setup server unavailable; the access point is up but there is no "
                     "page to serve and the panel shows no address");
    return false;
}

// §14.2: "show the setup SSID and local URL on the display". THE PANEL IS THE ONLY CHANNEL for
// the password — owner ruling D1 left no serial path, so this call is the entire reason a person
// can join the network Milestone 3.5 raises.
//
// It travels its own queue and never touches the store: a credential in AppState would breach
// §24.3, and this file's own rule is that no stored credential outlives the call that hands it to
// a driver. The generated session is the deliberate exception, and it goes exactly two places —
// the Wi-Fi driver, and here.
//
// ⬅️ THE ADDRESS IS WHATEVER THE PUBLISHED SET NOW DERIVES. provisioning::url() is empty when no
// access-point address is active, and the scene hides a row whose value is absent — so a device
// with nothing listening shows a joinable network and no address, rather than an address that
// would refuse the connection.
void step_submit_session(void* /*context*/) {
    ui::SetupSession visible;
    visible.ssid = g_setup_session.ssid;
    visible.password = g_setup_session.password;
    visible.url.assign(provisioning::url());
    ui::submit_setup_session(visible);
    // The UI mailbox owns its copied item now; this temporary app-task copy
    // must not remain in a coredump after the step returns.
    wipe_bytes(&visible, sizeof(visible));
}

void step_discard_session(void* /*context*/) {
    g_setup_session.clear();
    ui::clear_setup_session();
}

constexpr setup_entry::Steps kSetupSteps{
    &step_start_access_point, &step_publish_addresses, &step_start_server,
    &step_submit_session,     &step_discard_session,
};

void start_setup_ap(const std::uint32_t now_ms) {
    // ⛔ THE SERVER'S SNAPSHOT IS SEEDED FIRST, BEFORE ANYTHING BELOW CAN BE REACHED — F-2, found on
    // the board on 2026-08-26. Nothing on this path used to publish a setup state at all, so a fresh
    // session inherited the PREVIOUS one's terminal snapshot. After a validated Wi-Fi stage that
    // meant WifiValidated: a browser joining the new setup network was shown step 2, and
    // admits_printer_stage() accepted a printer POST on the strength of the same stale value — while
    // stop_setup_ap() had already cleared g_wifi_validated, so provisioning_tick() refused it as
    // "no validated wi-fi stage". A step nobody could finish, and no route back to step 1.
    //
    // ⬅️ THE CREDENTIAL WAS NEVER THE PROBLEM, AND THIS IS NOT A WIPE. stop_setup_ap() is the
    // authoritative terminal wipe — g_wifi_validated = false and wipe_submission(g_submission) —
    // and it stays the only one. What was missing was telling the server that the flow had gone back
    // to the beginning. This is that one statement and nothing more.
    //
    // ⛔ FIRST, NOT LAST, AND THAT IS THE WHOLE PLACEMENT ARGUMENT. R6 keeps the local server alive
    // across sessions, so a request can arrive before this function has started an access point:
    // publishing after setup_entry::begin() would leave a window in which the LAN surface still
    // answered with the old session's state. Seeded here, the snapshot is correct from the first
    // instant of the new session whether the server is being started or has been running all along.
    provisioning::publish(provisioning::SetupState::Editing, {});

    dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(),
                                                  app_model::ProvisioningPhase::SetupRequired));

    g_setup_session = network::setup_ap::new_session();

    const setup_entry::Outcome outcome = setup_entry::begin(kSetupSteps, nullptr);

    if (outcome == setup_entry::Outcome::AccessPointUnavailable) {
        // The phase stays SetupRequired. SetupFailed describes a setup flow that ran and did not
        // succeed; there is no flow to have failed here, and claiming one would put a state in
        // front of the user that nothing can clear.
        return;
    }

    // ⬅️ THE PRIVILEGED WINDOW OPENS HERE, and it is bounded from this moment — owner rulings 2
    // and C. Every entry, first boot or deliberate re-entry, gets its own window; nothing carries
    // over from a previous one, exactly as nothing carries over from a previous session password.
    //
    // ⬅️ THE CALLER'S OBSERVATION, NOT A FRESH ONE. See monotonic_ms(): the expiry check that
    // runs later in the same tick subtracts this from the tick's own reading, so the two must be
    // the same observation or the interval is meaningless.
    g_privileged.begin(now_ms);

    dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(),
                                                  app_model::ProvisioningPhase::SetupActive));
}

// A validated submission, handed over by the provisioning server on ITS task.
//
// ⛔ IT RUNS ON THE SERVER TASK, SO IT MAY NOT DISPATCH (§10.2). It does the one thing a
// foreign task may do: leave the submission where the application task will find it, and
// return. Everything the submission causes — the phase change, the join, the transaction —
// happens on the application task, which remains the store's only dispatcher.
//
// ⛔ IT LOGS PRESENCE, NEVER A VALUE. §13.5 keeps credentials out of logs, and this
// function is handed four of them.
provisioning::SubmissionHandoff note_submission(const provisioning::Submitted& submitted,
                                                void* /*context*/) {
    SubmissionOps ops;

    // ⛔ THE OBJECT IS BUILT INSIDE publish(), BEFORE THE MAILBOX IS TOUCHED. That ordering is what
    // makes "a submission that cannot be built completely must not disturb one that already exists"
    // a property of the code rather than of this call site's luck.
    //
    // ⛔ AND EVERY assign() IS CHECKED. It used to be four (void) casts. The server's Submitted and
    // the flow's Submission size their Wi-Fi password fields differently — 63 against 64, WPA2's
    // passphrase limit against what SecretConfigV1 stores — so the bounds are deliberately unequal
    // and a discarded result would silently truncate a credential, handing the driver a passphrase
    // nobody typed and then reporting the access point's rejection as the user's mistake.
    const submission_handoff::Report report =
        submission_handoff::publish(ops, [&submitted](provisioning_flow::Submission& fresh) {
            fresh.stage = submitted.stage == provisioning::SubmissionStage::Printer
                              ? provisioning_flow::Stage::Printer
                              : provisioning_flow::Stage::Wifi;
            fresh.printer_port = submitted.printer_port;

            // Operand order puts the call first, so every field is assigned whatever an earlier
            // one returned and `fits` can only narrow.
            bool fits = true;
            fits = fresh.wifi_ssid.assign(submitted.wifi_ssid.view()) && fits;
            fits = fresh.wifi_password.assign(submitted.wifi_password.view()) && fits;
            fits = fresh.printer_host.assign(submitted.printer_host.view()) && fits;
            fits = fresh.printer_api_key.assign(submitted.printer_api_key.view()) && fits;
            return fits;
        });

    const bool accepted = report.outcome == submission_handoff::Outcome::Accepted;

    if (!accepted) {
        note_submission_unavailable();
    }

    // ⛔ THE HEAP IS GONE. RETURN WITHOUT ALLOCATING ANOTHER BYTE.
    //
    // This early return is the whole of the exhaustion contract, and the code below is why it has
    // to exist. Every line after it builds a std::string, concatenates, or calls std::to_string —
    // and diagnostics::log() itself materialises its tag as a std::string before it reaches
    // ESP_LOGx. On the one path where `new (std::nothrow)` has just answered "there is no heap",
    // any of those could throw std::bad_alloc into a build compiled with -fno-exceptions, which
    // aborts the device over a form submission. std::nothrow only buys an ordinary 503 if nothing
    // between the null and the response asks the allocator for anything.
    //
    // ⬅️ SO THE COUNTER IS THE WHOLE RECORD HERE, AND THAT IS DELIBERATE. It was incremented above
    // without being formatted, and it is reported on the next line that CAN afford to allocate —
    // the accepted path below carries `total_unavailable=`, so a count taken during exhaustion is
    // surfaced as soon as the device is healthy enough to say it. No line is emitted now.
    //
    // ⚠️ THE HONEST BOUND. This guarantees no allocation in PROJECT code. httpd_resp_send() and the
    // three httpd_resp_set_* calls that answer the request are ESP-IDF's, and their internal
    // behaviour is not provable from the pinned source; the claim is about our code, not about
    // esp_http_server. See handle_submit()'s 503 branch, which is the rest of this path.
    if (report.failure == submission_handoff::Failure::AllocationFailed) {
        return provisioning::SubmissionHandoff::Unavailable;
    }

    const auto yes_no = [](const bool present) { return present ? "yes" : "no"; };

    // ⛔ LOGGED AFTER THE HANDOFF, WITH ITS ACTUAL RESULT. This line used to end in "(queued for
    // validation)" and was written BEFORE the enqueue — so it asserted a queueing that had not
    // happened yet and that the very next statement could fail to perform. Presence booleans and a
    // failure class only; not one submitted byte, which is §13.5 and §24.4.
    //
    // ⬅️ AND EVERY PATH THAT REACHES THIS LINE HAS A WORKING HEAP. Allocation failure returned
    // above; what is left is a bounded field that would not fit, a mailbox that refused, or a
    // successful handoff. None of those three is represented as heap-exhaustion-safe, so they may
    // allocate to say what happened.
    diagnostics::log(
        accepted ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Warning,
        diagnostics::tag::kProvisioning,
        std::string{"setup submission received ssid="} + yes_no(!submitted.wifi_ssid.empty()) +
            " wifi_password=" + yes_no(!submitted.wifi_password.empty()) +
            " host=" + yes_no(!submitted.printer_host.empty()) +
            " api_key=" + yes_no(!submitted.printer_api_key.empty()) +
            " port=" + std::to_string(submitted.printer_port) +
            (accepted
                 ? std::string{" (handed to the application task"} +
                       (report.displaced ? ", superseding one still pending" : "") +
                       "; total_unavailable=" + std::to_string(g_submissions_unavailable) + ")"
                 : std::string{" (NOT handed over: "} +
                       submission_handoff::log_label(report.failure) + "; nothing started, " +
                       "total_unavailable=" + std::to_string(g_submissions_unavailable) + ")"));

    return accepted ? provisioning::SubmissionHandoff::Accepted
                    : provisioning::SubmissionHandoff::Unavailable;
}

// Wipes a submission in place, through the same volatile-store discipline the driver
// adapters use on wifi_config_t.
void wipe_submission(provisioning_flow::Submission& submission) {
    wipe_bytes(&submission, sizeof(submission));
}

// Ends a transaction: publishes the phase, clears the retained credential, and lets the
// browser see the result before anything else moves.
void settle_transaction(const provisioning_flow::Verdict verdict,
                        const provisioning_flow::Failure failure) {
    g_transaction_running = false;
    g_validation.reset();
    g_wifi_validated = false;
    wipe_submission(g_submission);

    // ⛔ THE RADIO MUST NOT OUTLIVE THE CANDIDATE THAT CONFIGURED IT — checkpoint 3e, §13.3 step 7.
    // Three outcomes, and they are genuinely different:
    //
    //   • Validated — the pair was ACTIVATED, so what the driver holds is what flash holds. Nothing
    //     is owed and the record of an unproven credential goes.
    //   • PromotionFailed — activate_pair() got past the recovery record, so a pointer may have
    //     moved and the record is still in flash for the next boot to resolve. A restore decision
    //     taken from half-moved pointers would be acting on a store that is mid-activation, and
    //     boot reconciliation is what owns that state. Nothing is scheduled, and `held` is left
    //     alone so a LATER abandoned candidate still schedules one.
    //   • anything else — the candidate is abandoned. Put the radio back.
    if (verdict == provisioning_flow::Verdict::Validated) {
        g_rollback = RollbackTarget{};
        g_station_restore_due = false;
    } else if (g_rollback.held && failure != provisioning_flow::Failure::PromotionFailed) {
        g_station_restore_due = true;
    }

    // Any printer-validation attempt belongs to the transaction that is ending. Superseding its
    // lease is immediate and does not block, so a late answer cannot settle a transaction twice
    // — including the case where a new Wi-Fi submission arrives mid-validation.
    // ⛔ ONE ASSIGNMENT, AND THAT IS THE POINT. Session, staged pair and deadline are forgotten
    // together, so no boundary can clear a subset of them. Superseding the lease is immediate and
    // does not block, so a late answer cannot settle a transaction twice — including the case where
    // a new Wi-Fi submission arrives mid-validation.
    g_printer_validation = printer_session::Validation{};
    prusalink::stop();
    drain_printer_reports();

    provisioning::publish(verdict == provisioning_flow::Verdict::Validated
                              ? provisioning::SetupState::Succeeded
                              : provisioning::SetupState::Failed,
                          provisioning_flow::log_label(failure));

    if (verdict == provisioning_flow::Verdict::Validated) {
        // ⬅️ D-2'S ORDER. The phase reaches success first, the browser gets its poll during
        // the grace period, and only then does the access point stop. The browser must not
        // need another successful request after shutdown to learn that setup worked.
        dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(),
                                                      app_model::ProvisioningPhase::Inactive));

        g_access_point_stop_due_us = esp_timer_get_time() + kAccessPointGraceUs;

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "setup complete; the access point stops after the browser has seen "
                         "the result");
        return;
    }

    // ⬅️ D-6: SetupFailed IS RECOVERABLE. The access point stays up, the server keeps
    // serving, the form stays editable, and nothing new was persisted. The phase remains
    // SetupFailed until the user submits again rather than bouncing back to SetupActive,
    // because a failure that erased itself would leave them with no idea what happened.
    dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(),
                                                  app_model::ProvisioningPhase::SetupFailed));

    // ⛔ THE PRECISE TRUTH, BECAUSE THE OLD SENTENCE WAS FALSE. It said "nothing was persisted",
    // and by this point stage_pair() has written BOTH candidate records to flash and read them back
    // byte for byte. What is true is narrower and more useful: the candidate is in flash and
    // inactive, so the pair the device is running has not changed.
    //
    // PromotionFailed is the exception and must not make that claim. It means activate_pair() got
    // past the recovery record, so a pointer may have moved and the recovery record is still in
    // flash for the next boot to resolve. Saying the previous pair is unchanged would be a guess
    // about which write failed.
    const bool activation_attempted = failure == provisioning_flow::Failure::PromotionFailed;

    diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                     "setup failed reason=" + std::string{provisioning_flow::log_label(failure)} +
                         (activation_attempted
                              ? "; activation was interrupted and recovery evidence remains for "
                                "boot reconciliation"
                              : "; the candidate is staged but not activated and the previous "
                                "active configuration is unchanged") +
                         "; the setup network is still up");
}

// Begins a transaction: takes ownership of the credential, moves to
// ValidatingConfiguration, and starts the station underneath the running access point.
// ⛔ THE TICK'S READING IS A PARAMETER, NOT SOMETHING THIS FUNCTION FETCHES — R11, after the
// 2026-08-26 physical run. It used to convert the microsecond timer to milliseconds here for
// itself, which was a SECOND reading of the clock the caller had already read at the top of its
// iteration — and monotonic_ms() is now the only place in this file that conversion happens, so a
// grep for it is a standing check on this whole class of bug. Because that second reading was taken
// later than the caller's, `started_ms_` was always a few milliseconds AHEAD of the `now_ms` that
// provisioning_tick() then handed to WifiValidation::tick() inside the very same call — and
// elapsed_since() is unsigned, so `now - start` underflowed to about 4.29e9 ms and every submission
// was failed as WifiTimedOut one millisecond after being accepted. On the board the station went on
// to associate and lease an address perfectly while the browser was told the network had not
// finished connecting.
//
// The guard was already written down 900 lines above, on monotonic_ms(): two readings of that clock
// must never be compared. This is what honouring it looks like at this site — the caller owns the
// observation and passes it down, and nothing on this path reads the clock again.
void begin_transaction(const provisioning_flow::Submission& submission,
                       const std::uint32_t now_ms) {
    // ⛔ THE ROLLBACK TARGET IS ESTABLISHED BEFORE esp_wifi IS TOUCHED, AND A CANDIDATE THAT HAS NO
    // TARGET IS REFUSED OUTRIGHT — PM ruling, 2026-08-27. This is the last instant at which the
    // device is provably running the pair it would have to go back to, so it is the only honest
    // moment to read the pointers; the first version of this checkpoint read them AFTER the failure,
    // by which time a degraded store could no longer name a target it had once known.
    //
    // ⬅️ CAPTURED ONCE PER DISTURBANCE. A second submission supersedes the candidate but not the
    // target: the original capture was taken while the previous pair was still demonstrably active,
    // and re-reading now would substitute a later, weaker observation for it.
    //
    // ⛔ FIRST IN THIS FUNCTION, BEFORE ANYTHING IS COPIED, PUBLISHED OR SUPERSEDED. A refused
    // candidate must leave the device exactly as it found it: no Validating snapshot the browser
    // would then see replaced, no printer validation torn down on its behalf, and no credential
    // copied into g_submission. settle_transaction() below still supersedes and wipes, so nothing is
    // skipped — it is only that nothing was disturbed in order to refuse.
    if (!g_rollback.held) {
        const wifi_restore::Target target = wifi_restore::target_for(storage::key_value());

        if (target.action == wifi_restore::Action::Unavailable) {
            // ⛔ NOTHING IS SUBMITTED AND THE RADIO IS UNTOUCHED. The store cannot say which pair is
            // active, so joining the candidate would create a state the device could never explain or
            // undo. This is NOT WifiRestoreFailed: no restoration was attempted, and there is
            // therefore no statement about the user's network to make.
            diagnostics::log(
                diagnostics::LogLevel::Error, diagnostics::tag::kConfig,
                "setup refused before the radio was touched: the store cannot name the "
                "currently active pair, so there would be nothing to roll back to");

            settle_transaction(provisioning_flow::Verdict::Failed,
                               provisioning_flow::Failure::WifiRollbackUnavailable);
            return;
        }

        g_rollback.target = target;
    }

    // ⛔ A NEW SUBMISSION SUPERSEDES ANY PRINTER VALIDATION STILL RUNNING, EXPLICITLY. Before this,
    // a Wi-Fi stage arriving mid-validation left the old worker alive and its deadline armed, so its
    // late verdict could settle the transaction that had just replaced it. Three things are needed
    // and all three are here: the worker's lease is superseded so it cannot publish, the deadline is
    // disarmed so the tick stops waiting for it, and the transaction identity is cleared so a verdict
    // that slipped through matches nothing. None of these blocks.
    // ⬅️ AND THE STAGED PAIR GOES WITH THEM. This used to clear the identity and the deadline but
    // leave g_staged behind, so the next attempt's transaction would have been built from the
    // superseded transaction's candidate. One assignment makes that unrepresentable.
    g_printer_validation = printer_session::Validation{};
    prusalink::stop();
    drain_printer_reports();

    g_submission = submission;
    g_transaction_running = true;
    g_access_point_stop_due_us = 0;
    g_dwell_open = false;

    // The first record of a validation. Everything after it is measured from here.
    diagnostics::timeline(diagnostics::tag::kApp, "post-accepted");

    dispatch(app_model::Event::provisioning_phase(
        platform::monotonic_time(), app_model::ProvisioningPhase::ValidatingConfiguration));

    provisioning::publish(provisioning::SetupState::Validating, {});

    // ⛔ AND AN IN-FLIGHT RESTORATION IS SUPERSEDED BEFORE ANYTHING NEW IS SUBMITTED. A user
    // submission is the newer instruction, and a restore tag left armed could otherwise be settled by
    // evidence belonging to the candidate that replaced it.
    if (g_restore.armed()) {
        (void)g_restore.abandon();
        diagnostics::log(
            diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
            "a new submission supersedes the restoration in flight; no restore outcome "
            "is reported for it");
    }

    // ⬅️ THE STATION STARTS UNDER THE ACCESS POINT. network::station::submit() selects APSTA on
    // its own when the access point is running (owner ruling D5 keeps mode ownership there), so the
    // browser session survives the join.
    //
    // ⬅️ AND EVERY ACCEPTED STEP 1 SUBMISSION IS A FRESH ATTEMPT WITH A FRESH IDENTITY. There is no
    // longer a start()/restart() distinction to get wrong: one entry point mints one request
    // identity, and a submission that arrives while the previous attempt still owes a driver
    // completion is retained rather than forced through a disconnect the SDK documents no
    // completion for.
    network::Credentials credentials;
    credentials.ssid = g_submission.wifi_ssid.view();
    credentials.password = g_submission.wifi_password.view();

    const network::station::Tag tag = network::station::submit(credentials, now_ms);

    if (tag == network::station::kNoTag) {
        // ⛔ A QUARANTINED STATION IS NOT AN UNREACHABLE NETWORK, and telling the user it was
        // would send them to retype a password that was never tested.
        settle_transaction(provisioning_flow::Verdict::Failed,
                           network::station::quarantined()
                               ? provisioning_flow::Failure::WifiDriverStalled
                               : provisioning_flow::Failure::WifiUnreachable);
        return;
    }

    // ⛔ THE DRIVER NOW HOLDS AN UNPROVEN CREDENTIAL, AND A PENDING RESTORATION IS SUPERSEDED —
    // checkpoint 3e. Recorded only after the station ACCEPTED the intent: a refused submission moved
    // no radio, so scheduling a restoration for it would disconnect a station that is still on
    // whatever it was on.
    g_rollback.held = true;
    g_station_restore_due = false;

    // ⛔ THE TRANSACTION IS BOUND TO THAT IDENTITY AND TO NOTHING ELSE. Evidence charged to any
    // other identity — the attempt this one superseded, or a link event that belongs to a settled
    // transaction — advances time here and decides nothing. §8.1.
    g_validation.begin(now_ms, provisioning_flow::Candidate{tag});

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                     "validating submitted configuration tag=" + std::to_string(tag));
}

// Stage 1 succeeded — owner ruling 1.
//
// ⛔ NOTHING IS PERSISTED HERE, AND THAT IS THE WHOLE DESIGN. The submitted network has been
// joined, leased an address and held it for the dwell, so it is proven; the credential stays
// in RAM and the station stays joined while the user fills in the printer step. A power loss
// at this moment leaves the flash exactly as it was before provisioning started.
//
// The phase returns to SetupActive rather than gaining a new one (owner ruling 3): the setup
// network is still up and still joinable, which is what SetupActive means, and the browser
// owns the two-stage narrative.
void complete_wifi_stage() {
    g_transaction_running = false;
    g_validation.reset();
    g_wifi_validated = true;
    g_dwell_open = false;

    diagnostics::timeline(diagnostics::tag::kApp, "wifi-validated-published");

    provisioning::publish(provisioning::SetupState::WifiValidated, {});

    dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(),
                                                  app_model::ProvisioningPhase::SetupActive));

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                     "wi-fi stage validated; nothing has reached flash yet, awaiting the printer "
                     "step");
}

// §13.3 steps 1-4: stages the pair and hands off to the probe. Activates nothing.
//
// ⬅️ THIS USED TO BE commit(), AND THE DIFFERENCE IS THE WHOLE REPAIR. commit() wrote both records
// AND moved both active-generation pointers, so by the time the printer was probed the device was
// already running the unproven configuration and §13.3 step 7 had nothing left to retain. Now the
// records are written and verified, both pointers still name the previous pair, and what decides
// whether they move is whether the printer answers.
void commit_transaction() {
    // ⛔ THE PRINTER STAGE IS A TRANSACTION FROM HERE — PM ruling 2 of checkpoint 3e, and this one
    // assignment is the whole of it. ADR-0008 decision 5 says "a validation or commit in progress
    // suspends expiry outright", and until this line only the WI-FI stage ever set the flag that
    // says so: an accepted printer submission staged two records and ran a twenty-second printer
    // probe with the privileged window's idle clock still running against it. It was saved from
    // firing only by note_activity() having just reset that clock, which is a coincidence of
    // ordering rather than the rule the ADR states.
    //
    // ⬅️ AND IT IS SET BEFORE ANYTHING CAN EXPIRE IT. local_interface_tick() reads this flag through
    // set_transaction_running() LATER IN THE SAME ITERATION than the submission is consumed, so the
    // suspension is in force for the whole of the commit — including the case where staging fails
    // immediately, because settle_transaction() is what clears it again.
    //
    // ⛔ NO SECOND TIMER AND NO SECOND STATE. Reaching the printer step grants no window of its own:
    // only an allowed and accepted submission refreshes activity, exactly as before, and a user who
    // sits idle between the two stages is expired by the ordinary rule — which is what now restores
    // the previous pair or withdraws the station.
    g_transaction_running = true;

    const config::LoadReport current = config::load(storage::key_value());
    const std::uint32_t profile = provisioning_flow::next_profile_id(current.value.wifi_profile_id);

    provisioning_flow::Records records =
        provisioning_flow::compose(g_submission, current.value, profile);

    // ⬅️ THE FLAG DESCRIBES THE RECORD ONCE ACTIVE, AND ACTIVE IMPLIES VALIDATED. compose() writes
    // false because under R4 no probe existed and a stored record was always unproven. Under this
    // architecture a staged record is invisible — only load_staged() reaches it — and it becomes
    // visible only by an activation that runs solely on the success path. So the flag is true for
    // every state a reader can observe, which is what R8's "active, validated" is asking about.
    records.configuration.printer_validated = true;

    const provisioning_flow::StagedPair staged =
        provisioning_flow::stage_pair(storage::key_value(), records);

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kConfig,
                     std::string{"setup pair staged="} + (staged.staged ? "yes" : "no") +
                         " profile=" + std::to_string(staged.profile) + " config=" +
                         std::string{config::log_label(staged.configuration)} + " secret=" +
                         (staged.secret_attempted ? std::string{secrets::log_label(staged.secret)}
                                                  : std::string{"not-attempted"}) +
                         " (both candidates are in flash; neither pointer has moved)");

    if (!staged.staged) {
        settle_transaction(provisioning_flow::Verdict::Failed, staged.failure);
        return;
    }

    // ⛔ THE TWO ACCOUNTS OF THE PREVIOUS PAIR MUST AGREE, AND THIS IS WHERE THEY MEET — PM ruling,
    // 2026-08-27. stage_pair() captures the active generations for the recovery record; this
    // transaction captured them before it disturbed the radio. Both read the same two pointers, so
    // they agree whenever both reads succeeded — which is exactly why a disagreement matters: it
    // means the store moved underneath a live transaction and NEITHER account can be trusted as a
    // restore target.
    //
    // ⬅️ VERIFIED RATHER THAN ASSERTED. Assertions are compiled out of the release profile, and this
    // has to hold in the profile a user runs. The retained target is marked unestablished so the
    // restoration takes the rollback-unavailable route instead of acting on a number it cannot trust.
    if (!wifi_restore::agrees(g_rollback.target, staged.previous_configuration_generation,
                              staged.previous_secret_generation)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kConfig,
                         "the staged previous pair disagrees with the rollback target this "
                         "transaction retained; neither can be trusted, so nothing is activated");

        g_rollback.target.action = wifi_restore::Action::Unavailable;

        // ⛔ AND THE BROWSER IS TOLD EXACTLY ONCE. settle_transaction() below publishes this typed
        // failure immediately, which it must: that publication is what moves the page off Validating
        // while the user is watching. The radio, however, is still on the candidate and its
        // withdrawal is asynchronous — so the cleanup records the terminal settlement and does NOT
        // announce the same failure again.
        g_rollback.reported = true;

        settle_transaction(provisioning_flow::Verdict::Failed,
                           provisioning_flow::Failure::WifiRollbackUnavailable);
        return;
    }

    g_printer_validation.staged = staged;

    // ⛔ NO RELOAD HERE. ConfigurationState still describes the PREVIOUS pair, which is correct:
    // that is what the device is running, and it is what the panel must keep showing until the
    // candidate has earned its place.
    begin_printer_validation();
}

// §13.3 step 7's restore half, for the RADIO — checkpoint 3e.
//
// ⛔ THESE THREE FUNCTIONS ARE THE WHOLE OF THE ASYNCHRONOUS REPAIR, AND THEY DECIDE NOTHING. Each
// one routes owned facts into wifi_restore and acts on the typed answer that comes back; every rule
// about what an answer MEANS lives in that pure unit, where a host test can reach it. The first
// version of this checkpoint put those rules here, in a translation unit the host suite cannot
// compile, and treated the acceptance of an asynchronous intent as a completed restoration.

// Asks the station to come down and hands the supervisor the failure to report once it actually has.
//
// `carry` is Failure::None for the prescribed no-previous-pair outcome and WifiRollbackUnavailable
// when the target could not be established or its record could not be loaded — the unexplained
// candidate is taken down either way, and only the reported outcome differs.
void withdraw_station(const std::uint32_t now_ms, const provisioning_flow::Failure carry,
                      const bool already_reported) {
    if (!network::station::withdraw(now_ms)) {
        // Refused only while quarantined or while an exclusive operation holds the product state.
        // The first is the typed Device Fault's business and the second is the barrier's; neither is
        // a restore outcome, so the supervision ends silently.
        (void)g_restore.abandon();

        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "the station would not take a withdrawal; no restore outcome is reported "
                         "and nothing was erased");
        return;
    }

    // ⛔ ACCEPTED IS NOT DOWN. The supervisor holds this until the published phase actually reaches
    // Disabled — PM ruling, 2026-08-27.
    g_restore.arm_withdrawal(carry, already_reported);
}

// One terminal answer from the supervisor, reported exactly as the four publication rulings require.
void conclude_restore(const wifi_restore::Supervisor::Result result) {
    // ⛔ THE DECISION IS A TESTED FUNCTION, NOT THE ORDER OF STATEMENTS HERE — the shape
    // reentry::after_expiry() already establishes. It owns every publication ruling at once: what the
    // outcome is called, whether the browser may be told, that an expired surface is never pretended
    // into existence, that a quarantined station yields no restore vocabulary at all, and that one
    // failure is published exactly once — the accounting trail is recorded either way.
    const wifi_restore::Report report = wifi_restore::report_for(
        result,
        wifi_restore::Circumstances{network::setup_ap::running(), network::station::quarantined(),
                                    g_restore.already_reported()});

    diagnostics::timeline(diagnostics::tag::kApp, "station-restore-settled", "result",
                          static_cast<std::int64_t>(result), "published",
                          report.publication == wifi_restore::Publication::SetupFailed ? 1 : 0);

    if (report.failure == provisioning_flow::Failure::None) {
        // Rejoined, Withdrawn, or an abandonment that a newer instruction or the Device Fault owns.
        diagnostics::log(
            diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
            "station restoration settled result=" + std::string{wifi_restore::log_label(result)} +
                "; nothing was written and nothing was erased");
        return;
    }

    // The typed terminal outcome is recorded whether or not a browser can receive it.
    diagnostics::log(
        diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
        "station restoration settled result=" + std::string{wifi_restore::log_label(result)} +
            " reason=" + std::string{provisioning_flow::log_label(report.failure)} +
            (report.publication == wifi_restore::Publication::SetupFailed
                 ? "; reported on the live setup surface"
             : g_restore.already_reported()
                 ? "; already reported when the transaction settled, so this is the terminal "
                   "accounting only"
                 : "; the setup surface has expired, so nothing claims to have delivered "
                   "it") +
            "; the previously active pair is unchanged in flash");

    if (report.publication == wifi_restore::Publication::SetupFailed) {
        provisioning::publish(provisioning::SetupState::Failed,
                              provisioning_flow::log_label(report.failure));
    }

    // ⛔ AND THE NETWORK OBSERVATION IS LEFT EXACTLY AS THE STATION PUBLISHED IT. §7.4's phase and
    // §16.1's typed fault already describe the radio truthfully, and a restore outcome is a
    // statement about the SETUP FLOW rather than a second opinion about the link. Nothing here
    // dispatches, and nothing here can raise a device fault — PM ruling 3.
}

// Consumes the owed restoration: puts the radio back on the exact retained pair, or takes it down.
//
// ⛔ IT ARMS AND RETURNS. Every outcome is decided later, from terminal station evidence or from the
// published phase, by the supervisor. Nothing here calls a restoration complete.
void restore_station(const std::uint32_t now_ms) {
    g_station_restore_due = false;

    // The target is consumed exactly once. Whatever happens below, the driver stops holding an
    // unproven credential on this account: either it is being put back or it is coming down.
    const RollbackTarget rollback = g_rollback;
    g_rollback = RollbackTarget{};

    if (!rollback.held) {
        // Nothing disturbed the radio, so there is nothing to put back.
        return;
    }

    // ⛔ A QUARANTINED STATION IS THE DEVICE FAULT'S BUSINESS — PM ruling 3. The driver stopped
    // answering, the panel already says Wi-Fi is unavailable through the typed
    // DeviceFaultReason::WifiUnavailable path, and only a restart clears it.
    if (network::station::quarantined()) {
        (void)g_restore.abandon();

        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kNetwork,
                         "station restoration skipped: the station is quarantined and the device "
                         "fault already describes it");
        return;
    }

    if (rollback.target.action == wifi_restore::Action::Restore) {
        wifi_restore::Credential credential =
            wifi_restore::credential_for(storage::key_value(), rollback.target);

        network::station::Tag tag = network::station::kNoTag;

        if (credential.loaded) {
            network::Credentials credentials;
            credentials.ssid = credential.ssid.view();
            credentials.password = credential.password.view();

            tag = network::station::submit(credentials, now_ms);
        }

        // ⛔ WIPED ON EVERY PATH OUT OF THE BRANCH — success, refusal and the case where it never
        // loaded. §13.5: the passphrase has been copied into the driver's configuration by now, and
        // a refusal is no reason to leave this copy of it in RAM.
        wifi_restore::wipe(credential);

        if (tag != network::station::kNoTag) {
            g_restore.arm_restore(tag);

            diagnostics::timeline(diagnostics::tag::kApp, "station-restore", "action",
                                  static_cast<std::int64_t>(rollback.target.action), "tag", tag);

            diagnostics::log(
                diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                "rejoining the previously active pair profile=" +
                    std::to_string(rollback.target.profile) + " config-generation=" +
                    std::to_string(rollback.target.configuration_generation) +
                    " secret-generation=" + std::to_string(rollback.target.secret_generation) +
                    "; awaiting its terminal evidence, nothing is settled yet");
            return;
        }

        if (network::station::quarantined()) {
            // The submission quarantined the station inside the call. Ruling 3 again.
            (void)g_restore.abandon();

            diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                             "the station stopped answering while the previous pair was being "
                             "rejoined; the device fault path describes it");
            return;
        }

        // ⛔ THE EXACT RECORD WOULD NOT LOAD, OR THE DRIVER WOULD NOT TAKE IT — PM ruling, and this
        // is deliberately NOT a connectivity verdict. Nothing was ever attempted against the
        // network, so there is nothing true to say about it; what IS true is that the device could
        // not verify its saved network, and the unexplained candidate must still come down.
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "the retained rollback pair could not be used profile=" +
                             std::to_string(rollback.target.profile) +
                             "; withdrawing the unexplained candidate instead");

        withdraw_station(now_ms, provisioning_flow::Failure::WifiRollbackUnavailable,
                         rollback.reported);
        return;
    }

    diagnostics::timeline(diagnostics::tag::kApp, "station-restore", "action",
                          static_cast<std::int64_t>(rollback.target.action), "tag", 0);

    // Withdraw — PM ruling 1's prescribed outcome, reported only once the station has settled.
    // Unavailable — the staging disagreement, reported distinctly from a connectivity failure.
    withdraw_station(now_ms,
                     rollback.target.action == wifi_restore::Action::Withdraw
                         ? provisioning_flow::Failure::None
                         : provisioning_flow::Failure::WifiRollbackUnavailable,
                     rollback.reported);
}

// Driven from the application task's loop, which is the only task that may dispatch.
void provisioning_tick(const std::uint32_t now_ms) {
    // ⛔ AUTHORITATIVE BEFORE ANYTHING IS CONSUMED OR ADVANCED, AND THIS IS THE FIRST OF THE FOUR
    // TICKS. The loop runs provisioning_tick() → local_interface_tick() → printer_supervise() →
    // printer_ingest() → printer_launch(), so a guard anywhere later is a guard that runs after this
    // function has already acted. Two paths made that concrete:
    //
    //   • The reducer sets factory_resetting during the event drain at the top of the SAME iteration,
    //     so on the confirming tick a queued Wi-Fi submission was consumed here — retaining a
    //     credential, restarting the station, dispatching ValidatingConfiguration and telling the
    //     browser "validating" — before local_interface_tick() reached the erase at all.
    //
    //   • The provisioning server runs on its own task and can enqueue a submission the instant after
    //     quiesce_for_exclusive_operation() drains the queue. On the next hold tick this function ran
    //     first and processed it; printer_supervise()'s re-assertion of the barrier came too late.
    //
    // ⬅️ AND IT CALLS THE BARRIER RATHER THAN JUST RETURNING. A late submission must never be
    // PROCESSED, and it exists in the queue only until this task's next chance to wipe it — which is
    // here. The barrier is idempotent, so re-asserting it on every tick of the interval costs a
    // handful of comparisons and one queue poll.
    if (!exclusive::provisioning_work_permitted(exclusivity())) {
        quiesce_for_exclusive_operation();
        return;
    }

    // ⛔ THE POINTER IS NEVER HELD BY THIS FUNCTION. consume() takes it, hands over a reference, and
    // wipes and releases it on every path out of the callback — including the stage refusal below,
    // which is exactly the early return that a raw pointer here would invite somebody to leak. There
    // is no by-value Submission on this stack any more, so no secret-bearing local copy exists to
    // forget.
    {
        SubmissionOps ops;

        // ⛔ THE ITERATION'S READING IS CAPTURED BY VALUE, AND EVERY CLOCK-CONSUMING CALL BELOW
        // TAKES IT — R11. All three used to read the clock for themselves, milliseconds after this
        // function had been handed `now_ms`; see begin_transaction() for what that cost on the
        // board. Capturing it here is what makes the callback unable to fetch a second one.
        (void)submission_handoff::consume(ops, [now_ms](provisioning_flow::Submission& incoming) {
            if (incoming.stage == provisioning_flow::Stage::Printer) {
                // ⛔ AUTHORITATIVE STAGE GUARD. The server refuses this too, but this is the
                // check that decides: without a validated Wi-Fi stage there is no credential
                // to pair the printer with, and committing one would write a configuration
                // naming a network nobody proved.
                //
                // ⬅️ AND THE REFUSAL BRANCH NEEDS NO WIPE OF ITS OWN. consume() releases the
                // object after this callback returns, whichever way it returned.
                if (g_wifi_validated) {
                    (void)g_submission.printer_host.assign(incoming.printer_host.view());
                    (void)g_submission.printer_api_key.assign(incoming.printer_api_key.view());
                    g_submission.printer_port = incoming.printer_port;

                    // ⬅️ ACTIVITY — owner ruling 1 of this checkpoint. An accepted,
                    // user-initiated setup action, which is the only kind that refreshes the
                    // privileged window. The page's own /api/status polling reaches nothing
                    // like this on purpose.
                    g_privileged.note_activity(now_ms);

                    commit_transaction();
                } else {
                    diagnostics::log(diagnostics::LogLevel::Warning,
                                     diagnostics::tag::kProvisioning,
                                     "printer stage ignored: no validated wi-fi stage");
                }
            } else {
                // D-6: a new Wi-Fi submission is accepted from SetupFailed as readily as
                // from SetupActive, and supersedes any attempt still running — which is one
                // of ruling 2's wipe points, honoured by begin_transaction() overwriting the
                // retained credential after wiping it.
                wipe_submission(g_submission);
                g_wifi_validated = false;

                // Activity, on the same rule as the printer stage above: accepted and
                // user-initiated. A submission the request guard refused never reaches this
                // queue at all, so a refused caller cannot extend the window that refused it.
                g_privileged.note_activity(now_ms);

                begin_transaction(incoming, now_ms);
            }
        });
    }

    // ⛔ THE RESTORATION IS DRIVEN HERE, AFTER THE SUBMISSION MAILBOX AND BEFORE THE VERDICT —
    // checkpoint 3e. After, because a fresh submission supersedes it and begin_transaction() has
    // just cleared the flag if one arrived; before, because the verdict block below can SET the flag
    // and a restoration owed by this tick's own settlement is then driven on the next one rather
    // than in the middle of reading a verdict.
    //
    // ⬅️ AND NOTHING GUARDS IT AGAINST A RUNNING TRANSACTION, BECAUSE THE ORDER ALREADY DOES. The
    // flag is set only where a transaction has just ended or a session has just closed, so by the
    // time it is read here no transaction is running. The exclusive-operation barrier at the top of
    // this function is what keeps it from firing while a factory reset owns the device.
    if (g_station_restore_due) {
        restore_station(now_ms);
    }

    // ⛔ TAKEN AND CLEARED IN ONE ACT, ONCE, FOR WHICHEVER CONSUMER station_tick() ROUTED IT TO.
    // Both consumers apply §8.1's identity check themselves, so a record charged to a superseded
    // identity advances time and decides nothing wherever it lands.
    const StationEvidence evidence = g_station_evidence;
    g_station_evidence = StationEvidence{};

    // ⛔ THE SUPERVISOR IS STEPPED EVERY TICK, WITH OR WITHOUT EVIDENCE — checkpoint 3e's repair.
    // A withdrawal is judged by the published phase reaching Disabled, which arrives as a LEVEL and
    // may never arrive as an edge at all: a station that was already Disabled when the withdrawal
    // was accepted publishes no change. So the levels are read from the store, which this task owns,
    // and no new query is added to `network`.
    if (g_restore.armed()) {
        const app_model::NetworkState& network = g_store.state().network;

        const wifi_restore::Supervisor::Result settled = g_restore.observe(
            network.phase, network.fault,
            evidence.present && evidence.route == wifi_restore::Route::RestoreSupervision,
            evidence.tag, evidence.outcome);

        if (settled != wifi_restore::Supervisor::Result::Pending) {
            conclude_restore(settled);
        }
    }

    if (g_transaction_running) {
        const provisioning_flow::Verdict verdict =
            (evidence.present && evidence.route == wifi_restore::Route::CandidateValidation)
                ? g_validation.observe(evidence.tag, evidence.outcome, now_ms)
                : g_validation.tick(now_ms);

        // The dwell opens on the first lease this transaction sees and closes if that lease is
        // lost, which is exactly the restart rule the pure machine applies internally. Read from it
        // rather than re-derived here: provisioning_flow stays free of logging, and two derivations
        // of one fact is how they come to disagree.
        if (g_validation.dwelling() != g_dwell_open) {
            g_dwell_open = g_validation.dwelling();
            diagnostics::timeline(diagnostics::tag::kApp,
                                  g_dwell_open ? "dwell-begin" : "dwell-interrupted");
        }

        if (verdict == provisioning_flow::Verdict::Validated) {
            diagnostics::timeline(diagnostics::tag::kApp, "dwell-end");
            complete_wifi_stage();
        } else if (verdict == provisioning_flow::Verdict::Failed) {
            diagnostics::timeline(diagnostics::tag::kApp, "validation-failed", "failure",
                                  static_cast<std::int64_t>(g_validation.failure()));
            settle_transaction(verdict, g_validation.failure());
        }
    }

    if (g_access_point_stop_due_us != 0 && esp_timer_get_time() >= g_access_point_stop_due_us) {
        g_access_point_stop_due_us = 0;

        // ⬅️ THE SERVER SURVIVES THIS NOW — OWNER RULING R6, IMPLEMENTED AT MILESTONE 3.9.
        // Until this checkpoint the line here was `provisioning::stop()`, and the local web
        // interface died with the setup network. R6: "The local web interface must not
        // disappear after provisioning… Treat the web UI as a persistent local device
        // management interface, with provisioning as one mode, not as a disposable setup
        // page." So the access point stops, exactly as R6 also asks, and the server does not.
        //
        // ⛔ NOTHING IS RESTARTED OR REBOUND. The listener was never bound to one interface,
        // and restarting it would mint a new session token — which the page reads, correctly,
        // as proof that the display crashed mid-flow. Persistence here is the ABSENCE of the
        // call that used to be here, and that is the whole mechanism.
        //
        // The access point goes down first and the reachable set is refreshed immediately
        // after, so the origin it used to answer on stops being accepted as soon as it stops
        // being ours (owner ruling B). setup_ap::address() reports empty once it is down,
        // which is what makes one refresh sufficient rather than a second bookkeeping path.
        stop_setup_ap();
    }
}

// The Wi-Fi station's one step per tick — checkpoint 3c.
//
// ⛔ THIS FUNCTION IS WHERE THE SIXTH TASK WENT. §10.1 lists five tasks and says "Do not create a
// task per module or endpoint"; the station had a sixth, and that task owned the retry deadline,
// the phase machine and every esp_wifi call. §8.2 puts effect execution on this task ("effects are
// executed by `app_core`, then completion or failure is returned as another event", with StartWifi
// on the effect list) and §10.2 puts lifecycle timers here too. The SDK's own documented station
// architecture is the same shape (wifi.rst:404-416).
//
// ⛔ AND NO ESP-IDF TYPE APPEARS HERE. The driver stays behind the `network` boundary: this task
// calls one function and reads two answers. There is no mutex because there is one caller.
void station_tick(const std::uint32_t now_ms) {
    const network::station::Update update = network::station::service(now_ms);

    // ⛔ ONE TESTED DECISION DECIDES WHO THIS RECORD IS FOR — and the hand-written condition it
    // replaces was the defect. That condition discarded every record whenever no CANDIDATE
    // transaction was running, which silently included every record an accepted restoration was
    // waiting for; it was three tokens long, it lived in this file, and no negative control could
    // reach it. The policy now lives in wifi_restore where mutating it fails a host test, and the
    // ANSWER travels with the evidence rather than being re-derived by its consumer.
    //
    // R3's original purpose is preserved by the Discard branch: an answer nobody can consume must
    // not sit in the slot until some later transaction picks it up.
    const wifi_restore::Route route = wifi_restore::route_for(
        wifi_restore::Routing{update.evidence, g_transaction_running, g_restore.armed()});

    if (route == wifi_restore::Route::Discard) {
        g_station_evidence = StationEvidence{};
    }

    if (update.published) {
        // ⛔ DISPATCHED, NOT POSTED. This task is the store's only dispatcher and the station's only
        // consumer, so the observation goes straight into the reducer rather than through §10.7's
        // general event queue — which is bounded and lossy by design, and which previously stood
        // between a driver event and the verdict that depended on it.
        //
        // The observation is coherent by construction: `network` builds it from one instant of the
        // machine's truth, so a phase and an address in the same observation always describe the
        // same moment. An address change publishes even when the phase name did not move.
        dispatch(
            app_model::Event::network_observed(platform::monotonic_time(), update.observation));
    }

    if (route != wifi_restore::Route::Discard) {
        // Candidate truth, on its own channel and never mixed with the observation above.
        // provisioning_tick() owns the transaction or the restoration it belongs to and is the only
        // reader; the route it carries is what says which.
        g_station_evidence.present = true;
        g_station_evidence.tag = update.tag;
        g_station_evidence.outcome = update.outcome;
        g_station_evidence.route = route;
    }
}

// --- R6's persistent local management interface (Milestone 3.9) ------------

// Starts the local server if it is not already running.
//
// TWO CALLERS, TWO SITUATIONS. On a boot that enters provisioning, start_setup_ap() has
// already started it on the access point's address. On a boot that is already provisioned
// there is no access point at all, and this is what puts the interface on the LAN — which is
// what makes R6's persistence survive a power cycle rather than lasting until one.
//
// ⛔ READ-ONLY THERE, BY CONSTRUCTION. With no access-point address the server has no surface
// that permits_mutation(), so every mutating request is refused by owner ruling A without any
// endpoint needing to know why. The read-only routes never reach the gate.
void ensure_local_server() {
    if (provisioning::running() || g_lan_server_attempted) {
        return;
    }

    // Once per boot. A bind failure on a fixed port is not the kind of fault that clears
    // itself, and a tick that retried it would report the same thing forever.
    g_lan_server_attempted = true;

    provisioning::Config server;
    server.address = network::setup_ap::address();
    server.sink = &note_submission;
    server.update_sink = &note_update_request;

    if (!provisioning::start(server)) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                         "local management interface unavailable on this boot; the device is "
                         "reachable on the network but serves no page");
    }
}

// Publishes the addresses the device currently answers on — owner ruling B.
//
// ⬅️ EACH SLOT IS PRESENT ONLY WHILE IT IS GENUINELY ACTIVE, which is the ruling's own
// wording and the reason this reads all three from their owners rather than remembering them.
// setup_ap::address() is empty once the access point is down. The station address comes from
// the application state, where the network adapter reports it ONLY while associated with an
// address in hand — so a disconnect withdraws the station origin with no extra bookkeeping.
// The hostname is offered only while mDNS is actually advertising it.
//
// Speaks only when something changed: this runs on every application tick, and a log line per
// tick would bury everything else.
void refresh_reachability() {
    const std::string_view access_point = network::setup_ap::address();
    const std::string_view station = g_store.state().network.ip_address.view();

    if (access_point == g_published_access_point.view() && station == g_published_station.view()) {
        return;
    }

    g_published_access_point.assign(access_point);
    g_published_station.assign(station);

    provisioning::Addresses addresses;
    addresses.access_point = access_point;
    addresses.station = station;
    addresses.hostname =
        network::discovery::running() ? network::discovery::local_name() : std::string_view{};

    provisioning::publish_addresses(addresses);

    // Non-secret throughout: §7.4 already documents the network name and address as
    // displayable state, and a hostname is broadcast on the segment by design. No credential
    // and no token appears here.
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
        "local interface reachable at" +
            (access_point.empty() ? std::string{} : " setup=" + std::string{access_point}) +
            (station.empty() ? std::string{} : " lan=" + std::string{station}) +
            (addresses.hostname.empty() ? std::string{}
                                        : " name=" + std::string{addresses.hostname}) +
            (station.empty() ? " (no lan address yet)" : ""));
}

// Driven from the application task's loop, beside provisioning_tick() and for the same
// reason: the station address it needs lives in the store, and this is the store's only
// dispatcher — so it is the only task that can read that address without racing the reducer.
// Ends the privileged setup session: the access point goes down, its origin stops being
// accepted, and every trace of the session is wiped.
//
// ⛔ THE SERVER IS NOT TOUCHED — owner ruling R6 and ADR-0007. What ends here is the privileged
// SURFACE, not the persistent local interface: the read-only LAN server carries on, and the
// active configuration is untouched. Nothing is erased on this path, whichever caller reached it.
//
// TWO CALLERS, ONE IMPLEMENTATION: owner ruling D-2's grace period after a successful setup, and
// owner ruling C's bounded idle expiry. They differ only in why the session is over.
void stop_setup_ap() {
    network::setup_ap::stop();

    // The access-point origin stops being accepted in the same breath as the network it named —
    // owner ruling B of Milestone 3.9. setup_ap::address() reports empty once it is down, which
    // is what makes one refresh sufficient.
    refresh_reachability();

    g_privileged.end();

    g_setup_session.clear();
    ui::clear_setup_session();

    // Ruling 2's last wipe point: the session is over, so nothing it held may outlive it.
    g_wifi_validated = false;
    wipe_submission(g_submission);
}

// --- Printer validation and polling (Milestone 4) ---------------------------

// How long a printer-validation attempt may take before it is called failed.
//
// Two requests at §11.4's 4 s total budget each, plus room for a slow DHCP-era ARP and the
// worker's own wait slice. A user watching a browser must be told something within a few
// seconds of the truth, and an unbounded attempt would leave the panel in
// ValidatingConfiguration for as long as a printer chose to be silent.
constexpr std::int64_t kPrinterValidationBudgetUs = 20LL * 1000 * 1000;

// The worker's sink. It does exactly one thing, and that is the repair.
//
// ⬅️ RUNS ON THE PRUSALINK WORKER TASK, NOT THIS ONE. So it posts and never dispatches: §10.2 keeps
// the store single-consumer, and dispatch() reports a foreign caller as the defect it is.
//
// ⛔ IT NO LONGER TRANSLATES, AND IT NO LONGER JUDGES. It used to turn a Poll report into three
// application events here, after checking the worker's generation against a shared global. That put
// the liveness decision on the producing task, one queue away from the consumer, so a stop() landing
// between the check and the three posts left a superseded worker's connection, identity and snapshot
// to be applied by the reducer — refreshing observed_at and presenting a dead reading as current.
// The whole generation-stamped Report now crosses the boundary intact and printer_ingest() decides.
void publish_printer_report(const prusalink::Report& report) {
    const bool channel_present = g_printer_reports != nullptr;
    const bool accepted = channel_present && xQueueSend(g_printer_reports, &report, 0) == pdTRUE;

    const printer_session::Publication publication =
        printer_session::publication_of(channel_present, accepted);

    if (!printer_session::lost(publication)) {
        return;
    }

    // §10.7 and §16.4: saturation and a missing channel are both counted and reported, never
    // silent, and they are reported as DIFFERENT facts. A dropped Poll report costs one cycle of
    // freshness; a dropped Validate verdict makes the attempt time out, which is why the queue has a
    // second slot and why this is a warning rather than a debug line.
    ++g_printer_reports_dropped;
    diagnostics::log(
        diagnostics::LogLevel::Warning, diagnostics::tag::kPrinter,
        "printer report lost: " + std::string{printer_session::log_label(publication)} +
            " purpose=" + std::string{prusalink::log_label(report.purpose)} +
            " total_lost=" + std::to_string(g_printer_reports_dropped));
}

// Empties the report channel. Called when an attempt is superseded, so a report published a moment
// before the supersession cannot be classified against the session that replaced it.
//
// ⬅️ BELT AND BRACES, DELIBERATELY. classify() would reject every one of these on generation, so
// draining is not what makes the design correct — it just keeps the queue from carrying reports
// nobody will ever accept, and keeps the diagnostic log free of a rejection per stale report.
void drain_printer_reports() {
    if (g_printer_reports == nullptr) {
        return;
    }

    prusalink::Report discarded;

    while (xQueueReceive(g_printer_reports, &discarded, 0) == pdTRUE) {
    }

    wipe_bytes(&discarded, sizeof(discarded));
}

// Logs a start attempt, starts it, and does not let the credential outlive the call.
//
// ⛔ THE API KEY IS NEVER RETAINED. It is read from the secret store by the caller, handed to the
// worker, and the local copy dies here. §13.5 keeps it out of AppState and out of logs, and the line
// below prints booleans and a method name only — there is nothing in scope that could leak.
[[nodiscard]] prusalink::StartReport launch_printer_worker(prusalink::Target& target) {
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
        "starting worker purpose=" + std::string{prusalink::log_label(target.purpose)} +
            " profile=" + std::to_string(target.transaction.profile) +
            " attempt=" + std::to_string(target.transaction.attempt) +
            " host=" + std::string{target.host.empty() ? "absent" : "present"} +
            " port=" + std::to_string(static_cast<unsigned>(target.port)) +
            " auth=" + std::string{app_model::log_label(target.auth_method)} +
            " key=" + (target.api_key.empty() ? "absent" : "present"));

    const prusalink::StartReport started = prusalink::start(target, publish_printer_report);

    wipe_bytes(&target, sizeof(target));

    return started;
}

// §14.3 steps 5-8 against the CANDIDATE, with the transaction the caller already built.
//
// ⛔ THE IDENTITY IS A PARAMETER, NOT A COMPUTATION. It arrives from printer_session::
// transaction_for() and is used verbatim, so the value the worker is handed and the value the
// session records cannot differ. Recomputing it from active storage — which is what this function
// used to do on one side and begin_printer_validation() on the other — is the defect that made
// every genuine verdict look like another attempt's.
[[nodiscard]] prusalink::StartReport
start_validation_worker(const prusalink::Transaction& transaction) {
    prusalink::Target target;
    target.purpose = prusalink::Purpose::Validate;
    target.transaction = transaction;

    // ⛔ VALIDATION PROBES THE CANDIDATE, NOT THE ACTIVE RECORD. This is the direct consequence of
    // staging without activating: the record under test is no longer the one config::load()
    // returns. Reading storage here would probe the printer the device is ALREADY using and report
    // success for a candidate nobody contacted. The submission is the candidate, in RAM, and it is
    // what was staged.
    target.host = g_submission.printer_host;
    target.port = g_submission.printer_port;
    target.auth_method = g_submission.printer_api_key.empty() ? app_model::AuthMethod::None
                                                              : app_model::AuthMethod::ApiKey;
    target.api_key = g_submission.printer_api_key;

    return launch_printer_worker(target);
}

// A start attempt and the identity it used, returned together.
//
// ⛔ TOGETHER, BECAUSE THE ALTERNATIVE IS THE DEFECT THIS PASS EXISTS TO FIX. The caller must arm a
// session with the EXACT transaction the worker was handed. Reconstructing it — even from the same
// storage a moment later — is how the two sides came to disagree in the first place.
struct PrinterLaunch {
    prusalink::StartReport started;
    prusalink::Transaction transaction;
};

// Ongoing operational polling, against the ACTIVE pair — which by construction is a configuration
// whose printer has already answered.
[[nodiscard]] PrinterLaunch start_polling_worker() {
    const config::LoadReport configuration = config::load(storage::key_value());
    const secrets::LoadReport secret = secrets::load(storage::key_value());

    prusalink::Target target;
    target.purpose = prusalink::Purpose::Poll;
    target.host = configuration.value.printer_host;
    target.port = configuration.value.printer_port;
    target.auth_method = configuration.value.auth_method;
    target.api_key = secret.value.printer_api_key;

    // ⬅️ ATTEMPT IS 0, AND THAT IS NOT AN OMISSION. `attempt` exists to separate two validations of
    // one profile; a polling worker is identified by its GENERATION, and the profile records which
    // configuration it is polling so a report cannot be matched to a session for a different pair.
    target.transaction.profile = configuration.value.wifi_profile_id;
    target.transaction.attempt = 0;

    PrinterLaunch launch;
    launch.transaction = target.transaction;
    launch.started = launch_printer_worker(target);
    return launch;
}

// §14.3 steps 5-8, and §13.3 step 5. Begins the connection test for a just-staged candidate.
void begin_printer_validation() {
    // A report from a superseded attempt must not be classified against this one. classify() would
    // reject it on generation anyway; draining keeps the channel and the log clean.
    drain_printer_reports();

    // A new attempt. Monotonic, so a verdict still in flight for the previous one cannot match.
    ++g_validation_attempts;

    dispatch(app_model::Event::provisioning_phase(
        platform::monotonic_time(), app_model::ProvisioningPhase::ValidatingConfiguration));

    provisioning::publish(provisioning::SetupState::Validating, {});

    // ⛔ THE ONE CONSTRUCTION SITE. Built from the staged pair, used for the worker AND for the
    // session record below. Nothing else computes a validation identity.
    const prusalink::Transaction transaction =
        printer_session::transaction_for(g_printer_validation.staged, g_validation_attempts);

    if (!transaction.valid()) {
        // Nothing was staged, so there is no candidate to test and no identity to arm.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                         "printer validation not started: no staged candidate to test");
        settle_transaction(provisioning_flow::Verdict::Failed,
                           provisioning_flow::Failure::PromotionFailed);
        return;
    }

    const prusalink::StartReport started = start_validation_worker(transaction);

    if (!started.started) {
        // ⬅️ NOTHING IS ARMED, SO THERE IS NOTHING TO ROLL BACK. The session is armed only from a
        // StartReport that says a task exists, so a failed start cannot leave an identity a stray
        // report could match. The candidate stays staged and inactive and the access point stays up
        // for another try — D-6's recoverable failure.
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                         "printer validation could not start; candidate stays staged and inactive");
        settle_transaction(provisioning_flow::Verdict::Failed,
                           provisioning_flow::Failure::PrinterUnreachable);
        return;
    }

    // ⛔ ARMED FROM THE LEASE start() JUST CREATED, AND FROM THE SAME TRANSACTION THE WORKER HOLDS.
    // A report published before this line still matches: it carries exactly this generation, and
    // consumption happens in printer_ingest() on THIS task, which cannot run until this function
    // returns. So there is no window in which a genuine report is discarded, and none in which a
    // superseded one is accepted.
    g_printer_validation.session =
        printer_session::armed(prusalink::Purpose::Validate, started.generation, transaction);
    g_printer_validation.deadline_us = esp_timer_get_time() + kPrinterValidationBudgetUs;

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                     "printer validation running for profile " +
                         std::to_string(transaction.profile) + " attempt " +
                         std::to_string(transaction.attempt) + " generation " +
                         std::to_string(started.generation) + "; the candidate is NOT active yet");
}

// §13.3 step 6. Crosses the activation boundary, and only then finishes the setup.
void complete_printer_validation() {
    // ⛔ THE LAST CHECK BEFORE ANYTHING IS WRITTEN. Report ingress already refuses everything while a
    // reset owns the device, and the barrier disarmed the session that would have carried a verdict
    // here — this is the third and innermost reason a candidate cannot be activated after an erase,
    // sited where the writes actually happen.
    if (!exclusive::candidate_activation_permitted(exclusivity())) {
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kConfig,
                         "candidate activation refused: an exclusive operation owns the product "
                         "state");
        return;
    }

    const provisioning_flow::StagedPair staged = g_printer_validation.staged;

    const provisioning_flow::ActivatedPair activated =
        provisioning_flow::activate_pair(storage::key_value(), staged);

    // "activated=yes" means a reread proved both pointers name the new pair — not merely that two
    // writes were accepted. See activate_pair().
    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kConfig,
        std::string{"pair activation activated="} + (activated.activated ? "yes" : "no") +
            " profile=" + std::to_string(staged.profile) +
            " failure=" + std::string{provisioning_flow::log_label(activated.failure)} +
            " recovery-record=" + (activated.marker_retained ? "retained" : "cleared"));

    if (!activated.activated) {
        settle_transaction(provisioning_flow::Verdict::Failed, activated.failure);
        return;
    }

    // Re-observed so ConfigurationState carries the newly ACTIVE pair. This is the transition that
    // makes has_printer_configuration true, and therefore the only thing that permits polling.
    ConfigurationLoad reloaded = load_configuration();
    wipe_bytes(&reloaded.secret, sizeof(reloaded.secret));

    settle_transaction(provisioning_flow::Verdict::Validated, provisioning_flow::Failure::None);
}

// Turns one accepted Poll report into observations. Runs on the application task, so it dispatches
// directly: there is no second queue between the liveness check and the reducer, which is what
// removes the staleness window entirely rather than narrowing it.
void apply_poll_report(const prusalink::Report& report) {
    const app_model::Timestamp moment = platform::monotonic_time();

    // Connection first, and separately. §7.5's closing rule keeps connection state independent of
    // printer operational state, so a cycle that reached the printer but could not decode it still
    // reports the connection fact — and reports no snapshot, which is what stops a failure from
    // leaving the previous reading looking current.
    if (report.connection_present) {
        dispatch(app_model::Event::connection_observed(moment, report.connection));
    }

    if (report.identity_present) {
        dispatch(app_model::Event::printer_identity(moment, report.identity));
    }

    // ⛔ ONE RESPONSE, ONE EVENT. The printer half and the optional job half of the status body
    // were read together and are applied together: one store mutation, one projection, one
    // acceptance decision. §4.3's optional `job` is carried inside the observation as
    // `job.reported`, so "this printer reports no job" is still stated — it is simply not a
    // second event.
    //
    // ⬅️ IT USED TO BE TWO DISPATCHES, and that was not a tidiness question. The panel showed a
    // composition no response described, and worse, the terminal-completion snapshot reads the
    // store's current job — so a direct PRINTING 79% → FINISHED 100% response recorded the
    // completion at 79 %, because the job half of that same response had not been applied yet.
    // app_model::apply_status() now orders the two halves and settles the lifecycle last.
    if (report.status_present) {
        dispatch(app_model::Event::printer_status(moment, report.status));
    }
}

// Settles a validation attempt on its own verdict.
void apply_validate_report(const prusalink::Report& report) {
    g_printer_validation.session = printer_session::Session{};
    g_printer_validation.deadline_us = 0;

    if (report.verdict == prusalink::Verdict::Succeeded) {
        complete_printer_validation();
        return;
    }

    settle_transaction(provisioning_flow::Verdict::Failed,
                       provisioning_flow::failure_for_printer(report.failure_phase));
}

// The exclusivity conditions, assembled in ONE place from the store and this file's own deadline.
//
// ⬅️ ONE PLACE, BECAUSE THREE CALL SITES ASK THE SAME QUESTION. Report ingress, worker launch and the
// activation boundary all need it, and a second copy of the expression is how printer_supervise() and
// printer_launch() came to spell out the polling eligibility separately in the first place.
[[nodiscard]] exclusive::Conditions exclusivity() {
    const app_model::AppState& state = g_store.state();

    exclusive::Conditions conditions;
    conditions.factory_resetting = state.factory_resetting;
    conditions.factory_reset_confirmation_armed =
        state.modal_surface == app_model::ModalSurface::FactoryResetConfirmation;
    conditions.reset_restart_scheduled = g_reset_restart_due_us != 0;

    // ⛔ AUDIT ITEM 5. The boot's resume-failure path sets neither term above — the reducer's
    // factory_resetting comes only from a confirmed press, and no restart is scheduled on a failure
    // — so without this the one path where the device most needed holding released everything.
    conditions.device_fault = state.device_fault;

    // ⬅️ AND THE LATCH TOO, WHICH IS THE TERM NO LATER OBSERVATION CAN OVERWRITE. A barrier resting
    // on a single mutable presentation field is what audit item 5's fifth finding was.
    conditions.factory_reset_failed = state.factory_reset_failed;

    // ⛔ THE DIAGNOSTIC STATUS SOURCE, FROM THE COMPILE-TIME GATE ITSELF. Not from a runtime flag
    // that could be cleared, and not from whether the instrument happens to have started: if the
    // flow is compiled in, it owns the printer data for the whole life of the image. In every
    // product profile this is a constant false and the barrier folds away.
    conditions.synthetic_status_source = validation::printing_timeline_compiled();

    conditions.provisioning = state.provisioning;
    conditions.network = state.network.phase;
    conditions.has_printer_configuration = state.configuration.has_printer_configuration;
    conditions.firmware_update_active =
        app_model::ota_holds_device(state.ota.phase) || g_ota_admission.held();
    conditions.first_boot_pending = g_first_boot.pending();
    conditions.configuration_in_flight =
        g_transaction_running || g_printer_validation.session.armed;
    return conditions;
}

// ⛔ THE EXCLUSIVE-OPERATION BARRIER. Called synchronously on the application task BEFORE the first
// destructive factory-reset step, and idempotent so every later tick of the reset re-asserts it.
//
// After this returns, nothing a printer worker or a provisioning transaction has in flight can reach
// storage, the store or the browser: both sessions are disarmed, the whole validation object —
// identity, staged pair and deadline — is forgotten, the worker's lease is superseded so it cannot
// publish, the report channel is empty, and the transaction's retained credential is wiped.
//
// ⛔ AND THE STATION STANDS DOWN — R4, AND IT DID NOT UNTIL THIS REPAIR. An earlier account of this
// barrier read as though it already covered every subsystem. It did not cover the station at all:
// station_tick() ran unconditionally from the loop, so a backoff expiry could still issue connect()
// and a pending retirement could still issue disconnect() while the device erased itself, and the
// adapter went on holding a converted station configuration — passphrase included — from a
// submission made into the very window this barrier exists to close. Both are closed below.
//
// ⛔ IT DOES NOT PUBLISH A SETUP FAILURE. settle_transaction() would tell the browser the setup failed
// and move the panel to SetupFailed, which would be a false account of why the transaction ended: the
// user asked for a factory reset, and the reset's own presentation is what belongs on the panel.
// So the transaction is ENDED here, silently, and §9's reset progress owns the surface.
//
// ⛔ AND THE WORKER IS STOPPED WITHOUT WAITING. prusalink::stop() only supersedes the generation; it
// joins nothing, touches no socket and never reaches the HTTP handle, which lives entirely inside the
// worker task's own call to network::get(). The outgoing worker abandons its request at its next
// slice and exits on its own.
void quiesce_for_exclusive_operation() {
    g_printer_validation = printer_session::Validation{};
    g_poll = printer_session::Polling{};

    prusalink::stop();
    drain_printer_reports();

    // The provisioning transaction's RAM, ended rather than settled. Mirrors settle_transaction()'s
    // teardown minus everything that speaks to the browser or the store.
    g_transaction_running = false;
    g_validation.reset();
    g_wifi_validated = false;
    wipe_submission(g_submission);
    g_access_point_stop_due_us = 0;
    g_dwell_open = false;

    // A submission the server accepted but this task has not taken yet must not be committed after
    // the erase. Taken, wiped and freed rather than left: the queue holds a pointer to a heap object
    // that still carries a credential, and abandoning the pointer would leak both.
    //
    // ⛔ THE SERVER TASK CAN ENQUEUE AGAIN THE INSTANT THIS RETURNS, AND THAT IS FINE. What must never
    // happen is that such a submission is PROCESSED, and provisioning_tick()'s guard is what
    // guarantees it: the first thing that runs on the next tick re-enters this function and drains
    // it. The queue is allowed to hold one; the application task is not allowed to act on it.
    {
        SubmissionOps ops;
        const unsigned released = submission_handoff::drain(ops);

        // §10.7: never silent. A drain that found something means a user submitted during the window
        // an exclusive operation opened, and their submission was discarded rather than applied —
        // which is a thing the timeline must be able to explain.
        if (released != 0) {
            diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                             "discarded " + std::to_string(released) +
                                 " pending setup submission(s): an exclusive operation owns the "
                                 "product state");
        }
    }

    // ⛔ AND THE STATION STANDS DOWN TOO — R4. Until this repair the barrier ended the provisioning
    // transaction's RAM and drained the submission mailbox, but the station machine went on
    // advancing: a backoff expiry issued connect(), a pending retirement issued disconnect(), and a
    // device erasing itself went on joining a network. Worse, the adapter was still holding a
    // converted station configuration WITH THE PASSPHRASE from a submission made into the window
    // this barrier exists to close — the same class of RAM credential the mailbox drain above is
    // written for. quiesce() wipes that buffer, drops the retained intent, and refuses every
    // subsequent driver operation until a new submission.
    //
    // ⬅️ IT DOES NOT DISCONNECT AN ESTABLISHED ASSOCIATION. A successful reset restarts the device
    // moments later, and resumption after a FAILED erase is the open factory-reset item at the 3e
    // boundary — inventing a restoration transaction here is exactly the 3e work this checkpoint
    // must not pull in.
    if (!exclusive::station_work_permitted(exclusivity())) {
        network::station::quiesce(monotonic_ms());

        // ⛔ AND EVERY RESTORE-OWNED FACT GOES WITH IT — checkpoint 3e. A reset owns the product state:
        // the retained rollback target, the owed restoration and any supervision in flight all end here,
        // and none of them publishes a setup failure on the way out. station::quiesce() has already
        // forgotten the withdrawal inside the machine, so leaving the supervisor armed would leave it
        // waiting for a Disabled that this barrier, not the station, was responsible for.
        g_rollback = RollbackTarget{};
        g_station_restore_due = false;
        (void)g_restore.abandon();
    }

    // A long press observed before the reset took the device must not raise a setup session behind
    // it. local_interface_tick() returns early while factory_resetting, so the request was never
    // acted on — but it would have been the moment a FAILED reset released the device.
    g_reconfiguration_requested = false;

    // ⛔ THE PRIVILEGED SURFACE CLOSES, AND THE R6 SERVER DOES NOT. stop_setup_ap() ends the access
    // point, withdraws its origin, ends the privileged window and clears both the generated and the
    // displayed session — and deliberately never touches provisioning::stop(), so ADR-0007's
    // persistent read-only LAN management interface carries on. Gated on there being something open,
    // because this runs on every tick of the reset interval and refresh_reachability() is the only
    // part that would otherwise be asked the same question repeatedly.
    //
    // g_setup_session is checked as well as the access point: start_setup_ap() generates the session
    // BEFORE starting the radio and returns early if the radio does not come up, so a failed start
    // can leave a session with no access point behind it.
    if (network::setup_ap::running() || g_privileged.active() || !g_setup_session.ssid.empty()) {
        stop_setup_ap();
    }
}

// STEP 1 OF THE CYCLE — supersession, before anything is consumed.
//
// ⛔ THIS RUNS FIRST FOR A REASON. Every reason a worker must stop is a state change this task has
// just applied: provisioning opened, a factory reset armed, the station dropped, the configuration
// changed. Superseding here and consuming afterwards is what guarantees a report produced under the
// old conditions is judged against a session that already knows they ended.
void printer_supervise() {
    const exclusive::Conditions conditions = exclusivity();

    // ⛔ EXCLUSIVITY IS CHECKED BEFORE THE ARMED-VALIDATION EARLY RETURN, AND THAT ORDER IS THE FIX.
    // This function used to return here whenever a Validate session was armed — before it ever looked
    // at factory_resetting — so a reset could erase the store with a validation worker still live and
    // its verdict still queued. Re-asserting the barrier is idempotent and costs nothing on the ticks
    // where it is already held.
    if (exclusive::active(conditions)) {
        quiesce_for_exclusive_operation();
        return;
    }

    // A validation attempt owns the worker while it runs, and it is a provisioning-phase activity.
    // Leaving it alone here is what keeps the two purposes from fighting over one task.
    if (g_printer_validation.session.armed) {
        return;
    }

    // ⛔ ELIGIBILITY IS NOT THE ONLY REASON TO STOP — audit item 4. A worker polling the PREVIOUS
    // printer is still eligible by every term above: there is a configuration, the network is up,
    // provisioning is closed and no reset is running. It is simply pointed at the wrong machine.
    //
    // The reducer has already forgotten what that worker observed; if the worker keeps running, its
    // next report matches the armed session, classifies as Accept in printer_ingest() below, and
    // refills the state the boundary cleared. So the identity is checked here, BEFORE consumption —
    // which is the same ordering rule the tick already follows and the reason supervise() runs first.
    const bool target_superseded =
        printer_session::target_superseded(g_poll, g_store.state().configuration);

    if (exclusive::printer_worker_permitted(conditions) && !target_superseded) {
        return;
    }

    if (g_poll.session.armed) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kPrinter,
                         target_superseded
                             ? "polling superseded: the configured printer identity changed"
                             : "polling superseded: the device is no longer eligible to poll");
    }

    // Disarmed before the drain, so nothing can be accepted between the two.
    g_poll = printer_session::Polling{};
    prusalink::stop();
    drain_printer_reports();
}

// STEP 2 OF THE CYCLE — consume, check, and only then translate.
void printer_ingest() {
    const bool exclusive_operation = !exclusive::printer_reports_permitted(exclusivity());

    if (g_printer_reports != nullptr) {
        prusalink::Report report;

        while (xQueueReceive(g_printer_reports, &report, 0) == pdTRUE) {
            const printer_session::Session& session = report.purpose == prusalink::Purpose::Validate
                                                          ? g_printer_validation.session
                                                          : g_poll.session;

            const printer_session::Ingress verdict =
                printer_session::classify(session, report, exclusive_operation);

            if (verdict != printer_session::Ingress::Accept) {
                // ⛔ NONE of the report becomes an observation, and observed_at is not refreshed.
                // A rejected report is a fact about a worker, not about the printer.
                diagnostics::log(
                    diagnostics::LogLevel::Warning, diagnostics::tag::kPrinter,
                    "printer report rejected: " + std::string{printer_session::log_label(verdict)} +
                        " purpose=" + std::string{prusalink::log_label(report.purpose)} +
                        " report generation=" + std::to_string(report.generation) +
                        " profile=" + std::to_string(report.transaction.profile) +
                        " attempt=" + std::to_string(report.transaction.attempt) +
                        "; session generation=" + std::to_string(session.generation) +
                        " profile=" + std::to_string(session.transaction.profile) +
                        " attempt=" + std::to_string(session.transaction.attempt));
                continue;
            }

            if (report.purpose == prusalink::Purpose::Validate) {
                apply_validate_report(report);
            } else {
                apply_poll_report(report);
            }
        }

        wipe_bytes(&report, sizeof(report));
    }

    // ⛔ NO TIMEOUT SETTLEMENT WHILE A RESET OWNS THE DEVICE. The barrier has already cleared the
    // deadline, so this cannot fire — and if it could, publishing "setup failed" over the reset's own
    // presentation would be the misleading account the barrier exists to avoid.
    if (exclusive_operation) {
        return;
    }

    if (!printer_session::expired(g_printer_validation, esp_timer_get_time())) {
        return;
    }

    // The attempt ran out of time. Supersede the worker so a late answer cannot settle a
    // transaction that has already been called failed, and leave the candidate staged.
    diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kProvisioning,
                     "printer validation timed out; candidate stays staged and inactive");

    settle_transaction(provisioning_flow::Verdict::Failed,
                       provisioning_flow::Failure::PrinterUnreachable);
}

// STEP 3 OF THE CYCLE — start a polling worker when there is nothing running and every reason to.
//
// ⛔ ARMED ON AN ACTIVE, VALIDATED CONFIGURATION ONLY. has_printer_configuration is true only when
// the stored record carries printer_validated, which nothing sets but the activation above. §14.3
// step 10 puts the transition to the normal UI after validation succeeds, and §13.3 step 6 puts
// "mark configuration active" after the connection test — so a pending candidate is never the source
// of operational printer data or presentation. printer_configuration_pending is deliberately NOT
// part of this condition.
//
// ⛔ NEITHER CALL BLOCKS. prusalink::stop() only supersedes the worker's lease, and start() refuses
// while a previous worker is still winding down — so a retry lands on a later tick rather than
// stalling the task that services button gestures.
void printer_launch() {
    if (g_printer_validation.session.armed || g_poll.session.armed) {
        return;
    }

    // ⛔ ONE EXPRESSION, AND IT INCLUDES THE EXCLUSIVITY TERM. printer_supervise() and this function
    // used to spell the eligibility out separately, which is how the factory-reset term came to be
    // present in both and load-bearing in neither: supervise() returned before reaching it. Now both
    // ask exclusive::printer_worker_permitted(), so no worker can launch while a confirmed reset is
    // erasing or holding.
    if (!exclusive::printer_worker_permitted(exclusivity()) || prusalink::running()) {
        return;
    }

    const PrinterLaunch launch = start_polling_worker();

    if (!launch.started.started) {
        // A refusal — an unsupported auth method, an absent key, a worker still winding down — is
        // logged by the callee and simply retried, which costs two NVS reads on a tick that had
        // nothing else to do. Nothing is armed, so nothing can be accepted.
        return;
    }

    // Armed from the lease start() created and the transaction the worker actually holds.
    g_poll.session = printer_session::armed(prusalink::Purpose::Poll, launch.started.generation,
                                            launch.transaction);

    // ⬅️ AND THE IDENTITY IT WAS LAUNCHED AGAINST, FROM THE SAME STATE start_polling_worker() READ
    // THE RECORD FOR. printer_supervise() compares this against the configuration on every later
    // tick, which is what makes a reconfigured printer stop the worker rather than silently keep it.
    g_poll.target = printer_session::target_of(g_store.state().configuration);
}

// --- Deliberate reconfiguration and factory reset (Milestone 3.10) ----------

// Notes the physical acts that mean something to this checkpoint.
//
// ⬅️ WHY app_core AND NOT THE REDUCER. Owner ruling 1 makes a long press the physical-presence
// authorization that raises the privileged setup surface — which is a radio and a server, i.e.
// side effects. §5.2 forbids the reducer from performing any, so the reducer gives the gesture no
// state meaning and this observes the same event after it has been applied.
//
// Called immediately after each dispatch, so the store's answer to "is a destructive confirmation
// armed" is already current when it is read below.
void note_gesture(const app_model::Event& event) {
    if (event.type != app_model::EventType::ButtonGestureObserved) {
        return;
    }

    if (event.button.gesture != app_model::ButtonGesture::LongPress) {
        return;
    }

    // ⛔ NOT WHILE A DESTRUCTIVE CONFIRMATION IS ARMED — owner ruling A: "LongPress does not
    // confirm reset and retains its M3.10 reconfiguration meaning OUTSIDE the confirmation
    // modal". Inside it, the gesture does nothing at all rather than quietly doing something
    // else while the user is looking at a reset prompt.
    if (g_store.state().modal_surface == app_model::ModalSurface::FactoryResetConfirmation) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "long press ignored: a factory-reset confirmation is on screen");
        return;
    }

    request_reconfiguration();
}

// R1's deliberate re-entry, and §14.1's second entry condition — "the user deliberately requests
// setup".
//
// ⛔ IT ONLY REQUESTS. Raising a radio from here would run it on whichever call site observed the
// gesture; the tick is where the access point is actually brought up, in one place, after the
// guards below.
void request_reconfiguration() {
    const app_model::AppState& state = g_store.state();

    // An exclusive operation or a device fault outranks a reconfiguration request (UIAD 001
    // §12.2). A firmware install must not be interrupted, and a reset in progress least of all.
    if (state.device_operation == app_model::DeviceOperation::FirmwareUpdating ||
        state.device_operation == app_model::DeviceOperation::FactoryResetting ||
        state.device_operation == app_model::DeviceOperation::DeviceFault) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "reconfiguration request ignored: the device is busy with an exclusive "
                         "operation");
        return;
    }

    // Already provisioning. A second request must not restart a session in progress — and it
    // must certainly not mint a second setup password while a browser is using the first.
    if (network::setup_ap::running() || g_transaction_running) {
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "reconfiguration request ignored: setup is already active");
        return;
    }

    g_reconfiguration_requested = true;
}

// What the reset's log line says after the outcome, per outcome and exhaustively.
//
// ⛔ IT USED TO BE A TERNARY THAT CLAIMED RESUMPTION ON EVERY FAILURE — audit item 5. On a journal
// write failure nothing was journalled, so "it will be resumed on the next boot" was false. Naming
// each outcome separately is what keeps the timeline honest about which of the two happened.
[[nodiscard]] std::string_view reset_outcome_note(const factory_reset::Outcome outcome) {
    switch (outcome) {
    case factory_reset::Outcome::Complete:
        return "; the device will start as unprovisioned";

    case factory_reset::Outcome::JournalWriteFailed:
        // ⛔ BOUNDED TO THIS ATTEMPT. Reached only from a newly confirmed runtime reset, where the
        // journal write is step 1 — so this attempt established no journal and removed nothing, and
        // no resumable job is owed by it. It says nothing about what an earlier reset may have left.
        return "; its journal could not be written, so this attempt removed nothing and owes no "
               "resumption";

    case factory_reset::Outcome::Failed:
        return "; it will be resumed on the next boot";
    }

    return "";
}

// Drives a confirmed factory reset through erase, hold and restart.
//
// ⛔ ONE CALLER, AND IT REQUIRES PRIOR USER CONSENT. The tick calls this when
// AppState::factory_resetting becomes true, which the reducer sets only on a second deliberate
// very-long press inside the confirmation window. No failure path reaches it.
//
// The boot's resumption of an interrupted reset does NOT come through here: it calls
// factory_reset::run() directly, before any configuration is read, and needs no restart because it
// is already in a fresh boot. That is why the deadline below has exactly one writer.
//
// ⬅️ WHICH TICK THIS IS, IS reset_progress::decide()'s ANSWER AND NOT THIS FUNCTION'S. See that
// header for what Milestone 3.11 T16 found when the sequencing lived here.
void run_factory_reset() {
    switch (reset_progress::decide(g_store.state().factory_resetting,
                                   g_store.state().factory_reset_failed, g_reset_restart_due_us,
                                   esp_timer_get_time())) {
    case reset_progress::Step::Nothing:
    case reset_progress::Step::Wait:
        // §9's progress presentation is on the panel and the erase is already done. Nothing to do
        // and — the T16 log-spam half — nothing to say.
        return;

    case reset_progress::Step::Halted:
        // ⛔ THE RESET ALREADY FAILED AND THE DEVICE IS FAULTED — audit item 5. factory_resetting is
        // still true, which is what keeps the exclusive barrier held, and g_reset_restart_due_us is
        // still 0 — so without this answer this function would re-enter the erase on every tick at
        // 10 Hz. Silent for the same reason Wait is: the panel is already saying it.
        return;

    case reset_progress::Step::Restart:
        diagnostics::log(diagnostics::LogLevel::Warning, diagnostics::tag::kConfig,
                         "factory reset complete; restarting into first-boot behaviour");
        platform::restart();
        return;

    case reset_progress::Step::Erase:
        break;
    }

    // ⛔ THE BARRIER, BEFORE THE FIRST DESTRUCTIVE STEP AND ON THIS TASK. factory_reset::run() erases;
    // it cannot also police what else is running, and its own header states this as a precondition.
    // Everything that could still write storage — both printer sessions, the worker's lease, the
    // queued reports, the transaction's retained credential, an unconsumed submission — is forgotten
    // here, synchronously, so nothing can act between this line and the erase below.
    quiesce_for_exclusive_operation();

    diagnostics::log(
        diagnostics::LogLevel::Warning, diagnostics::tag::kConfig,
        "factory reset confirmed; printer and provisioning activity quiesced; removing "
        "this installation's configuration and credentials");

    const factory_reset::Outcome outcome = factory_reset::run(storage::key_value());

    // Presence and outcome only. Not a key name, not a record, and nothing that was in one.
    diagnostics::log(outcome == factory_reset::Outcome::Complete ? diagnostics::LogLevel::Info
                                                                 : diagnostics::LogLevel::Error,
                     diagnostics::tag::kConfig,
                     "factory reset " + std::string{factory_reset::log_label(outcome)} +
                         std::string{reset_outcome_note(outcome)});

    if (outcome != factory_reset::Outcome::Complete) {
        // ⛔ NO RESTART ON FAILURE, DELIBERATELY. If the store is failing persistently, rebooting
        // into the resume would be a reboot loop. The recovery is an explicit restart by the user.
        //
        // ⛔ AND NO COMPLETION IS DISPATCHED — AUDIT ITEM 5, 2026-08-28. This used to end the
        // operation with FactoryResetCompleted, which cleared factory_resetting and released the
        // device: exclusivity gone, the panel back to the ordinary screen with nothing said, and a
        // journal still armed. A device that then provisioned would have that new configuration
        // erased by the next boot's resume. The event has been deleted; it had no other emitter.
        //
        // ⬅️ factory_resetting DELIBERATELY STAYS TRUE, because it is still true: a reset owns this
        // device and did not finish. It is what holds exclusive::Owner::FactoryReset. The panel does
        // not stay on "Erasing settings" regardless — resolve_device_operation() checks device_fault
        // FIRST, so the fault raised here wins the presentation on this very tick, dispatch() being
        // synchronous.
        //
        // ⛔ AND THE EXECUTOR IS STOPPED BY AppState::factory_reset_failed, WHICH THE REDUCER LATCHES
        // WHEN THIS FAULT IS RAISED — not by the fault reason itself. The reason is a single mutable
        // slot five reducer paths assign, and a NetworkObserved already queued when this failed
        // reached the next drain, replaced it, and the very next tick answered Step::Erase. The
        // latch is cleared only by an actual boot, so no later event can re-enter the erase.
        dispatch(app_model::Event::device_fault(
            platform::monotonic_time(), app_model::DeviceFaultReason::FactoryResetIncomplete));
        return;
    }

    // ⬅️ THE OPERATION DELIBERATELY DOES NOT END HERE. factory_resetting stays true so §9's progress
    // presentation is on the panel while the hold runs; the state ends by restarting, which is the
    // honest way for it to end — everything it described is gone.
    //
    // The next tick reaches Step::Wait on this deadline, and the tick after it elapses reaches
    // Step::Restart. Both above; neither anywhere else.
    g_reset_restart_due_us = esp_timer_get_time() + kFactoryResetHoldUs;
}

void local_interface_tick() {
    // ONE observation for the whole tick, passed to everything on this path that needs a time.
    const std::uint32_t now_ms = monotonic_ms();

    // ⬅️ THE ONE PLACE A CONFIRMED RESET IS EXECUTED. factory_resetting is set by the reducer and
    // by nothing else; see apply_factory_reset_confirmed().
    if (g_store.state().factory_resetting) {
        run_factory_reset();
        return;
    }

    // An armed confirmation whose window has passed. The reducer already refuses to complete a
    // stale one — that check happens at the moment of the second press and is authoritative —
    // so this exists to take the prompt off the panel rather than to enforce anything.
    if (g_store.state().modal_surface == app_model::ModalSurface::FactoryResetConfirmation) {
        const auto armed = g_store.state().factory_reset_armed_at;
        const auto elapsed = platform::monotonic_time() - armed;

        if (armed != app_model::Timestamp{} &&
            elapsed > app_model::kFactoryResetConfirmationWindow) {
            dispatch(app_model::Event::factory_reset_cancelled(platform::monotonic_time()));
            diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                             "factory-reset confirmation expired; nothing was erased");
        }
    }

    // ⛔ THE RESTART IS NOT CHECKED HERE, AND THAT IS THE MILESTONE 3.11 T16 FIX. It used to be,
    // and it was unreachable: the `factory_resetting` branch above returns on every tick of a reset
    // in progress, which is precisely when a scheduled restart exists. It now belongs to
    // run_factory_reset(), reached through reset_progress::decide(), which is the only place that
    // reads the deadline. A second copy here is what made the first one dead code.

    // R1's deliberate re-entry. Raised here so there is one path to an access point whether the
    // request came from a long press or from a boot with no configuration at all.
    if (g_reconfiguration_requested) {
        g_reconfiguration_requested = false;

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "deliberate reconfiguration requested; raising a fresh setup session");

        // ⬅️ AND RECOVERY IS OVER — MILESTONE 3.11 T18. The long press is ADR-0008 decision 3's
        // physical-presence act, so from here the device is in setup, not waiting in recovery.
        // Until this dispatch existed the only thing that cleared the condition was a reboot
        // (reducer's BootPhase::Starting), so RecoveryMode outlived its own meaning: the scene's
        // recovery branch returns early, and it went on drawing §8's Status Message with all three
        // session rows cleared while a live session sat behind it — a panel telling the reader to
        // hold the button to start setup, with the password they needed withheld.
        //
        // ⛔ GUARDED, BECAUSE THE ORDINARY RE-ENTRY IS THE COMMON CASE. RecoveryModeExited always
        // reports a change, so dispatching it unconditionally would bump the store revision and
        // re-render on every long press that had nothing to leave.
        //
        // The event already existed and was already reduced and tested; nothing dispatched it.
        if (g_store.state().recovery_mode) {
            dispatch(app_model::Event::recovery_mode_exited(platform::monotonic_time()));
        }

        // ⛔ THE TICK'S OWN now_ms, WHICH IS THE WHOLE POINT. The expiry check a few lines below
        // subtracts the session's activity moment from this same value, so a session begun here
        // reads as zero milliseconds idle rather than as an underflowed 49 days.
        start_setup_ap(now_ms);
    }

    // The privileged window's bounds — owner rulings 1 and C of this checkpoint.
    g_privileged.set_transaction_running(g_transaction_running, now_ms);

    if (g_privileged.expired(now_ms)) {
        // ⛔ THE PHASE IS THIS CALLER'S TO CHOOSE, AND IT DEPENDS ON WHETHER THERE IS A
        // CONFIGURATION TO FALL BACK TO — Milestone 3.11 T4 finding B. stop_setup_ap() clears the
        // transient session, so the moment it runs the panel has no SSID, no password, no address
        // and no QR to draw. Leaving the phase at SetupActive left the panel saying "Join this
        // network" over an empty composition, for ever, about a network that no longer existed —
        // and a panel asserting something untrue is worse than one saying nothing.
        //
        // ⬅️ WHY NOT INSIDE stop_setup_ap(). Its two callers carry three different product
        // meanings: the D-2 grace after a successful commit, which already dispatched Inactive
        // before scheduling the grace and needs nothing here; and this expiry, which resolves two
        // ways depending on whether a configuration exists. A single unconditional phase in the
        // shared helper would be wrong for at least one of the three.
        const bool provisioned =
            g_store.state().configuration.status == app_model::ConfigurationStatus::Valid;

        // What that resolves to is reentry::after_expiry()'s, not this file's, so every branch is
        // host-tested rather than reasoned about here. It answers both halves at once on purpose:
        // the phase alone cannot distinguish "configured, nothing to say" from "back to recovery",
        // because both are Inactive.
        const reentry::AfterExpiry next =
            reentry::after_expiry(provisioned, g_last_provisioning_decision);

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kProvisioning,
                         "privileged setup session idle for " +
                             std::to_string(g_privileged.idle_for(now_ms) / 1000) +
                             "s; stopping the setup network. the local interface stays up and "
                             "nothing was erased. phase=" +
                             std::string{app_model::log_label(next.phase)} +
                             (next.return_to_recovery ? "; returning to recovery" : ""));

        stop_setup_ap();
        dispatch(app_model::Event::provisioning_phase(platform::monotonic_time(), next.phase));

        // ⬅️ MILESTONE 3.11 T18, OWNER RULING. A session opened out of recovery that ends without
        // replacing the faulted record leaves the fault exactly where it was, so recovery is the
        // truthful resting state and not a UI special case: the same condition that put the device
        // there on boot still holds.
        //
        // ⛔ AFTER THE PHASE, NOT BEFORE IT. Both apply inside this tick, but the render task reads
        // the store on its own schedule, so the order decides what a frame landing between them
        // could show. This way it is Inactive with no recovery — momentarily neutral. The other way
        // round it would be recovery beside SetupActive, which is precisely the composition this
        // fix exists to remove, and a frame asserting something untrue is the worse of the two.
        if (next.return_to_recovery) {
            dispatch(app_model::Event::recovery_mode_entered(platform::monotonic_time()));
        }

        // ⛔ AND THE RADIO GOES BACK — PM ruling 2 of checkpoint 3e. This is the "user remains idle
        // between stages" case in full: Step 1 validated, nothing persisted, the printer step never
        // submitted, and the ordinary idle rule closes the window. ADR-0008 decision 5 is unchanged —
        // reaching the printer step granted no window of its own and only an accepted submission
        // ever refreshed activity — so what changes here is not WHEN the session ends but what the
        // station is left holding when it does.
        //
        // ⬅️ SCHEDULED RATHER THAN DONE, AND `now_ms` IS THE REASON. This function takes its own
        // clock observation; provisioning_tick() takes the loop's. Restoring from here would issue
        // the station submission against this reading and then have the next tick compare deadlines
        // against the other one, which is the shape R11 was the price of. The flag is consumed one
        // tick later, on the observation that owns the station machine.
        if (g_rollback.held) {
            g_station_restore_due = true;
        }
    }

    // A station address in hand is the whole condition. It appears once the device has joined
    // and leased an address, whether that happened during provisioning or on a boot that was
    // already provisioned, and it disappears on a disconnect.
    if (!g_store.state().network.ip_address.empty()) {
        ensure_local_server();

        if (!network::discovery::running()) {
            // R6 prefers "stable local discovery/addressing … rather than requiring the user
            // to discover the DHCP-assigned IP manually". A failure here is degraded, not
            // fatal: the device stays reachable by address, so the return value is logged
            // inside discovery::start() and the interface carries on without it.
            (void)network::discovery::start(provisioning::kServerPort);
        }
    }

    refresh_reachability();
}

// §15.1 steps 13 and 18.
//
// The credential travels no further than this call: network::start() copies it into the
// driver's configuration and wipes its own copy, and the password is cleared out of
// `loaded` here so the boot sequence's remaining lifetime holds nothing sensitive.
void start_network(ConfigurationLoad& loaded) {
    // ⬅️ THE BRANCH IS EXPLICIT NOW. It used to be hidden inside network::start()'s empty-SSID
    // guard, which meant "no credential" and "the driver refused" came back as the same answer.
    // Withdrawing is not a failure: it is the un-provisioned device §14.1 has already put into
    // provisioning, and it publishes Disabled, which is the correct observable state.
    if (!loaded.wifi_credentials_usable) {
        network::station::station_absent(monotonic_ms());
        loaded.secret.value.wifi_password.clear();
        return;
    }

    network::Credentials credentials;
    credentials.ssid = loaded.secret.value.wifi_ssid.view();
    credentials.password = loaded.secret.value.wifi_password.view();

    const network::station::Tag tag = network::station::submit(credentials, monotonic_ms());

    // ⛔ THE CREDENTIAL IS CLEARED WHETHER OR NOT THE SUBMISSION WAS ACCEPTED. It has been copied
    // into the driver's configuration by now, and a refusal is no reason to leave the boot's copy
    // of a passphrase in RAM.
    //
    // ⬅️ AND A REFUSAL DOES NOT MEAN NOTHING CHANGED. An earlier note here said a refused submission
    // "has changed nothing", which was never true of every path: a refusal can come from bring-up
    // that already created the shared stack and registered handlers, from a synchronous driver
    // refusal that has since quarantined the station, or from the internal-memory guard. What is
    // true is narrower — no INTENT was accepted, so no transaction was started and nothing is
    // waiting for a verdict.
    loaded.secret.value.wifi_password.clear();

    if (tag == network::station::kNoTag) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kNetwork,
                         "wifi station did not start; the device will not reach the network");
    }
}

} // namespace

void run() {
    // main creates the application task and then does nothing that touches the store.
    //
    // §4.1 wants this function small: initialise services, create the composition
    // root, start the application runtime. That is now literally all it does, and the
    // reason is measured rather than stylistic — a single store dispatch puts two 2 kB
    // AppState copies and an Event on the caller's stack, and the ESP-IDF main task
    // has 3584 bytes in total.
    if (!start_application_task()) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "no application task; the store will not advance");
    }

    // The main task's own stack, once startup is done. Reported because Phase B
    // overflowed it, and a measured number is the only thing that stops that
    // happening again. The size comes from the Kconfig value rather than a local
    // constant, so the figure cannot drift from the configuration.
    diagnostics::log_task_stack("main", CONFIG_ESP_MAIN_TASK_STACK_SIZE);

    // Keep the main task alive without a busy loop. Blocking on the FreeRTOS
    // scheduler lets the idle tasks run and feed the task watchdog; control never
    // spins and the watchdog is never disabled.
    while (true) {
        vTaskDelay(kHeartbeatInterval);
        diagnostics::log(diagnostics::LogLevel::Debug, diagnostics::tag::kApp,
                         "heartbeat rev=" + std::to_string(g_store.revision()));
    }
}

} // namespace prd::app_core
