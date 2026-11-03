// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "display/display.hpp"

#include <string>

// This TU is the ESP-IDF display-adapter boundary. The ESP-IDF C headers it
// includes are not clean under the project's strict flags (undefined macros ->
// -Wundef; #include_next -> -Wpedantic, which no in-file pragma can silence).
// Both warnings are relaxed for this single file in CMakeLists.txt per
// architecture spec §12.3; all other project warnings still apply here.
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "backlight_internal.hpp"
#include "board/board.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "panel_tuning_internal.hpp"
#include "transfer_tracker.hpp"

namespace prd::display {
namespace {

// SPI host selection (deliberate, per Phase C requirement 3).
//
// SPI0 and SPI1 are reserved by the ESP32-S3 for the SPI flash and the octal
// PSRAM, both of which Phase B proved active, so they are unavailable. That
// leaves SPI2 (FSPI) and SPI3. SPI2 is chosen: it is the general-purpose host
// ESP-IDF's own LCD examples target, it has a full-featured DMA channel, and
// leaving SPI3 free keeps a second host available for any later peripheral.
//
// None of the LCD pins (IO39-42, IO45) are dedicated FSPI pads, so the signals
// route through the GPIO matrix. That costs a small amount of propagation
// delay at high clock rates but places no constraint on host choice — which is
// why the SPI host is a firmware decision and not a board fact.
constexpr spi_host_device_t kSpiHost = SPI2_HOST;

constexpr std::string_view kTag = diagnostics::tag::kDisplay;

// Longest a full-width band can legitimately take, with generous margin. One
// 320x40 RGB565 band is 25.6 kB, about 5.1 ms at the validated 40 MHz clock, so
// this is roughly a 200x allowance: it exists to catch a wedged bus, not to
// bound normal latency.
constexpr int kTransferTimeoutMs = 1000;

// Drain allowance at teardown, over the same reasoning.
constexpr int kDrainTimeoutMs = 1000;

esp_lcd_panel_io_handle_t g_io = nullptr;
esp_lcd_panel_handle_t g_panel = nullptr;
bool g_initialised = false;
bool g_bus_ready = false;
Geometry g_geometry{};
int g_clock_hz = 0;
board::DisplayPins g_pins{};

// --- Transfer completion ---------------------------------------------------
//
// Completions arrive in interrupt context, so the tracker is guarded by a
// spinlock rather than a mutex, and the waiter is woken through a counting
// semaphore. The semaphore counts completions; the tracker owns the ownership
// state and the diagnostics.
portMUX_TYPE g_transfer_lock = portMUX_INITIALIZER_UNLOCKED;
TransferTracker g_transfers{};
SemaphoreHandle_t g_transfer_done = nullptr;
std::uint32_t g_submitted = 0;
std::uint32_t g_completed = 0;
std::uint32_t g_wait_timeouts = 0;

TransferCompleteCallback g_complete_callback = nullptr;
void* g_complete_context = nullptr;

void log_error(const std::string& message, const esp_err_t error) {
    diagnostics::log(diagnostics::LogLevel::Error, kTag,
                     message + " (" + esp_err_to_name(error) + ")");
}

// esp_lcd colour-transfer completion. RUNS IN INTERRUPT CONTEXT: esp_lcd invokes
// this from the SPI transaction post-callback. Nothing here may log, allocate or
// block, and only *FromISR FreeRTOS APIs are legal.
//
// esp_lcd sets its trans-done flag only on the final chunk of each draw, so this
// fires exactly once per blit() and the count is exact rather than approximate.
//
// Returns whether a higher-priority task was woken, which esp_lcd propagates so
// the ISR can yield.
bool IRAM_ATTR on_colour_transfer_done(esp_lcd_panel_io_handle_t /*io*/,
                                       esp_lcd_panel_io_event_data_t* /*event*/,
                                       void* /*context*/) {
    portENTER_CRITICAL_ISR(&g_transfer_lock);
    const bool expected = g_transfers.on_complete();
    if (expected) {
        ++g_completed;
    }
    portEXIT_CRITICAL_ISR(&g_transfer_lock);

    BaseType_t higher_priority_woken = pdFALSE;
    if (g_transfer_done != nullptr) {
        (void)xSemaphoreGiveFromISR(g_transfer_done, &higher_priority_woken);
    }

    // The consumer callback runs here, in the same interrupt, which is why the
    // public contract states the ISR restrictions so bluntly.
    if (g_complete_callback != nullptr) {
        g_complete_callback(g_complete_context);
    }

    return higher_priority_woken == pdTRUE;
}

// Applies the selected panel tuning directly after esp_lcd's generic init and
// before the transform calls. Every transmitted byte is logged so the exact
// sequence that reached the panel is recoverable from the validation log.
//
// Ordering matters. esp_lcd_panel_init() has already set MADCTL, COLMOD and
// RAMCTRL. A verbatim vendor sequence overwrites MADCTL (its own portrait
// orientation) and RAMCTRL (little-endian). MADCTL is repaired for free by the
// swap_xy/mirror calls that follow, but RAMCTRL is only written during
// panel_init, so it is explicitly restored here — our pixels are byte-swapped
// for a big-endian panel and would render as noise otherwise. Restoring these
// two keeps the A/B strictly a colour comparison.
bool apply_tuning(const TuningMode mode) {
    if (mode == TuningMode::Generic) {
        diagnostics::log(diagnostics::LogLevel::Info, kTag,
                         std::string{"tuning="} + tuning_mode_name(mode) +
                             " (no additional registers written)");
        return true;
    }

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     std::string{"tuning="} + tuning_mode_name(mode) +
                         " applying, exact bytes follow");

