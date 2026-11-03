// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "display/panel_tuning.hpp"

// Display hardware interface (Architecture Spec §5.5). This component owns
// esp_lcd, panel initialisation and the panel transform. It owns no product
// UI: nothing here knows what is being drawn or why.
//
// Only this component may call esp_lcd (Architecture Spec constraint for
// Milestone 1 Phase C).

namespace prd::display {

// Starting point for bring-up: the lowest clock, used before any frequency
// has been proven on hardware.
inline constexpr int kConservativeSpiClockHz = 10 * 1000 * 1000;

// Validated default (Phase C, 2026-08-02). 10, 20, 40 and 80 MHz all produced
// a clean image on the physical unit, so 80 MHz is the demonstrated ceiling.
// 40 MHz is chosen instead, deliberately keeping a 2x margin:
//
//   * the LCD signals route through the GPIO matrix rather than dedicated
//     IOMUX pads, which erodes timing margin at the top of the range;
//   * SPI stability with Wi-Fi active is still unvalidated and is explicitly
//     deferred by Architecture Spec §25.5, so headroom is worth keeping until
//     Milestone 3/4 exercises the radio;
//   * the product is a mostly-static glanceable display (Architecture Spec
//     §17.6 — no 30/60 FPS requirement). 40 MHz already redraws the full
//     320x240 frame in roughly 31 ms, far beyond what this UI needs.
//
// Raising this to 80 MHz remains available once Wi-Fi coexistence is proven.
inline constexpr int kValidatedSpiClockHz = 40 * 1000 * 1000;

// Backlight PWM frequency used before any frequency has been proven on
// hardware. Deliberately mid-range: high enough to be far above flicker fusion,
// low enough to leave a useful duty resolution.
inline constexpr int kConservativePwmFrequencyHz = 5000;

// Validated default (Phase D, 2026-08-03). 200 Hz, 1, 2, 5, 10, 20 and 40 kHz
// were all swept on the physical unit. None of them produced visible flicker,
// phone-camera banding, audible noise or any image corruption while the timer
// was re-timed, so the perceptual axes did not discriminate at all — including
// the 200 Hz negative control.
//
// The axis that does discriminate is duty resolution, which the LEDC timer
// derives from the frequency: 14 bits at or below 2441 Hz, 12 at 5 kHz, 11 at
// 10 kHz, 10 at 20 kHz, 9 at 40 kHz. 5 kHz is selected because:
//
//   * 12 bits is demonstrably enough — the fade was observed smooth with no
//     stepping, and 1 % is visible in a dark room, so more resolution buys
//     nothing measurable;
//   * at 20 kHz and above the whole 1-5 % range collapses onto a single duty
//     step, which would destroy the dark-room range this display needs;
//   * 5 kHz keeps roughly 25x margin over the 200 Hz region for people more
//     flicker-sensitive than one observer and for cameras with a faster shutter
//     than the automatic exposure used here. That margin is worth keeping, since
//     this product will be photographed and filmed.
//
// Every step of the brightness and fade validation was performed at this
// frequency; see hardware/validation/waveshare-esp32-s3-lcd-2.8.md.
inline constexpr int kValidatedPwmFrequencyHz = kConservativePwmFrequencyHz;

// Lowest brightness that is visible at all on the physical unit, measured in a
// dark room in Phase D: 1 % is duty 1 of 4096, and it reads as lit rather than
// off. 1 % and 2 % share that duty value because it is the resolution floor.
//
// Advisory. The driver honours every percentage from 1 to 100, so this reports
// what the hardware can do; it is not a limit the driver enforces. Which level a
// night or dark-room UI should actually offer is a Milestone 2 decision — the
// measured comfort figures are in the validation record.
inline constexpr std::uint8_t kMinimumVisibleBrightnessPercent = 1;

// Comfortable brightness for normal indoor use, measured in Phase D. Used as the
// startup brightness so lighting the panel needs no guesswork.
inline constexpr std::uint8_t kDefaultBrightnessPercent = 50;

struct Config {
    int spi_clock_hz{kValidatedSpiClockHz};

    // Transform and inversion, all confirmed on hardware in Phase C:
    // corner markers landed in the correct logical corners, the 1 px border
    // was intact on all four edges (so no gap correction is needed), and the
    // field rendered dark with a white border (so inversion is right).
    //
    // Landscape is presented with the panel ribbon exiting to the RIGHT, so
    // logical x=0 is the edge furthest from the PCB. The enclosure must place
    // the board on the right of the viewed image, or mirror_x/mirror_y need
    // flipping.
    bool invert_colour{true};
    bool swap_xy{true}; // portrait-native 240x320 -> landscape 320x240
    bool mirror_x{true};
    bool mirror_y{false};
    int gap_x{0};
    int gap_y{0};

