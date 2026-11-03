// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "backlight_internal.hpp"

#include <string>
#include <string_view>

// This TU is the ESP-IDF LEDC-adapter boundary. Like display.cpp, the ESP-IDF C
// headers it includes are not clean under the project's strict flags (undefined
// macros -> -Wundef; #include_next -> -Wpedantic, which no in-file pragma can
// silence). Both warnings are relaxed for this single file in CMakeLists.txt per
// architecture spec §12.3; all other project warnings still apply here.
#include "driver/ledc.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "brightness_curve.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "display/display.hpp"

namespace prd::display {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kDisplay;

// ---------------------------------------------------------------------------
// LEDC resource allocation (deliberate, per Phase D requirement 4).
//
// None of this is a board fact. The only backlight information that comes from
// board is the GPIO number and its polarity; the ESP32-S3 routes any LEDC
// channel to any output-capable GPIO through the peripheral matrix, so nothing
// about the wiring constrains which timer or channel is used. The same
// reasoning applied to the SPI host choice in Phase C.
// ---------------------------------------------------------------------------

// First timer and first channel of four timers / eight channels. The backlight
// is the only LEDC consumer in MVP scope, so it takes the first pair and leaves
// the remainder contiguous and free.
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;

// Forced, not chosen: the ESP32-S3 implements only the low-speed channel group
// (no SOC_LEDC_SUPPORT_HS_MODE for this target).
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;

// Clock source: XTAL, not APB.
//
// Both reach every frequency of interest, and APB (80 MHz) would allow one more
// bit of duty resolution than XTAL (40 MHz). XTAL is chosen anyway. ESP-IDF's
// LEDC source-clock table lists XTAL_CLK as Dynamic Frequency Scaling
// compatible and APB_CLK as not, and the LEDC driver takes no power-management
// lock of its own. With APB as the source, enabling DFS later would silently
// shift the backlight PWM frequency along with the CPU frequency. This board
// carries a battery charger, so power management is a plausible future change
// and one extra bit is not worth a latent defect. Phase B confirmed the 40 MHz
// crystal.
constexpr ledc_clk_cfg_t kClockSource = LEDC_USE_XTAL_CLK;

// Fade step interval: ~62 updates per second. Fast enough that a fade reads as
// continuous motion, slow enough that it costs almost nothing.
constexpr int kFadeStepIntervalMs = 16;

bool g_ready = false;
bool g_enabled = false;
std::uint8_t g_percent = 0;
int g_pin = -1;
int g_frequency_hz = 0;
int g_resolution_bits = 0;
std::uint32_t g_max_duty = 0;
std::uint32_t g_duty_table[kBrightnessTableSize] = {};

void log_error(const std::string& message, const esp_err_t error) {
    diagnostics::log(diagnostics::LogLevel::Error, kTag,
                     message + " (" + esp_err_to_name(error) + ")");
}

std::uint32_t source_clock_hz() {
    std::uint32_t hz = 0;
    const esp_err_t err = esp_clk_tree_src_get_freq_hz(static_cast<soc_module_clk_t>(kClockSource),
                                                       ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
    if (err != ESP_OK) {
        log_error("esp_clk_tree_src_get_freq_hz failed", err);
        return 0;
    }
    return hz;
}

// Configures (or reconfigures) the timer and rebuilds the duty table for the
// resolution that frequency permits. Does not touch the channel or the duty.
bool configure_timer(const int frequency_hz) {
    if (frequency_hz <= 0) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "backlight frequency must be positive, got " +
                             std::to_string(frequency_hz));
        return false;
    }

    const std::uint32_t src_hz = source_clock_hz();
    if (src_hz == 0) {
        return false;
    }