    const auto sequence = tuning_sequence(mode);

    for (std::size_t i = 0; i < sequence.count; ++i) {
        const auto& entry = sequence.commands[i];
        const esp_err_t err = esp_lcd_panel_io_tx_param(
            g_io, entry.command, entry.length != 0 ? entry.params : nullptr, entry.length);
        if (err != ESP_OK) {
            log_error("tuning command failed", err);
            return false;
        }
        log_tuning_command(entry.command, entry.params, entry.length);
    }

    if (tuning_sends_gamma(mode)) {
        std::size_t length = 0;
        const std::uint8_t* positive = gamma_positive(length);
        if (esp_lcd_panel_io_tx_param(g_io, 0xE0, positive, length) != ESP_OK) {
            return false;
        }
        log_tuning_command(0xE0, positive, length);

        const std::uint8_t* negative = gamma_negative(length);
        if (esp_lcd_panel_io_tx_param(g_io, 0xE1, negative, length) != ESP_OK) {
            return false;
        }
        log_tuning_command(0xE1, negative, length);
    }

    if (sequence.overwrites_orientation_and_endianness) {
        // Restore the big-endian RAM data path the pixel generator assumes.
        // MADCTL is re-sent by the transform calls that follow.
        constexpr std::uint8_t kRamctrlBigEndian[] = {0x00, 0xF0};
        if (esp_lcd_panel_io_tx_param(g_io, 0xB0, kRamctrlBigEndian, sizeof(kRamctrlBigEndian)) !=
            ESP_OK) {
            return false;
        }
        log_tuning_command(0xB0, kRamctrlBigEndian, sizeof(kRamctrlBigEndian));
        diagnostics::log(diagnostics::LogLevel::Info, kTag,
                         "restored RAMCTRL big-endian and orientation after vendor sequence");
    }

    return true;
}

} // namespace