    // Panel tuning applied after esp_lcd's generic init. Selected by Kconfig,
    // so every build is deterministic about which sequence it carries.
    //
    // Gamma-only is the accepted default for this board (Phase C1). The other
    // modes are gated on the hardware-validation test patterns in Kconfig, so
    // a debug or release image cannot select a rejected experimental mode —
    // and the fallback below is the accepted default, not the control.
#if defined(CONFIG_PRD_DISPLAY_TUNING_GENERIC)
    TuningMode tuning{TuningMode::Generic};
#elif defined(CONFIG_PRD_DISPLAY_TUNING_VENDOR_EXACT)
    TuningMode tuning{TuningMode::VendorExact};
#elif defined(CONFIG_PRD_DISPLAY_TUNING_VENDOR_ANALOGUE_GAMMA)
    TuningMode tuning{TuningMode::VendorAnalogueAndGamma};
#else
    TuningMode tuning{TuningMode::VendorGammaOnly};
#endif

    // Backlight PWM frequency. The LEDC timer, channel, clock source and duty
    // resolution are firmware resource choices, not board facts; only the GPIO
    // and its polarity come from board. See the rationale block at the top of
    // src/backlight.cpp.
    int backlight_frequency_hz{kValidatedPwmFrequencyHz};

    // Deterministic startup: init() always creates the LEDC channel with duty 0
    // and the output disabled, so no partially drawn frame can be visible. This
    // is the brightness a later set_backlight_enabled(true) applies, which makes
    // lighting the panel a single call rather than a guess.
    std::uint8_t startup_brightness_percent{kDefaultBrightnessPercent};
};

// Resolution after the panel transform.
struct Geometry {
    int width_px{0};
    int height_px{0};
};

[[nodiscard]] bool init(const Config& config);

// Releases the panel, panel IO and SPI bus. Used by the repeated-init
// validation so initialisation can be exercised more than once.
void deinit();

[[nodiscard]] bool is_initialised();

[[nodiscard]] Geometry geometry();

[[nodiscard]] int spi_clock_hz();

// --- Backlight -------------------------------------------------------------
//
// LEDC PWM on the board's backlight pin, brought up and torn down with the
// panel. Brightness is expressed as a percentage and mapped onto duty through a
// perceptual curve, so equal percentage steps look like equal brightness steps
// (see src/brightness_curve.hpp).
//
// NOT thread-safe: these functions drive one LEDC channel through ESP-IDF APIs
// that ESP-IDF itself documents as unsafe to call for a single channel from
// several tasks at once. Brightness must be owned by one task, which matches the
// task model in Architecture Spec §10. No mutex is imposed here because a
// second owner would be an architecture change, not a locking problem.

// Blanks or unblanks the panel while preserving the brightness setting.
// Disabled means duty 0 — the LED string is not driven at all, not driven dimly.
void set_backlight_enabled(bool enabled);

[[nodiscard]] bool backlight_enabled();

// Sets the requested brightness, 0-100 (clamped). Takes effect immediately when
// the backlight is enabled, and is remembered when it is not. 0 is true off, by
// the same duty-0 path as set_backlight_enabled(false).
void set_brightness_percent(std::uint8_t percent);

[[nodiscard]] std::uint8_t brightness_percent();

// A fraction of the light the panel emits, as a percentage.
//
// ⛔ A NAMED TYPE BECAUSE THE WHOLE DEFECT BELOW WAS A PERCENTAGE USED IN THE WRONG
// DOMAIN. Two bare percentages side by side are told apart only by their order at the
// call site, and swapping them compiles: percent_for_light_fraction(15, 80) would
// quietly return 14 instead of 34. That is the same class of silent-wrong-brightness
// error the correction exists to remove, so the units are in the type. Same remedy
// backoff.hpp's Jitter and server_view.hpp's CsrfToken were introduced for, and the
// same clang-tidy check — bugprone-easily-swappable-parameters — asks for it here.
struct LightFraction {
    std::uint8_t percent{0};
};

// The brightness whose EMITTED LIGHT is `light` % of the light emitted at
// `ceiling_percent`. Both inputs are clamped to 0-100.
//
// ⛔ ASK FOR LIGHT, NOT FOR A FRACTION OF THE CONTROL VALUE. Brightness percentages
// are perceptually pre-distorted (see the curve note above set_brightness_percent),
// so `ceiling * f / 100` emits f^2.2 of the light rather than f — at f = 15 % that
// is 1.5 %, which is a dark panel rather than a dimmed one. The display policy did
// exactly that, and the panel measured it: at a ceiling of 50 the rule asked for 7 %,
// which is duty 12 against the ceiling's 891. Anything wanting "a fraction as bright"
// must come through here.
//
// ⬅️ DEFINED IN src/brightness_curve.cpp, BESIDE THE EXPONENT IT INVERTS. Declared
// here because app_core calls it; defined there so the forward and inverse mappings
// cannot drift into two constants. The gamma stays a property of the backlight rather
// than of any caller's policy.
//
// Guarantees, every one of them exercised by the host tests:
//
//   * result <= ceiling_percent  - a dim panel can never be brighter than an awake one
//   * result >= 1 when both inputs are non-zero
//                                - a "dimmed" panel that rounds to zero is an off panel
//   * result == 0 iff either input is zero
//   * non-decreasing in each argument while the other is held
[[nodiscard]] std::uint8_t percent_for_light_fraction(std::uint8_t ceiling_percent,
                                                      LightFraction light);

// Ramps to `percent` over `duration_ms`, stepping in perceptual space.
//
// Blocks the calling task for the duration — it is a timed ramp, so there is
// nothing to return early to. Callers already run on their own task. A duration
// at or below one step, or a disabled backlight, applies the target immediately.
void fade_brightness_to(std::uint8_t percent, int duration_ms);

[[nodiscard]] int backlight_frequency_hz();
[[nodiscard]] int backlight_duty_resolution_bits();
[[nodiscard]] std::uint32_t backlight_duty();
[[nodiscard]] std::uint32_t backlight_max_duty();

// The duty this brightness maps to through the perceptual curve, without changing anything.
//
// ⬅️ EXISTS SO A BENCH ROW CAN STATE THE OUTPUT RATHER THAN A PERCENTAGE. A control percentage
// alone is not the panel's output — the curve is what turns it into one — and the whole reason
// the Dimmed defect survived to hardware is that percentages were compared where light mattered.
// Reads the same table set_brightness_percent() writes from, so a logged duty is the duty.
[[nodiscard]] std::uint32_t duty_for_brightness_percent(std::uint8_t percent);

// Re-times the LEDC timer in place, keeping the current brightness, and takes
// the best duty resolution the new frequency allows. The panel, its SPI bus and
// its contents are untouched.
//
// Exists for the Phase D frequency selection; product code should take the
// frequency from Config at init() instead.
[[nodiscard]] bool set_backlight_frequency_hz(int frequency_hz);

// --- Pixel transfer and buffer ownership -----------------------------------
//
// The panel is driven over SPI with DMA, and the transfer is ASYNCHRONOUS. This
// is the contract; getting it wrong corrupts pixels silently.
//
// OWNERSHIP RULE
//
//   blit() lends `pixels` to the panel. From the moment it returns true until
//   the matching completion is signalled, the buffer belongs to DMA: the caller
//   must not modify it, free it, or let it go out of scope. Reading it is safe.
//
//   Completion is signalled in exactly one of two ways, and every blit()
//   produces exactly one completion:
//     * wait_for_transfers() returns true — every outstanding buffer is free; or
//     * the callback registered with set_transfer_complete_callback() fires.
//
//   blit_blocking() does both for you: on return the buffer is yours again.
//
// Callers that only ever use blit_blocking() cannot get this wrong. The
// asynchronous pair exists because a display pipeline needs to prepare the next
// buffer while the current one is in flight, and that is the path a future
// double-buffered consumer will take.

// Number of transfers that may be in flight at once. Exceeding it does not fail:
// esp_lcd reclaims a slot first, which blocks the caller until one frees.
inline constexpr int kMaxTransfersInFlight = 10;

// Completion notification.
//
// EXECUTION CONTEXT: called from the SPI transaction-complete INTERRUPT, not from
// a task. It must be ISR-safe — no logging, no allocation, no blocking call, no
// mutex, and only *FromISR FreeRTOS APIs. Keep it to setting a flag or giving a
// semaphore. `context` is passed through untouched.
//
// Register before the first blit(); pass nullptr to unregister. Registering does
// not disable wait_for_transfers(), which keeps working alongside it.
using TransferCompleteCallback = void (*)(void* context);

void set_transfer_complete_callback(TransferCompleteCallback callback, void* context);

// Submits one horizontal band of RGB565 pixels. Coordinates are in transformed
// (landscape) space. Exposed so the pattern generator does not need esp_lcd.
//
// ASYNCHRONOUS — see the ownership rule above. Returns false if the transfer was
// not submitted, in which case the buffer was never lent out.
[[nodiscard]] bool blit(int x_start, int y_start, int x_end, int y_end,
                        const std::uint16_t* pixels);

// Blocks until every outstanding transfer has completed, i.e. until the caller
// owns all of its buffers again. Returns false on timeout, which leaves the
// buffers still owned by DMA — a timeout is a signal that the bus is wedged, not
// permission to reuse the memory. A negative timeout waits indefinitely.
[[nodiscard]] bool wait_for_transfers(int timeout_ms);

// Submits and waits. The buffer is free to reuse the instant this returns,
// whatever it returns. Built on the same completion mechanism as the
// asynchronous pair, not on incidental blocking.
[[nodiscard]] bool blit_blocking(int x_start, int y_start, int x_end, int y_end,
                                 const std::uint16_t* pixels, int timeout_ms);

// Transfers still owned by DMA. Zero means every submitted buffer is free.
[[nodiscard]] std::uint32_t transfers_in_flight();

// Diagnostics for the completion path. Both must stay at zero.
struct TransferStatistics {
    std::uint32_t submitted{0};
    std::uint32_t completed{0};

    // Completions with no transfer outstanding — would indicate the one-callback-
    // per-blit assumption this driver relies on has been broken.
    std::uint32_t spurious_completions{0};

    // Waits that gave up with buffers still in flight.
    std::uint32_t wait_timeouts{0};

    // Highest number of transfers ever simultaneously in flight.
    std::uint32_t peak_in_flight{0};
};

[[nodiscard]] TransferStatistics transfer_statistics();

} // namespace prd::display