    // Always take the highest duty resolution the frequency allows: resolution
    // is what limits how smooth a fade looks at the bottom of the range, where
    // the eye is most sensitive to stepping.
    const std::uint32_t bits =
        ledc_find_suitable_duty_resolution(src_hz, static_cast<std::uint32_t>(frequency_hz));
    if (bits == 0) {
        diagnostics::log(
            diagnostics::LogLevel::Error, kTag,
            "no duty resolution achievable for frequency_hz=" + std::to_string(frequency_hz) +
                " src_clk_hz=" + std::to_string(src_hz));
        return false;
    }

    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.duty_resolution = static_cast<ledc_timer_bit_t>(bits);
    timer.timer_num = kTimer;
    timer.freq_hz = static_cast<std::uint32_t>(frequency_hz);
    timer.clk_cfg = kClockSource;
    timer.deconfigure = false;

    const esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        log_error("ledc_timer_config failed", err);
        return false;
    }

    g_frequency_hz = frequency_hz;
    g_resolution_bits = static_cast<int>(bits);

    // Duty range is [0, 2**resolution] (ESP-IDF ledc.h). Full scale is
    // 1 << bits, not (1 << bits) - 1: at 1 << bits the output is held constant
    // high with no switching at all, which is what 100 % should mean and which
    // also removes any switching artefact at maximum brightness.
    g_max_duty = 1U << bits;
    build_brightness_table(g_duty_table, g_max_duty);

    return true;
}

// Not thread-safe by design — see the note on set_brightness_percent() in
// display.hpp. ledc_set_duty/ledc_update_duty are used rather than
// ledc_set_duty_and_update() because the latter requires the LEDC fade service,
// which allocates an interrupt this component has no other use for.
void write_duty(const std::uint32_t duty) {
    const esp_err_t set = ledc_set_duty(kSpeedMode, kChannel, duty);
    if (set != ESP_OK) {
        log_error("ledc_set_duty failed", set);
        return;
    }

    const esp_err_t update = ledc_update_duty(kSpeedMode, kChannel);
    if (update != ESP_OK) {
        log_error("ledc_update_duty failed", update);
    }
}

std::uint32_t current_duty() {
    return g_enabled ? g_duty_table[g_percent] : 0;
}

void refresh_output() {
    write_duty(current_duty());
}

std::uint8_t clamp_percent(const std::uint8_t percent) {
    return percent > 100 ? std::uint8_t{100} : percent;
}

} // namespace

bool backlight_init(const board::DisplayPins& pins, const int frequency_hz,
                    const std::uint8_t startup_percent) {
    if (g_ready) {
        backlight_deinit();
    }

    g_pin = pins.backlight;
    if (g_pin < 0) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "target declares no backlight pin; brightness control inactive");
        return true;
    }

    if (!configure_timer(frequency_hz)) {
        return false;
    }

    ledc_channel_config_t channel = {};
    channel.gpio_num = g_pin;
    channel.speed_mode = kSpeedMode;
    channel.channel = kChannel;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = kTimer;

    // Deterministic startup: the channel is created at duty 0, so the very first
    // thing this pin ever does is hold the backlight off. Nothing can display a
    // partially initialised frame.
    channel.duty = 0;
    channel.hpoint = 0;

    // Polarity is the one backlight fact that comes from board. Inverting inside
    // the LEDC output stage keeps duty semantics absolute — duty 0 is off and
    // full scale is on, whichever way the panel is wired — so no code above this
    // line has to reason about polarity, including the true-off path.
    channel.flags.output_invert = pins.backlight_active_high ? 0U : 1U;

    const esp_err_t err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        log_error("ledc_channel_config failed", err);
        return false;
    }

    g_ready = true;
    g_enabled = false;
    g_percent = clamp_percent(startup_percent);

    diagnostics::log(
        diagnostics::LogLevel::Info, kTag,
        "backlight ledc ok gpio=" + std::to_string(g_pin) + " timer=0 channel=0 clk=xtal" +
            " freq_hz=" + std::to_string(g_frequency_hz) + " resolution_bits=" +
            std::to_string(g_resolution_bits) + " max_duty=" + std::to_string(g_max_duty) +
            " active=" + (pins.backlight_active_high ? "high" : "low") +
            " startup_percent=" + std::to_string(g_percent) + " output=disabled duty=0");

    return true;
}

