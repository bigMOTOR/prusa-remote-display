// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "button/button.hpp"

#include <atomic>
#include <string>
#include <string_view>

// This TU is the ESP-IDF input-adapter boundary. Like the display and platform
// adapters, the ESP-IDF C headers it includes are not clean under the project's
// strict flags (undefined macros -> -Wundef; #include_next -> -Wpedantic, which
// no in-file pragma can silence). Both warnings are relaxed for this single file
// in CMakeLists.txt per architecture spec §12.3; all other project warnings
// still apply here.
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board/board.hpp"
#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"
#include "diagnostics/task_report.hpp"
#include "platform/clock.hpp"
#include "press_classifier.hpp"

// ---------------------------------------------------------------------------
// Cross-task ownership
// ---------------------------------------------------------------------------
//
// Three actors touch this component, and the split between them is the whole of
// its thread-safety argument:
//
//   the edge interrupt   reads the pin, stamps the time, posts a RawEdge. Touches
//                        nothing else. IRAM-resident (see below).
//   the classifier task  the ONLY owner of the PressClassifier. Nothing outside
//                        this task may touch that object, or read a pointer to it.
//   any other task       calls wait_for_event(), is_pressed(), statistics(). Every
//                        one of those reads an atomic or a FreeRTOS queue and
//                        nothing else.
//
// The debounced level is therefore *published* by the task into an atomic rather
// than read out of the classifier by whoever asks. Reading classifier fields from
// another task was a data race and, worse, a use-after-free waiting for a deinit()
// to land between the null check and the read.

