// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

// sdkconfig.h first and explicitly — see the note in render_report_flow.cpp. A #if on
// an undefined CONFIG_* macro is silently false and turns this whole file into a stub.
#include "sdkconfig.h"

#include "panel_flows.hpp"

// Milestone 1 Phase C and D display bring-up, moved out of app_core unchanged.
//
// The behaviour here was signed off on the physical unit — panel initialisation,
// SPI clock sweep, the colour A/B comparison, the brightness curve and the PWM
// frequency sweep — so this file is a relocation, not a rewrite. Nothing about what
// it does to the display or the backlight has been altered.
//
// Mutually exclusive with the renderer, which is why nothing else needs to check.
// These flows write pixels straight to the panel, bypassing LVGL entirely, so app_core
// does not start ui when they are compiled in (display_patterns_compiled()). Until
// Milestone 2 Phase C that exclusion was against the Phase F bench scene; it is now
// against the product renderer, which always runs otherwise.

// TWO CONTRADICTIONS, MADE INTO BUILD FAILURES.
//
// Both were live in this repository at some point and neither produced any signal.
//
// The first: these flows and the LVGL diagnostic scene both initialise the panel, and
// only one may own it. Milestone 1 resolved that by run-time precedence, which meant
// the losing flow was compiled into the image and never ran.
//
// The second: the scenario flow exists to drive the product scene. With these patterns
// compiled in, app_core does not start the renderer, so there is no scene for the
// scenarios to drive — and a hardware-validation image built that way validates nothing
// it was built to validate. That combination shipped in the profile for one build during
// Milestone 2 Phase C and was caught only by comparing the measured image size against
// the committed figure. This is what makes it impossible rather than merely documented.
#if defined(CONFIG_PRD_DISPLAY_TEST_PATTERNS) && defined(CONFIG_PRD_UI_DIAGNOSTIC_SCENE)
#error "PRD_DISPLAY_TEST_PATTERNS and PRD_UI_DIAGNOSTIC_SCENE both own the panel; select one"
#endif

#if defined(CONFIG_PRD_DISPLAY_TEST_PATTERNS) && defined(CONFIG_PRD_VALIDATION_SCENARIOS)
#error                                                                                             \
    "PRD_DISPLAY_TEST_PATTERNS stops the product renderer starting, so PRD_VALIDATION_SCENARIOS would have no scene to drive; select one"
#endif

#if defined(CONFIG_PRD_DISPLAY_TEST_PATTERNS)

#include <cstdint>
#include <string>

// ESP-IDF headers are not -Wundef clean. Suppressed for this TU in CMakeLists.txt
// per architecture spec §12.3, as for every other ESP-IDF adapter boundary.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "display/display.hpp"
#include "display/panel_tuning.hpp"
#include "display/test_patterns.hpp"

namespace prd::validation::internal {
namespace {

// Advances patterns on a timer so the panel can be inspected without any input
// component existing. Phase C predates the button.
constexpr TickType_t kPatternInterval = pdMS_TO_TICKS(6000);

// Runs on its own task rather than on the main task. The ESP-IDF main task stack
// is 3584 bytes, which esp_lcd initialisation plus this component's
// std::ostringstream-based logging overflows — observed during Phase C as a
// cache/MMU fault inside esp_intr_alloc, because the FreeRTOS canary only checks
// at context-switch time and cannot catch an overflow that faults immediately. A
// dedicated stack also matches the task model in Architecture Spec §10.
constexpr char kValidationTaskName[] = "prd_display_val";
constexpr std::uint32_t kValidationTaskStackBytes = 8192;
constexpr UBaseType_t kValidationTaskPriority = 4;

// Reports the display component's pixel-transfer bookkeeping. Every drop-out
// counter here must stay at zero: a spurious completion would mean the
// one-callback-per-blit assumption the ownership contract rests on is wrong, and a
// wait timeout would mean a buffer was still owned by DMA when the caller gave up
// on it.
void log_transfer_statistics() {
    const auto stats = display::transfer_statistics();
    const bool healthy = stats.spurious_completions == 0 && stats.wait_timeouts == 0 &&
                         stats.submitted == stats.completed;

    diagnostics::log(healthy ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
                     diagnostics::tag::kApp,
                     "transfers submitted=" + std::to_string(stats.submitted) +
                         " completed=" + std::to_string(stats.completed) +
                         " in_flight=" + std::to_string(display::transfers_in_flight()) +
                         " peak_in_flight=" + std::to_string(stats.peak_in_flight) +
                         " spurious=" + std::to_string(stats.spurious_completions) +
                         " wait_timeouts=" + std::to_string(stats.wait_timeouts));
}

#if defined(CONFIG_PRD_DISPLAY_COLOUR_AB)

// Phase C1: draw the colour comparison chart once and hold it, so the panel can be
// photographed under stable conditions. The chart's marker strip encodes the tuning
// mode so a photograph is never ambiguous about which build produced it.
int tuning_marker_count() {
#if defined(CONFIG_PRD_DISPLAY_TUNING_VENDOR_EXACT)
    return 2;
#elif defined(CONFIG_PRD_DISPLAY_TUNING_VENDOR_GAMMA_ONLY)
    return 3;
#elif defined(CONFIG_PRD_DISPLAY_TUNING_VENDOR_ANALOGUE_GAMMA)
    return 4;
#else
    return 1;
#endif
}

void run_colour_ab() {
    display::Config config; // tuning mode comes from Kconfig

    if (!display::init(config)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "display init failed");
        return;
    }