bool init(const Config& config) {
    if (g_initialised) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "init called while already initialised");
        return true;
    }

    g_pins = board::display_pins();
    const auto native = board::display_geometry();

    // Backlight first, created disabled at duty 0: the panel must never show an
    // uninitialised frame.
    if (!backlight_init(g_pins, config.backlight_frequency_hz, config.startup_brightness_percent)) {
        return false;
    }

    const int transformed_width = config.swap_xy ? native.native_height_px : native.native_width_px;
    const int transformed_height =
        config.swap_xy ? native.native_width_px : native.native_height_px;

    // One horizontal band is the largest single transfer.
    const int max_transfer = transformed_width * 2 * 40;

    spi_bus_config_t bus = {};
    bus.sclk_io_num = g_pins.sclk;
    bus.mosi_io_num = g_pins.mosi;
    bus.miso_io_num = g_pins.miso; // -1: write-only, leaves GPIO46 undriven
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = max_transfer;

    esp_err_t err = spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        log_error("spi_bus_initialize failed", err);
        return false;
    }
    g_bus_ready = true;

    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = g_pins.cs;
    io_config.dc_gpio_num = g_pins.data_command;
    io_config.spi_mode = 0;
    io_config.pclk_hz = static_cast<unsigned>(config.spi_clock_hz);
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;

    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config,
                                   &g_io);
    if (err != ESP_OK) {
        log_error("esp_lcd_new_panel_io_spi failed", err);
        deinit();
        return false;
    }

    // Completion signalling must exist before any pixel can be submitted,
    // otherwise a caller could lend out a buffer with no way to learn when it is
    // free again. Counting, not binary: several transfers may be in flight and a
    // binary semaphore would collapse their completions into one.
    g_transfer_done = xSemaphoreCreateCounting(kMaxTransfersInFlight, 0);
    if (g_transfer_done == nullptr) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "failed to create transfer completion semaphore");
        deinit();
        return false;
    }

    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = on_colour_transfer_done,
    };
    err = esp_lcd_panel_io_register_event_callbacks(g_io, &io_callbacks, nullptr);
    if (err != ESP_OK) {
        log_error("esp_lcd_panel_io_register_event_callbacks failed", err);
        deinit();
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = g_pins.reset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;

    err = esp_lcd_new_panel_st7789(g_io, &panel_config, &g_panel);
    if (err != ESP_OK) {
        log_error("esp_lcd_new_panel_st7789 failed", err);
        deinit();
        return false;
    }

    err = esp_lcd_panel_reset(g_panel);
    if (err != ESP_OK) {
        log_error("esp_lcd_panel_reset failed", err);
        deinit();
        return false;
    }

    err = esp_lcd_panel_init(g_panel);
    if (err != ESP_OK) {
        log_error("esp_lcd_panel_init failed", err);
        deinit();
        return false;
    }

    if (!apply_tuning(config.tuning)) {
        deinit();
        return false;
    }

    // Transform: portrait-native 240x320 presented as landscape 320x240.
    // Done in the panel (MADCTL) rather than in software, so it costs nothing
    // per frame.
    (void)esp_lcd_panel_invert_color(g_panel, config.invert_colour);
    (void)esp_lcd_panel_swap_xy(g_panel, config.swap_xy);
    (void)esp_lcd_panel_mirror(g_panel, config.mirror_x, config.mirror_y);
    (void)esp_lcd_panel_set_gap(g_panel, config.gap_x, config.gap_y);

    err = esp_lcd_panel_disp_on_off(g_panel, true);
    if (err != ESP_OK) {
        log_error("esp_lcd_panel_disp_on_off failed", err);
        deinit();
        return false;
    }

    g_geometry.width_px = transformed_width;
    g_geometry.height_px = transformed_height;
    g_clock_hz = config.spi_clock_hz;
    g_initialised = true;

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     std::string{"tuning="} + tuning_mode_name(config.tuning) + " active");

    diagnostics::log(diagnostics::LogLevel::Info, kTag,
                     "init ok host=SPI2 clock_hz=" + std::to_string(config.spi_clock_hz) +
                         " native=" + std::to_string(native.native_width_px) + "x" +
                         std::to_string(native.native_height_px) +
                         " transformed=" + std::to_string(transformed_width) + "x" +
                         std::to_string(transformed_height) +
                         " invert=" + (config.invert_colour ? "on" : "off") +
                         " swap_xy=" + (config.swap_xy ? "on" : "off") +
                         " mirror_x=" + (config.mirror_x ? "on" : "off") +
                         " mirror_y=" + (config.mirror_y ? "on" : "off"));

    return true;
}

void deinit() {
    backlight_deinit();

    // Drain before destroying anything. Tearing down the panel IO or freeing the
    // bus while DMA is still reading a caller's buffer is exactly the corruption
    // the ownership contract exists to prevent, and here the memory being read
    // may also be about to disappear.
    if (g_initialised && !wait_for_transfers(kDrainTimeoutMs)) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "transfers still in flight at deinit; tearing down anyway "
                         "after " +
                             std::to_string(kDrainTimeoutMs) + " ms");
    }

    if (g_panel != nullptr) {
        (void)esp_lcd_panel_del(g_panel);
        g_panel = nullptr;
    }
    if (g_io != nullptr) {
        (void)esp_lcd_panel_io_del(g_io);
        g_io = nullptr;
    }
    if (g_bus_ready) {
        (void)spi_bus_free(kSpiHost);
        g_bus_ready = false;
    }
    if (g_transfer_done != nullptr) {
        vSemaphoreDelete(g_transfer_done);
        g_transfer_done = nullptr;
    }

    // No completion can arrive now: the panel IO that would raise it is gone.
    portENTER_CRITICAL(&g_transfer_lock);
    g_transfers.reset();
    portEXIT_CRITICAL(&g_transfer_lock);

    g_complete_callback = nullptr;
    g_complete_context = nullptr;

    g_initialised = false;
    g_geometry = Geometry{};
    g_clock_hz = 0;
}