namespace prd::button {
namespace {

constexpr std::string_view kTag = diagnostics::tag::kButton;

// Poll interval used only while something is in flight — a level change being
// settled, or a press being held and timed. When the line is idle the task
// blocks indefinitely and costs nothing; edges wake it.
constexpr int kPollIntervalMs = 10;

// Raw edge capacity. Sized for a burst of contact bounce far worse than this
// switch produces, so the ISR effectively never has to drop. It is bounded
// regardless, because an unbounded queue would just move the failure.
constexpr UBaseType_t kEdgeQueueLength = 32;

// Classified gestures are human-paced — tens of milliseconds apart at the very
// fastest — so this only fills if the consumer has stopped draining it.
constexpr UBaseType_t kEventQueueLength = 8;

constexpr char kTaskName[] = "prd_button";
constexpr std::uint32_t kTaskStackBytes = 4096;

// Above the display validation task (4) so input latency does not depend on
// whatever is drawing, and well below any driver or networking task.
constexpr UBaseType_t kTaskPriority = 5;

// Longest to wait for the classifier task to acknowledge a stop.
constexpr TickType_t kStopTimeout = pdMS_TO_TICKS(500);

struct RawEdge {
    bool pressed;
    std::int64_t timestamp_us;
};

// ---------------------------------------------------------------------------
// Resources that are created once and never destroyed
// ---------------------------------------------------------------------------
//
// The queues and the stop semaphore outlive every init()/deinit() cycle on
// purpose. Deleting a queue that another task may be blocked on is a
// use-after-free with no safe ordering available: FreeRTOS gives no way to know
// whether a reader is inside xQueueReceive, and wait_for_event() blocks
// indefinitely by contract. Keeping them alive removes the hazard by construction
// rather than trying to win a race against it.
//
// The cost is 32*16 + 8*4 bytes plus one semaphore — under 600 bytes, held for the
// life of the process. deinit() resets them instead of freeing them, so a
// subsequent init() starts clean.
QueueHandle_t g_edge_queue = nullptr;
QueueHandle_t g_event_queue = nullptr;
SemaphoreHandle_t g_task_done = nullptr;

board::ButtonPins g_pins{};

// Owned exclusively by the classifier task once it starts, and by init()/deinit()
// while it is not running. Never read from any other task.
PressClassifier* g_classifier = nullptr;
alignas(PressClassifier) unsigned char g_classifier_storage[sizeof(PressClassifier)];

// Release-stored by init() after every resource is in place, acquire-loaded by the
// public entry points. That pairing is what makes the queue handles safe to read:
// a caller that observes `true` also observes everything written before it.
std::atomic<bool> g_initialised{false};
std::atomic<bool> g_stop{false};

// The debounced level, published by the classifier task. This is what is_pressed()
// reads; the classifier itself stays private to the task.
std::atomic<bool> g_pressed{false};

std::atomic<std::uint32_t> g_raw_edges{0};
std::atomic<std::uint32_t> g_raw_edges_dropped{0};
std::atomic<std::uint32_t> g_events_emitted{0};
std::atomic<std::uint32_t> g_events_dropped{0};

void log_error(const std::string& message, const esp_err_t error) {
    diagnostics::log(diagnostics::LogLevel::Error, kTag,
                     message + " (" + esp_err_to_name(error) + ")");
}

// IRAM-resident: both are on the interrupt path, and at the debug profile's
// optimisation level a static function is not reliably inlined.
IRAM_ATTR bool level_is_pressed(const int level) {
    return g_pins.active_low ? (level == 0) : (level != 0);
}

IRAM_ATTR bool read_pressed() {
    return level_is_pressed(gpio_get_level(static_cast<gpio_num_t>(g_pins.gpio)));
}

// Minimal edge capture, per the required pipeline: read the level, stamp it, hand
// it to task context. No classification, no logging, no allocation, and it never
// blocks — a full queue drops the edge and counts it rather than waiting.
//
// IRAM-SAFE, and it has to be. The interrupt is allocated with ESP_INTR_FLAG_IRAM
// so it keeps firing while the flash cache is disabled — which is what an NVS write
// (Milestone 3) and an OTA write (Milestone 7) both do. Every call below is
// IRAM-resident: platform::monotonic_time() is marked so and wraps
// esp_timer_get_time(), which is IRAM by default; gpio_get_level() is placed in
// IRAM by CONFIG_GPIO_CTRL_FUNC_IN_IRAM, set in sdkconfig.defaults; the FreeRTOS
// FromISR family is in IRAM unless CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is
// enabled, which it is not; and the atomic increments compile to inline
// instructions.
//
// Getting one of those wrong does not fail here — it faults during someone else's
// flash write, which is about the least debuggable symptom available. That is why
// the whole chain is spelled out.
void IRAM_ATTR edge_isr(void* /*argument*/) {
    RawEdge edge{};
    edge.pressed = read_pressed();
    edge.timestamp_us = platform::monotonic_time().count();

    g_raw_edges.fetch_add(1, std::memory_order_relaxed);

    if (xQueueSendFromISR(g_edge_queue, &edge, nullptr) != pdTRUE) {
        g_raw_edges_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// Publishes a classified gesture.
//
// Overflow policy: never block, never invent, never fail silently. A full queue
// means the consumer has stopped draining, so the oldest gesture is discarded to
// make room for the newest — the one the user just performed is the one they
// expect to act, and stale gestures are the safe ones to lose. Every discard is
// counted in Statistics and reported at error level, because a full queue is a
// consumer defect and must be visible rather than absorbed.
//
// Dropping is always safe in this direction: a lost gesture means nothing
// happens, whereas blocking here would stall debounce and mistime every
// subsequent press.
void publish(const Event event) {
    if (xQueueSend(g_event_queue, &event, 0) == pdTRUE) {
        g_events_emitted.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    Event discarded = Event::None;
    if (xQueueReceive(g_event_queue, &discarded, 0) == pdTRUE) {
        g_events_dropped.fetch_add(1, std::memory_order_relaxed);
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         std::string{"event queue full; discarded oldest="} +
                             std::string{event_name(discarded)} + " to admit " +
                             std::string{event_name(event)} +
                             " — the consumer is not draining events");
    }

    if (xQueueSend(g_event_queue, &event, 0) == pdTRUE) {
        g_events_emitted.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_events_dropped.fetch_add(1, std::memory_order_relaxed);
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         std::string{"event queue full; lost "} + std::string{event_name(event)});
    }
}

// Runs on the classifier task only.
void feed(const bool pressed, const std::chrono::microseconds when) {
    const Event event = g_classifier->observe(pressed, when);

    // Published after every observation, not only when a gesture resolves, so a
    // caller asking "is it down right now" gets the debounced answer while the
    // press is still in progress.
    g_pressed.store(g_classifier->is_pressed(), std::memory_order_relaxed);

    if (event != Event::None) {
        publish(event);

        // Reported here rather than on a timer: this is the end of the task's
        // deepest path — an edge drained, classified and published, with the
        // string-building log call inside publish() already charged to the stack.
        // It is also naturally rate-limited, because it only happens when a human
        // presses the button.
        diagnostics::log_task_stack(kTaskName, kTaskStackBytes);
    }
}

void classification_task(void* /*argument*/) {
    while (!g_stop.load(std::memory_order_relaxed)) {
        // Poll only while a change is settling or a press is being timed.
        const bool in_flight = g_classifier->has_pending_change() || g_classifier->is_pressed();
        const TickType_t wait = in_flight ? pdMS_TO_TICKS(kPollIntervalMs) : portMAX_DELAY;

        RawEdge edge{};
        if (xQueueReceive(g_edge_queue, &edge, wait) == pdTRUE) {
            feed(edge.pressed, std::chrono::microseconds{edge.timestamp_us});

            // Drain the rest of the burst without blocking, so bounce is
            // processed with the timestamps it was captured with.
            while (xQueueReceive(g_edge_queue, &edge, 0) == pdTRUE) {
                feed(edge.pressed, std::chrono::microseconds{edge.timestamp_us});
            }
        }

        if (g_stop.load(std::memory_order_relaxed)) {
            break;
        }

        // Authoritative observation. Whatever the interrupt delivered or lost,
        // the pin level right now is the truth, and feeding it every iteration is
        // what makes a dropped edge cost precision rather than correctness. It
        // also drives the debounce commit and the very-long threshold.
        feed(read_pressed(), platform::monotonic_time());
    }

    xSemaphoreGive(g_task_done);
    vTaskDelete(nullptr);
}

// Creates the long-lived resources on first use. Never freed — see the note above.
bool ensure_resources() {
    if (g_edge_queue == nullptr) {
        g_edge_queue = xQueueCreate(kEdgeQueueLength, sizeof(RawEdge));
    }
    if (g_event_queue == nullptr) {
        g_event_queue = xQueueCreate(kEventQueueLength, sizeof(Event));
    }
    if (g_task_done == nullptr) {
        g_task_done = xSemaphoreCreateBinary();
    }

    return g_edge_queue != nullptr && g_event_queue != nullptr && g_task_done != nullptr;
}

bool configure_input() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(g_pins.gpio);
    cfg.mode = GPIO_MODE_INPUT;

    // The board provides a 10 kΩ pull-up on this net, so the internal one stays
    // off: enabling it would put two pull-ups in parallel and quietly change the
    // strapping-pin drive strength for no benefit. If a future target has no
    // external pull-up, board reports that and the internal one is used instead.
    cfg.pull_up_en = g_pins.has_external_pull_up ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE;

    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        log_error("button gpio_config failed", err);
        return false;
    }