    const int markers = tuning_marker_count();
    const bool drawn = display::patterns::draw_comparison_chart(markers);
    display::set_backlight_enabled(true);

    diagnostics::log(drawn ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
                     diagnostics::tag::kApp,
                     std::string{"colour A/B chart "} + (drawn ? "drawn" : "FAILED") +
                         " markers=" + std::to_string(markers) +
                         " tuning=" + display::tuning_mode_name(config.tuning));

    diagnostics::log_task_stack(kValidationTaskName, kValidationTaskStackBytes);
    log_transfer_statistics();

    // Hold indefinitely; the image must stay still while it is photographed.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

#elif defined(CONFIG_PRD_DISPLAY_CLOCK_SWEEP)

// SPI clock sweep, lowest first (Phase C requirements 10-11). Each step
// re-initialises the panel from scratch, so the sweep doubles as the repeated
// initialisation test: N sweeps exercise N x 4 full init/deinit cycles.
constexpr int kSweepClocksHz[] = {
    10 * 1000 * 1000,
    20 * 1000 * 1000,
    40 * 1000 * 1000,
    80 * 1000 * 1000,
};

// Patterns most likely to expose SPI signal-integrity faults: a 1 px checkerboard
// stresses every data line on every pixel, colour bars expose bit errors as wrong
// hues, corner markers expose addressing errors.
constexpr display::patterns::Pattern kSweepPatterns[] = {
    display::patterns::Pattern::Checkerboard,
    display::patterns::Pattern::ColourBars,
    display::patterns::Pattern::CornerMarkers,
};

constexpr TickType_t kSweepPatternHold = pdMS_TO_TICKS(4000);

void run_clock_sweep() {
    unsigned init_attempts = 0;
    unsigned init_failures = 0;
    unsigned sweep = 0;

    while (true) {
        ++sweep;

        for (const int clock_hz : kSweepClocksHz) {
            display::Config config;
            config.spi_clock_hz = clock_hz;

            ++init_attempts;
            if (!display::init(config)) {
                ++init_failures;
                diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                                 "sweep=" + std::to_string(sweep) +
                                     " clock_hz=" + std::to_string(clock_hz) + " INIT FAILED");
                display::deinit();
                continue;
            }

            display::set_backlight_enabled(true);

            diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                             "sweep=" + std::to_string(sweep) +
                                 " clock_hz=" + std::to_string(clock_hz) +
                                 " init_ok attempts=" + std::to_string(init_attempts) +
                                 " failures=" + std::to_string(init_failures));

            for (const auto pattern : kSweepPatterns) {
                if (!display::patterns::draw(pattern)) {
                    diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                                     "clock_hz=" + std::to_string(clock_hz) + " pattern=" +
                                         std::string{display::patterns::name(pattern)} +
                                         " DRAW FAILED");
                }
                vTaskDelay(kSweepPatternHold);
            }

            // Reported before teardown, while this sweep step's deepest path and
            // its transfer counters are still meaningful.
            diagnostics::log_task_stack(kValidationTaskName, kValidationTaskStackBytes);
            log_transfer_statistics();