bool is_initialised() {
    return g_initialised;
}

Geometry geometry() {
    return g_geometry;
}

int spi_clock_hz() {
    return g_clock_hz;
}

void set_transfer_complete_callback(const TransferCompleteCallback callback, void* const context) {
    // Written with completions masked, so an interrupt can never observe the
    // callback and its context half-updated.
    portENTER_CRITICAL(&g_transfer_lock);
    g_complete_callback = callback;
    g_complete_context = context;
    portEXIT_CRITICAL(&g_transfer_lock);
}

bool blit(const int x_start, const int y_start, const int x_end, const int y_end,
          const std::uint16_t* const pixels) {
    if (!g_initialised || pixels == nullptr) {
        return false;
    }

    // Counted before submitting, not after: the completion interrupt can fire
    // before esp_lcd_panel_draw_bitmap() even returns, and a completion that
    // arrives before its submission was recorded would look spurious and would
    // leave the count stranded above zero.
    portENTER_CRITICAL(&g_transfer_lock);
    g_transfers.on_submit();
    ++g_submitted;
    portEXIT_CRITICAL(&g_transfer_lock);

    const esp_err_t err =
        esp_lcd_panel_draw_bitmap(g_panel, x_start, y_start, x_end, y_end, pixels);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&g_transfer_lock);
        g_transfers.on_submit_failed();
        --g_submitted;
        portEXIT_CRITICAL(&g_transfer_lock);

        log_error("esp_lcd_panel_draw_bitmap failed", err);
        return false;
    }

    return true;
}

bool wait_for_transfers(const int timeout_ms) {
    if (g_transfer_done == nullptr) {
        // Nothing was ever submitted through a live panel, so nothing is owed.
        return true;
    }

    const TickType_t slice = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    // Consume one completion per outstanding transfer. Re-reading the count each
    // time is deliberate: a transfer submitted from another task while this one
    // waits is also waited for, which is what "the caller owns all its buffers"
    // has to mean.
    while (true) {
        portENTER_CRITICAL(&g_transfer_lock);
        const bool idle = g_transfers.idle();
        portEXIT_CRITICAL(&g_transfer_lock);

        if (idle) {
            return true;
        }

        if (xSemaphoreTake(g_transfer_done, slice) != pdTRUE) {
            portENTER_CRITICAL(&g_transfer_lock);
            ++g_wait_timeouts;
            portEXIT_CRITICAL(&g_transfer_lock);

            diagnostics::log(diagnostics::LogLevel::Error, kTag,
                             "wait_for_transfers timed out with " +
                                 std::to_string(transfers_in_flight()) +
                                 " transfer(s) still owned by DMA");
            return false;
        }
    }
}

bool blit_blocking(const int x_start, const int y_start, const int x_end, const int y_end,
                   const std::uint16_t* const pixels, const int timeout_ms) {
    if (!blit(x_start, y_start, x_end, y_end, pixels)) {
        return false;
    }

    return wait_for_transfers(timeout_ms);
}

std::uint32_t transfers_in_flight() {
    portENTER_CRITICAL(&g_transfer_lock);
    const std::uint32_t pending = g_transfers.pending();
    portEXIT_CRITICAL(&g_transfer_lock);
    return pending;
}

TransferStatistics transfer_statistics() {
    TransferStatistics stats;

    portENTER_CRITICAL(&g_transfer_lock);
    stats.submitted = g_submitted;
    stats.completed = g_completed;
    stats.spurious_completions = g_transfers.spurious_completions();
    stats.wait_timeouts = g_wait_timeouts;
    stats.peak_in_flight = g_transfers.peak_pending();
    portEXIT_CRITICAL(&g_transfer_lock);

    return stats;
}

} // namespace prd::display