    // ESP_INTR_FLAG_IRAM keeps this interrupt enabled while the flash cache is
    // disabled, instead of having it masked for the duration of every flash write.
    // Without it an edge that arrives during an NVS or OTA write is simply lost,
    // and a press that both begins and ends inside a long write produces no gesture
    // at all — which for an OTA is precisely when a user is most likely to press
    // something.
    //
    // The flag applies to the shared GPIO ISR service, so it is a constraint on
    // every future handler added to it: all of them must be IRAM-safe. ESP-IDF's
    // own dispatcher already is (gpio_intr_service is IRAM_ATTR unconditionally).
    //
    // Another component may have installed the service first, with unknown flags.
    // That is not an error for us, but it does mean the IRAM guarantee is no longer
    // ours to make, so it is reported rather than assumed.
    const esp_err_t service = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (service == ESP_ERR_INVALID_STATE) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "gpio isr service was already installed by another component; "
                         "its interrupt-allocation flags are unknown, so IRAM safety "
                         "during flash writes cannot be guaranteed here");
    } else if (service != ESP_OK) {
        log_error("gpio_install_isr_service failed", service);
        return false;
    }

    const esp_err_t added =
        gpio_isr_handler_add(static_cast<gpio_num_t>(g_pins.gpio), edge_isr, nullptr);
    if (added != ESP_OK) {
        log_error("gpio_isr_handler_add failed", added);
        return false;
    }

    return true;
}

} // namespace

bool init(const Config& config) {
    if (g_initialised.load(std::memory_order_acquire)) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "init called while already initialised");
        return true;
    }

    g_pins = board::button_pins();
    if (g_pins.gpio < 0) {
        diagnostics::log(diagnostics::LogLevel::Warning, kTag,
                         "target declares no button pin; input inactive");
        return false;
    }

    if (!ensure_resources()) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "failed to allocate button queues");
        return false;
    }

    // Start from empty rather than from whatever a previous cycle left behind.
    xQueueReset(g_edge_queue);
    xQueueReset(g_event_queue);

    g_classifier = new (g_classifier_storage) PressClassifier{config.thresholds};

    g_raw_edges.store(0, std::memory_order_relaxed);
    g_raw_edges_dropped.store(0, std::memory_order_relaxed);
    g_events_emitted.store(0, std::memory_order_relaxed);
    g_events_dropped.store(0, std::memory_order_relaxed);
    g_stop.store(false, std::memory_order_relaxed);

    if (!configure_input()) {
        g_classifier->~PressClassifier();
        g_classifier = nullptr;
        return false;
    }

    // Published before the task exists, so a caller that sees is_initialised()
    // never reads a stale level from a previous cycle.
    g_pressed.store(read_pressed(), std::memory_order_relaxed);

    // Own task, with an explicitly sized stack. Carried forward from Phase C:
    // the ESP-IDF main task is 3584 bytes and overflows without the FreeRTOS
    // canary catching it.
    const BaseType_t created = xTaskCreate(classification_task, kTaskName, kTaskStackBytes, nullptr,
                                           kTaskPriority, nullptr);

    if (created != pdPASS) {
        diagnostics::log(diagnostics::LogLevel::Error, kTag, "failed to create button task");
        (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(g_pins.gpio));
        g_classifier->~PressClassifier();
        g_classifier = nullptr;
        return false;
    }

    g_initialised.store(true, std::memory_order_release);

    diagnostics::log(
        diagnostics::LogLevel::Info, kTag,
        "button ready gpio=" + std::to_string(g_pins.gpio) +
            " active=" + (g_pins.active_low ? "low" : "high") +
            " pull_up=" + (g_pins.has_external_pull_up ? "external" : "internal") +
            " hw_debounce=" + (g_pins.has_hardware_debounce ? "yes" : "no") +
            " debounce_ms=" + std::to_string(config.thresholds.debounce.count()) +
            " long_ms=" + std::to_string(config.thresholds.long_press.count()) +
            " very_long_ms=" + std::to_string(config.thresholds.very_long_press.count()) +
            " isr=iram level=" + (read_pressed() ? "pressed" : "released"));

    return true;
}