            // Blank before tearing the bus down so the transition between steps is
            // unambiguous to the observer.
            display::set_backlight_enabled(false);
            display::deinit();
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
}

#elif defined(CONFIG_PRD_DISPLAY_BRIGHTNESS_TEST)

// Phase D brightness validation. Holds one static pattern and walks the backlight
// through the required sequence, announcing each step on the console so an
// observation can always be tied to a known state.
//
// White is used rather than a product colour: maximum light output makes flicker,
// stepping and the low-brightness floor as visible as they can be.
struct BrightnessStep {
    const char* label;
    std::uint8_t percent;
};

// The bottom of the range is walked one percent at a time: the lowest useful
// brightness is a threshold, and a threshold cannot be found with a step size
// larger than the answer.
constexpr BrightnessStep kBrightnessSteps[] = {
    {"off", 0},  {"1%", 1},   {"2%", 2},   {"3%", 3},   {"4%", 4},     {"5%", 5},
    {"10%", 10}, {"25%", 25}, {"50%", 50}, {"75%", 75}, {"100%", 100},
};

constexpr TickType_t kBrightnessStepHold = pdMS_TO_TICKS(4000);
constexpr int kFadeDurationMs = 3000;

void log_backlight_state(const char* const label) {
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     std::string{"brightness step="} + label +
                         " percent=" + std::to_string(display::brightness_percent()) +
                         " duty=" + std::to_string(display::backlight_duty()) + "/" +
                         std::to_string(display::backlight_max_duty()) +
                         " enabled=" + (display::backlight_enabled() ? "yes" : "no"));
}

// Redraws the field with `index` marker squares so a dark-room observer can name
// the step they are looking at. Without this, "the lowest one I could see" is not a
// measurement.
void draw_step_marker(const int index) {
    if (!display::patterns::draw_marked_white(index)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "marked-white draw FAILED");
    }
}

void run_brightness_test() {
    display::Config config;

    if (!display::init(config)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "display init failed");
        return;
    }

    display::set_backlight_enabled(true);

    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "brightness validation active freq_hz=" +
                         std::to_string(display::backlight_frequency_hz()) + " resolution_bits=" +
                         std::to_string(display::backlight_duty_resolution_bits()) +
                         " max_duty=" + std::to_string(display::backlight_max_duty()));

    unsigned pass = 0;

    while (true) {
        ++pass;
        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                         "brightness pass=" + std::to_string(pass) + " begin");

        int index = 0;
        for (const auto& step : kBrightnessSteps) {
            ++index;
            draw_step_marker(index);
            display::set_brightness_percent(step.percent);
            log_backlight_state(step.label);
            vTaskDelay(kBrightnessStepHold);
        }

        diagnostics::log_task_stack(kValidationTaskName, kValidationTaskStackBytes);
        log_transfer_statistics();

        // Fades run from the extremes so the whole range is exercised, including
        // the bottom of the curve where stepping would be most visible.
        display::set_brightness_percent(0);
        log_backlight_state("fade-up begin");
        display::fade_brightness_to(100, kFadeDurationMs);
        log_backlight_state("fade-up end");
        vTaskDelay(kBrightnessStepHold);

        log_backlight_state("fade-down begin");
        display::fade_brightness_to(0, kFadeDurationMs);
        log_backlight_state("fade-down end");
        vTaskDelay(kBrightnessStepHold);
    }
}

#elif defined(CONFIG_PRD_DISPLAY_BACKLIGHT_FREQ_SWEEP)

// Phase D PWM frequency selection. Conservative first, then outward in both
// directions from the starting point, so the observer sees the failure mode at the
// bottom of the range as well as the candidates.
//
// 200 Hz is included deliberately as a negative control: it is expected to flicker
// and band, which makes the rest of the results interpretable rather than merely
// asserted.
constexpr int kSweepFrequenciesHz[] = {200, 1000, 2000, 5000, 10000, 20000, 40000};

// Flicker and camera banding are invisible at 100 % duty, where the output never
// switches, and most visible at short pulse widths. Each frequency is therefore
// held at a moderate level and then a low one.
constexpr std::uint8_t kSweepDutyPercents[] = {25, 5};

constexpr TickType_t kSweepHold = pdMS_TO_TICKS(6000);