void backlight_deinit() {
    if (!g_ready) {
        g_pin = -1;
        return;
    }

    // Idle level 0 is the LEDC-internal off level; the output inverter
    // configured above turns that into the correct physical level for either
    // polarity, so the pin is parked dark.
    (void)ledc_stop(kSpeedMode, kChannel, 0);

    // The timer must be paused before it can be de-configured (ESP-IDF
    // ledc_timer_config_t.deconfigure).
    (void)ledc_timer_pause(kSpeedMode, kTimer);

    ledc_timer_config_t timer = {};
    timer.speed_mode = kSpeedMode;
    timer.timer_num = kTimer;
    timer.deconfigure = true;
    (void)ledc_timer_config(&timer);

    g_ready = false;
    g_enabled = false;
    g_percent = 0;
    g_pin = -1;
    g_frequency_hz = 0;
    g_resolution_bits = 0;
    g_max_duty = 0;
}

void set_backlight_enabled(const bool enabled) {
    if (!g_ready) {
        return;
    }

    g_enabled = enabled;
    refresh_output();
}

bool backlight_enabled() {
    return g_enabled;
}

void set_brightness_percent(const std::uint8_t percent) {
    if (!g_ready) {
        return;
    }

    g_percent = clamp_percent(percent);
    refresh_output();
}

std::uint8_t brightness_percent() {
    return g_percent;
}

void fade_brightness_to(const std::uint8_t target_percent, const int duration_ms) {
    if (!g_ready) {
        return;
    }

    const std::uint8_t target = clamp_percent(target_percent);

    // Nothing to interpolate when the caller asked for an instant change, or
    // when the output is disabled and no ramp would be visible anyway.
    if (duration_ms <= kFadeStepIntervalMs || !g_enabled || target == g_percent) {
        set_brightness_percent(target);
        return;
    }

    const std::int64_t from_q8 = static_cast<std::int64_t>(g_percent) * kPercentQ8One;
    const std::int64_t delta_q8 = static_cast<std::int64_t>(target) * kPercentQ8One - from_q8;
    const int steps = duration_ms / kFadeStepIntervalMs;

    // Ramp in perceptual space, interpolating between table entries so the step
    // size is a fraction of a percent rather than a whole one.
    for (int step = 1; step <= steps; ++step) {
        const std::int64_t position = from_q8 + (delta_q8 * step) / steps;
        write_duty(duty_for_percent_q8(g_duty_table, static_cast<std::uint32_t>(position)));
        vTaskDelay(pdMS_TO_TICKS(kFadeStepIntervalMs));
    }

    // Land exactly on the request rather than wherever the last interpolated
    // step happened to round to.
    g_percent = target;
    refresh_output();
}

int backlight_frequency_hz() {
    return g_frequency_hz;
}

int backlight_duty_resolution_bits() {
    return g_resolution_bits;
}

std::uint32_t backlight_duty() {
    return g_ready ? current_duty() : 0;
}

std::uint32_t backlight_max_duty() {
    return g_max_duty;
}

std::uint32_t duty_for_brightness_percent(const std::uint8_t percent) {
    if (!g_ready) {
        return 0;
    }

    return g_duty_table[clamp_percent(percent)];
}

bool set_backlight_frequency_hz(const int frequency_hz) {
    if (!g_ready) {
        return false;
    }

    if (!configure_timer(frequency_hz)) {
        return false;
    }

    // The duty table was rebuilt against the new resolution, so the old duty
    // value now means a different brightness. Re-apply the requested percentage
    // so the visible level is unchanged by the frequency switch.
    refresh_output();

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "backlight freq_hz=" + std::to_string(g_frequency_hz) +
                         " resolution_bits=" + std::to_string(g_resolution_bits) + " max_duty=" +
                         std::to_string(g_max_duty) + " percent=" + std::to_string(g_percent) +
                         " duty=" + std::to_string(current_duty()));

    return true;
}

} // namespace prd::display