void deinit() {
    if (!g_initialised.load(std::memory_order_acquire)) {
        return;
    }

    // Closed to new callers first, so a reader that has not yet blocked gives up
    // immediately instead of waiting out its timeout.
    g_initialised.store(false, std::memory_order_release);

    (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(g_pins.gpio));
    (void)gpio_set_intr_type(static_cast<gpio_num_t>(g_pins.gpio), GPIO_INTR_DISABLE);

    // The pin is deliberately left as a plain input rather than reset:
    // gpio_reset_pin() would enable the internal pull-up, which is exactly what
    // configure_input() avoids on this strapping pin.

    g_stop.store(true, std::memory_order_relaxed);

    // Wake the task if it is blocked indefinitely. The synthetic edge reports the
    // current level, so it is a harmless observation even if it is processed.
    RawEdge wake{};
    wake.pressed = read_pressed();
    wake.timestamp_us = platform::monotonic_time().count();
    (void)xQueueSend(g_edge_queue, &wake, 0);

    const bool stopped = xSemaphoreTake(g_task_done, kStopTimeout) == pdTRUE;

    if (!stopped) {
        // The classifier is deliberately left constructed. The task may still be
        // inside observe(), and destroying the object under it would turn a stuck
        // task into a corrupted one. The queues are never freed in any case, so
        // nothing else here is at risk.
        diagnostics::log(diagnostics::LogLevel::Error, kTag,
                         "button task did not stop within " +
                             std::to_string(pdTICKS_TO_MS(kStopTimeout)) +
                             " ms; leaving its classifier constructed rather than "
                             "destroying state it may still be using");
        return;
    }

    // The task has confirmed it is gone, so this is the one safe moment to destroy
    // what only it was allowed to touch.
    if (g_classifier != nullptr) {
        g_classifier->~PressClassifier();
        g_classifier = nullptr;
    }

    // A courtesy wake for a reader already blocked in wait_for_event(). Not a
    // safety measure — the queue outlives deinit(), so a reader left blocked is
    // merely waiting, not reading freed memory. wait_for_event() treats None as
    // "no gesture" and returns false.
    const Event sentinel = Event::None;
    (void)xQueueSend(g_event_queue, &sentinel, 0);
}

bool is_initialised() {
    return g_initialised.load(std::memory_order_acquire);
}

bool wait_for_event(Event& out, const int timeout_ms) {
    // Acquire-load first: observing `true` guarantees the queue handle below was
    // published by init(), and the handle is never cleared afterwards.
    if (!g_initialised.load(std::memory_order_acquire)) {
        return false;
    }

    QueueHandle_t queue = g_event_queue;
    if (queue == nullptr) {
        return false;
    }

    const TickType_t wait = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    Event event = Event::None;
    if (xQueueReceive(queue, &event, wait) != pdTRUE) {
        return false;
    }

    // The deinit() sentinel, or a spurious None that no classification produces.
    if (event == Event::None) {
        return false;
    }

    out = event;
    return true;
}

bool is_pressed() {
    // The level the classifier task published, not a read into the classifier.
    return g_initialised.load(std::memory_order_acquire) &&
           g_pressed.load(std::memory_order_relaxed);
}

Statistics statistics() {
    Statistics stats;
    stats.raw_edges = g_raw_edges.load(std::memory_order_relaxed);
    stats.raw_edges_dropped = g_raw_edges_dropped.load(std::memory_order_relaxed);
    stats.events_emitted = g_events_emitted.load(std::memory_order_relaxed);
    stats.events_dropped = g_events_dropped.load(std::memory_order_relaxed);
    return stats;
}

} // namespace prd::button