void run_backlight_frequency_sweep() {
    display::Config config;
    config.backlight_frequency_hz = kSweepFrequenciesHz[0];

    if (!display::init(config)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "display init failed");
        return;
    }

    display::set_backlight_enabled(true);

    unsigned sweep = 0;

    while (true) {
        ++sweep;

        int index = 0;
        for (const int frequency_hz : kSweepFrequenciesHz) {
            // Redraw the marker strip so an observer watching an unattended sweep
            // can always tell which frequency is on screen. Redrawing the frame
            // between steps also exercises requirement 6 — the panel must not
            // corrupt while the PWM timer is being re-timed underneath it.
            ++index;
            if (!display::patterns::draw_marked_white(index)) {
                diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                                 "marked-white draw FAILED");
            }

            if (!display::set_backlight_frequency_hz(frequency_hz)) {
                diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                                 "sweep=" + std::to_string(sweep) +
                                     " freq_hz=" + std::to_string(frequency_hz) + " RETIME FAILED");
                continue;
            }

            for (const std::uint8_t percent : kSweepDutyPercents) {
                display::set_brightness_percent(percent);

                diagnostics::log(
                    diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                    "sweep=" + std::to_string(sweep) + " markers=" + std::to_string(index) +
                        " freq_hz=" + std::to_string(display::backlight_frequency_hz()) +
                        " resolution_bits=" +
                        std::to_string(display::backlight_duty_resolution_bits()) +
                        " percent=" + std::to_string(percent) +
                        " duty=" + std::to_string(display::backlight_duty()) + "/" +
                        std::to_string(display::backlight_max_duty()));

                diagnostics::log_task_stack(kValidationTaskName, kValidationTaskStackBytes);
                log_transfer_statistics();

                vTaskDelay(kSweepHold);
            }
        }
    }
}

#else // normal validation run: cycle every pattern at the conservative clock

void run_pattern_cycle() {
    display::Config config; // conservative SPI clock; see display.hpp

    if (!display::init(config)) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "display init failed");
        return;
    }

    const auto size = display::geometry();
    diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kApp,
                     "display validation active " + std::to_string(size.width_px) + "x" +
                         std::to_string(size.height_px) + " cycling patterns every 6 s");

    auto pattern = display::patterns::Pattern::CornerMarkers;
    bool backlight_on = false;

    while (true) {
        const bool drawn = display::patterns::draw(pattern);

        // Light the panel only once a complete frame has been written, so the first
        // thing the eye sees is a finished pattern.
        if (!backlight_on) {
            display::set_backlight_enabled(true);
            backlight_on = true;
        }

        diagnostics::log(drawn ? diagnostics::LogLevel::Info : diagnostics::LogLevel::Error,
                         diagnostics::tag::kApp,
                         "pattern=" + std::string{display::patterns::name(pattern)} +
                             (drawn ? " drawn" : " FAILED"));

        // Reported after a full frame has been rendered and transferred, which is
        // this task's deepest path.
        diagnostics::log_task_stack(kValidationTaskName, kValidationTaskStackBytes);
        log_transfer_statistics();

        vTaskDelay(kPatternInterval);
        pattern = display::patterns::next(pattern);
    }
}

#endif // display validation flow selection

// Exactly one of these is compiled: the flows are a Kconfig choice, so the branches
// cannot overlap.
void run_display_validation() {
#if defined(CONFIG_PRD_DISPLAY_COLOUR_AB)
    run_colour_ab();
#elif defined(CONFIG_PRD_DISPLAY_CLOCK_SWEEP)
    run_clock_sweep();
#elif defined(CONFIG_PRD_DISPLAY_BRIGHTNESS_TEST)
    run_brightness_test();
#elif defined(CONFIG_PRD_DISPLAY_BACKLIGHT_FREQ_SWEEP)
    run_backlight_frequency_sweep();
#else
    run_pattern_cycle();
#endif
}

void display_validation_task(void* /*argument*/) {
    run_display_validation();
    vTaskDelete(nullptr);
}

} // namespace

void start_display_patterns() {
    const BaseType_t created =
        xTaskCreate(display_validation_task, kValidationTaskName, kValidationTaskStackBytes,
                    nullptr, kValidationTaskPriority, nullptr);

    if (created != pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, diagnostics::tag::kApp,
                         "failed to create display validation task");
    }
}

bool display_patterns_compiled() {
    return true;
}

} // namespace prd::validation::internal

#else // no pattern flow compiled

namespace prd::validation::internal {

void start_display_patterns() {}

bool display_patterns_compiled() {
    return false;
}

} // namespace prd::validation::internal

#endif
