// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "app_model/observation.hpp"
#include "app_model/printer_data.hpp"
#include "app_model/state_layers.hpp"
#include "app_model/values.hpp"

// The typed application events — Architecture Spec §8.
//
// EVERY EVENT NAMES A FACT. Not a command, not a screen, not a request. The names
// are past tense because that is the discipline: "the printer reported this",
// "the timer fired", "the finger did this". Nothing here says "show", "open",
// "navigate" or "render". What a fact *means* is the reducer's decision, which is
// what makes the meaning testable and keeps every producer — a fake source now, a
// PrusaLink adapter later — from quietly acquiring UI opinions (§5.6: "Exact
// product actions remain in app_store or app_core, not in the driver").
//
// The one exception worth naming is ModalSurfaceDismissed, which is the completed
// observation that a modal surface has finished. Which gesture produces it is an
// interaction decision that no accepted source has taken yet, so it is not
// encoded here.
//
// Representation: a tag plus a union of trivially copyable payloads. §10.7
// requires a bounded event queue, and a queue entry's size is therefore a budget
// item — a flat struct carrying every payload at once measures about four times
// the union and would be paid on every queue slot. std::variant was the other
// candidate and was rejected: exceptions are disabled in this build
// (CONFIG_COMPILER_CXX_EXCEPTIONS=n), which leaves std::get's failure path as an
// abort rather than a diagnosable error, and the union costs nothing in
// readability once construction goes through the factories below.

namespace prd::app_model {

// Physical gestures, mirroring button::Event.
//
// Declared here rather than reused from the button component because app_model
// must not depend on a driver, and the driver must not depend upward (§4.2).
// app_core translates one into the other, which is also where a second input
// source would be adapted.
enum class ButtonGesture {
    ShortPress,
    LongPress,
    VeryLongPress,
};

[[nodiscard]] std::string_view log_label(ButtonGesture gesture);

enum class EventType {
    None,

    // --- Boot and device lifecycle ---
    BootPhaseObserved,
    HardwareProbed,
    StorageObserved,
    DeviceFaultRaised,
    RecoveryModeEntered,
    RecoveryModeExited,

    // --- Setup and configuration lifecycle ---
    ConfigurationObserved,
    ProvisioningPhaseObserved,

    // --- Network lifecycle ---
    NetworkObserved,

    // --- Printer connection lifecycle ---
    PrinterConnectionObserved,
    PrinterIdentityObserved,

    // --- Printer snapshot updates ---
    PrinterSnapshotObserved,

    // One whole `GET /api/v1/status` reading: the printer half and the optional job half, applied
    // together. See StatusObservation.
    PrinterStatusObserved,

    // --- Job lifecycle ---
    JobObserved,
    JobUnavailable,

    // --- Warnings and errors ---
    AlertRaised,
    AlertCleared,

    // --- Confidence ageing ---
    ConfidenceAged,

    // --- Firmware update and reset lifecycle ---
    OtaObserved,
    FactoryResetConfirmed,

    // The confirmation was withdrawn — Milestone 3.10. Either the user cancelled with a
    // short press, or its bounded window elapsed. Nothing is erased on this path; it exists
    // so an armed confirmation cannot linger on the panel with nothing able to clear it.
    FactoryResetCancelled,

    // ⛔ THERE IS NO FactoryResetCompleted — REMOVED 2026-08-28, audit item 5. It existed to end
    // the operation, and its ONLY production emitter was the failure path: a reset that could not
    // finish dispatched "completed", which cleared `factory_resetting` and released the device into
    // provisioning and printer work while its journal was still armed. A successful reset never
    // emitted it and never could — it ends by restarting. An event whose name asserts a completion
    // that only ever followed a failure is not worth keeping for a caller that no longer exists.
    // An incomplete reset now raises DeviceFaultReason::FactoryResetIncomplete, which is §8.2's
    // "completion or failure is returned as another event" answered truthfully.

    // --- Display power and idle ---
    DisplayIdleTimerFired,
    DisplaySleepTimerFired,

    // A meaningful change (UI Behaviour Decision 001 §4) reached a panel that was dimmed or off.
    //
    // ⛔ NOT AN INTERACTION, AND DELIBERATELY SEPARATE FROM A BUTTON GESTURE. §7.11's
    // lastInteractionAt means a person touched the device; this is the machine having something
    // worth showing. Folding the two together would let a printer coming back online look, to
    // every later rule, exactly like a user pressing the button.
    DisplayWakeRequested,
    DisplayBrightnessObserved,

    // --- Physical input ---
    ButtonGestureObserved,

    // --- Modal surface lifecycle ---
    ModalSurfaceDismissed,
};

[[nodiscard]] std::string_view log_label(EventType type);

// --- Payloads --------------------------------------------------------------

struct BootPhaseObservation {
    BootPhase phase{BootPhase::Starting};
};

struct HardwareProbeObservation {
    BoundedString<32> board_target;
    BoundedString<32> reset_reason;
    ValidationState flash_validation{ValidationState::Unknown};
    ValidationState psram_validation{ValidationState::Unknown};
};

struct StorageObservation {
    StorageStatus status{StorageStatus::Unknown};
};

struct DeviceFaultObservation {
    // Cannot express a printer error, an auth failure, an unsupported interface
    // or a connectivity loss — see DeviceFaultReason. That is the structural
    // guarantee behind UIAD 001 §10.
    DeviceFaultReason reason{DeviceFaultReason::None};
};

struct ConfigurationObservation {
    ConfigurationStatus status{ConfigurationStatus::Missing};
    std::uint32_t schema_version{0};
    std::uint32_t credential_generation{0};
    bool has_wifi_credentials{false};
    bool has_printer_configuration{false};

    // R8's candidate state — see ConfigurationState::printer_configuration_pending.
    bool printer_configuration_pending{false};

    BoundedString<64> printer_address;
    BoundedString<32> printer_alias;
    AuthMethod auth_method{AuthMethod::None};
};

struct ProvisioningObservation {
    ProvisioningPhase phase{ProvisioningPhase::Inactive};
};

// ⛔ NO SSID FIELD, ON THE SAME GROUND AS NetworkState — see device_data.hpp. The observation is the
// only route from the Wi-Fi adapter into the store, so removing it here is what makes "the home
// network name cannot enter `AppState`" a property of the type rather than of the adapter's manners.
struct NetworkObservation {
    NetworkPhase phase{NetworkPhase::Disabled};
    BoundedString<16> ip_address;
    bool signal_known{false};
    std::int8_t signal_strength_dbm{0};
    BoundedString<48> error;

    // ⛔ THE TYPED FORM OF `error`, AND THE ONLY ONE A DECISION MAY BE TAKEN FROM. `error` is the
    // §7.4 diagnostic text; this is the fact. NetworkFault::DriverUnresponsive is what raises
    // DeviceFaultReason::WifiUnavailable in the reducer, so nothing downstream ever matches a
    // string to reach the product's highest-priority presentation.
    NetworkFault fault{NetworkFault::None};
};

struct ConnectionObservation {
    ConnectionPhase phase{ConnectionPhase::Unconfigured};
    BoundedString<64> endpoint;
    AuthMethod auth_method{AuthMethod::None};
    BoundedString<24> capability_tier;
    bool response_code_known{false};
    std::uint16_t response_code{0};
    bool latency_known{false};
    std::uint32_t latency_ms{0};
    BoundedString<48> error;
};

struct PrinterIdentityObservation {
    PrinterIdentity identity;
};

// §8.1: "rejects stale responses using request generation or sequence IDs".
// A snapshot whose sequence is not greater than the last accepted one is
// rejected — which is what makes an out-of-order or duplicated response
// harmless rather than a source of flicker.
struct PrinterSnapshotObservation {
    // ⬅️ WHICH WORKER PRODUCED THIS, added 2026-08-23. The sequence below orders snapshots WITHIN a
    // worker; it cannot order them across one. A replacement worker starts counting again, so
    // without a generation its first snapshots lose to the previous worker's watermark and the panel
    // stops updating after every reconnect — silently, for as many cycles as the old worker had run.
    // A generation change resets the ordering; see snapshot_is_current().
    std::uint32_t generation{0};

    std::uint32_t sequence{0};

    PrinterState normalized{PrinterState::Unknown};

    // The printer's own string, kept whether or not it mapped (§24.8).
    BoundedString<24> raw;

    bool is_busy{false};
    bool requires_attention{false};

    TemperatureValue nozzle_current;
    TemperatureValue nozzle_target;
    TemperatureValue bed_current;
    TemperatureValue bed_target;
    TemperatureValue chamber_current;
    TemperatureValue chamber_target;
};

// The GENERAL job record, for a producer that reports a whole job: a fixture, and any future
// endpoint that actually supplies a name, a file, layers or an estimate quality.
//
// ⛔ NOT WHAT `/api/v1/status` PRODUCES. That endpoint supplies four job values and nothing else,
// and a response carrying this struct would put ~224 bytes of name, file name and source path —
// plus layers, estimate quality and a duplicate of the printer half's state and ordering — into
// the event union, and therefore into all twelve slots of the application event queue. See
// StatusJobTelemetry below, which is the endpoint-specific payload the poll path uses.
//
// Unchanged by the Printing-telemetry checkpoint, deliberately: this path is still the fixtures'.
struct JobObservation {
    std::uint32_t sequence{0};

    BoundedString<24> id;
    BoundedString<64> display_name;
    BoundedString<64> original_file_name;
    BoundedString<96> source_path;

    PrinterState normalized_state{PrinterState::Unknown};
    BoundedString<24> raw_state;

    ProgressValue progress;
    DurationValue elapsed;
    DurationValue remaining;
    LayerProgress layers;

    EstimateQuality estimate_quality{EstimateQuality::Unknown};
};

// ⛔ THE JOB TELEMETRY `/api/v1/status` ACTUALLY SUPPLIES, AND NOTHING MORE.
//
// Four values and two flags — `id_known` and `reported`. API Research Report §6.1 documents the
// endpoint's `job` object as `id`, `progress`, `time_remaining` and `time_printing`, and makes
// every one of them optional; the two flags are which of those facts the response actually stated.
// It exposes no name, no file, no path, no layer and no estimate quality, and
// Prusa-Firmware-Buddy v6.8.1 sends none of them.
//
// ⛔ WHY IT IS NOT JobObservation, WHICH WOULD HAVE BEEN THE OBVIOUS REUSE. That struct is 348
// bytes, of which ~224 are three bounded strings this endpoint never fills, and it repeats the
// ordering and the printer state that the snapshot half of the same reading already carries.
// Embedding it made `sizeof(Event)` 464 rather than 360 — and `Event` is the element type of the
// twelve-slot application event queue, so the unused fields cost 1248 bytes of STARTUP HEAP in
// every product image. The queue is built with xQueueCreate(), which allocates through
// pvPortMalloc, so this is FreeRTOS heap taken at startup and never returned — not `.bss`. The
// device's measured internal-heap low-water is near 916 bytes, which is the very pool that
// allocation comes out of. A convenience that spends more than the whole remaining headroom is
// not a convenience.
//
// ⬅️ AND NOTHING IS DUPLICATED. There is no generation, no sequence, no raw state and no
// normalized state here: all four belong to the reading, and the reading has one — the printer
// half's. The reducer derives them from there when it expands these fields into the domain's
// PrintJob, which is the only place the two halves are ever combined.
struct StatusJobTelemetry {
    // Four-byte-aligned members first, so the compact payload stays compact: laid out in
    // declaration order with the strings first this struct is larger for no reason, and it is
    // multiplied by twelve queue slots.
    DurationValue remaining;
    DurationValue elapsed;

    // The identity, which is what makes a changed job a different job in the reducer. Text
    // because PrintJob::id is text; the contract types it as an integer and the decoder renders
    // it.
    BoundedString<24> id;

    ProgressValue progress;

    // ⛔ WHETHER THE RESPONSE ACTUALLY CARRIED AN ID, WHICH IS NOT THE SAME AS AN EMPTY ONE.
    // §6.1 makes all four members of the `job` object optional, `id` included. Without this fact
    // the reducer read an absent id as an ASSERTED EMPTY IDENTITY, decided the job had changed on
    // every poll, and reset the record — losing the locally inferred start, the elapsed counter
    // and any `/api/v1/job`-owned metadata, and on a terminal response persisting a completion
    // with no identity at all. §9.3's absent-versus-zero rule, applied to a string: absence is
    // unknown, not empty, and not proof of a change.
    bool id_known{false};

    // ⛔ WHETHER THE RESPONSE CARRIED A JOB OBJECT AT ALL. False is an AFFIRMATIVE statement —
    // "this printer reports no job" — and is what starts §7.8's expiry. It is not silence: a
    // response that never arrived produces no event.
    bool reported{false};
};

// ⛔ ONE STATUS RESPONSE, WHOLE. The printer half and the optional job half of a single
// `GET /api/v1/status` body, carried together so the reducer applies them as one mutation under
// one ordering decision.
//
// ⛔ WHY THIS EXISTS RATHER THAN TWO EVENTS. Two events are two store mutations, two projections
// and two acceptance decisions, and the panel showed the gap: a new state word beside the previous
// response's figures for one frame. Worse, the terminal-completion snapshot reads `current_job`,
// so a direct PRINTING 79% → FINISHED 100% response recorded the completion at 79 % — the job half
// of the very same response had not been applied yet. Neither is fixable by ordering two
// dispatches; both need one application.
//
// The single-fact events below remain, because a fixture legitimately publishes a job with no
// printer reading and vice versa. What must not be rebuilt is a hand-written pair of them standing
// in for one response.
struct StatusObservation {
    // Carries the response's ordering identity, its raw and normalized state, and its
    // temperatures. There is no second copy of any of them.
    PrinterSnapshotObservation printer;

    StatusJobTelemetry job;
};

struct JobUnavailableObservation {
    std::uint32_t sequence{0};
};

struct AlertObservation {
    AlertSeverity severity{AlertSeverity::Info};
    AlertSource source{AlertSource::PrinterState};
    BoundedString<24> code;
    BoundedString<48> title;
    BoundedString<96> message;
    BoundedString<64> help_url;
    bool is_actionable{false};
};

struct OtaObservation {
    OtaPhase phase{OtaPhase::Idle};
    BoundedString<32> available_version;
    ProgressValue progress;
    BoundedString<48> error;
    bool pending_validation{false};
};

struct DisplayBrightnessObservation {
    std::uint8_t brightness_percent{100};
};

struct ButtonGestureObservation {
    ButtonGesture gesture{ButtonGesture::ShortPress};
};

// --- Size budgets, checked in every profile ---------------------------------
//
// ⛔ THESE ARE STARTUP-HEAP BUDGETS, NOT STYLE. `Event` is the element type of app_core's
// twelve-slot application event queue and `StatusObservation` is the payload the poll path
// publishes, so every byte added to either is multiplied by twelve in a product image. That queue
// is created with xQueueCreate(), which allocates through pvPortMalloc — so the cost lands on the
// FreeRTOS heap at startup rather than in `.bss`, and it comes out of the very pool the device's
// measured internal-heap low-water of ~916 bytes describes. The first version of the
// Printing-telemetry checkpoint spent 1248 bytes of that by embedding the general JobObservation
// in the status payload. It compiled, it passed every test, and it would have been taken to
// hardware.
//
// A budget that lives in a review comment is one that is checked when someone remembers. These are
// checked by the compiler, on the target as well as the host.
//
// ⬅️ THE `Event` FIGURE IS ITS PRE-CHECKPOINT SIZE. Printing telemetry is product functionality and
// is present in debug and release; what this asserts is that it arrived at NO cost to the queue.
static_assert(sizeof(StatusJobTelemetry) <= 64,
              "the status job payload must stay compact: it is what keeps StatusObservation out of "
              "the event union's high-water mark");

static_assert(sizeof(StatusObservation) <= 160,
              "a status reading is a snapshot plus four values; anything larger means a general "
              "job record has been embedded again");

// --- Event -----------------------------------------------------------------

struct Event {
    EventType type{EventType::None};

    // Monotonic time at which the fact was observed. Every event carries one:
    // the reducer reads no clock, so this is the only time it has (§8.1, and the
    // purity rule that ageing arrives as an explicit timestamp or event).
    Timestamp observed_at{};

    // The largest payload is initialized rather than an arbitrary one, so a
    // default-constructed Event leaves as little of the union's storage
    // indeterminate as the language allows.
    union {
        // ⛔ THE LARGEST MEMBER IS THE ONE THAT IS INITIALIZED, and it is JobObservation. The rule
        // this restores: a default-constructed Event leaves as little of the union's storage
        // indeterminate as the language allows, which is only true if the initialized member is the
        // one that sizes it.
        //
        // ⬅️ StatusObservation BRIEFLY HELD THIS PLACE AND NO LONGER SHOULD. While it embedded the
        // general JobObservation it was 444 bytes and genuinely the largest; compacting it to 144
        // made JobObservation the largest again, and leaving `status{}` as the initialized member
        // then left 200 bytes of the union indeterminate on every default construction — for no
        // reason, and while a comment claimed the opposite. The structural assertion below is what
        // keeps the two facts from drifting apart again.
        JobObservation job{};
        StatusObservation status;
        BootPhaseObservation boot;
        HardwareProbeObservation hardware;
        StorageObservation storage;
        DeviceFaultObservation fault;
        ConfigurationObservation configuration;
        ProvisioningObservation provisioning;
        NetworkObservation network;
        ConnectionObservation connection;
        PrinterIdentityObservation identity;
        PrinterSnapshotObservation printer;
        JobUnavailableObservation job_absence;
        AlertObservation alert;
        OtaObservation ota;
        DisplayBrightnessObservation brightness;
        ButtonGestureObservation button;
    };

    // --- Factories ---
    //
    // Construction goes through these so no call site has to know which union
    // member belongs to which tag. The composite ones take their payload whole
    // rather than a parameter list: a long list of same-typed parameters is
    // trivially transposable at a call site, and the payload struct makes each
    // field named where it is written.

    [[nodiscard]] static Event boot_phase(Timestamp moment, BootPhase phase);
    [[nodiscard]] static Event hardware_probed(Timestamp moment,
                                               const HardwareProbeObservation& observation);
    [[nodiscard]] static Event storage_observed(Timestamp moment, StorageStatus status);
    [[nodiscard]] static Event device_fault(Timestamp moment, DeviceFaultReason reason);
    [[nodiscard]] static Event recovery_mode_entered(Timestamp moment);
    [[nodiscard]] static Event recovery_mode_exited(Timestamp moment);

    [[nodiscard]] static Event configuration_observed(Timestamp moment,
                                                      const ConfigurationObservation& observation);
    [[nodiscard]] static Event provisioning_phase(Timestamp moment, ProvisioningPhase phase);

    [[nodiscard]] static Event network_observed(Timestamp moment,
                                                const NetworkObservation& observation);

    [[nodiscard]] static Event connection_observed(Timestamp moment,
                                                   const ConnectionObservation& observation);
    [[nodiscard]] static Event printer_identity(Timestamp moment, const PrinterIdentity& identity);

    [[nodiscard]] static Event printer_snapshot(Timestamp moment,
                                                const PrinterSnapshotObservation& observation);

    // ⛔ THE WHOLE OF ONE STATUS RESPONSE. What the PrusaLink poll path publishes; see
    // StatusObservation for why it is one event and not two.
    [[nodiscard]] static Event printer_status(Timestamp moment,
                                              const StatusObservation& observation);

    [[nodiscard]] static Event job_observed(Timestamp moment, const JobObservation& observation);
    [[nodiscard]] static Event job_unavailable(Timestamp moment, std::uint32_t sequence);

    [[nodiscard]] static Event alert_raised(Timestamp moment, const AlertObservation& observation);
    [[nodiscard]] static Event alert_cleared(Timestamp moment);

    [[nodiscard]] static Event confidence_aged(Timestamp moment);

    [[nodiscard]] static Event ota_observed(Timestamp moment, const OtaObservation& observation);
    [[nodiscard]] static Event factory_reset_confirmed(Timestamp moment);
    [[nodiscard]] static Event factory_reset_cancelled(Timestamp moment);

    [[nodiscard]] static Event display_idle_timer_fired(Timestamp moment);
    [[nodiscard]] static Event display_sleep_timer_fired(Timestamp moment);
    [[nodiscard]] static Event display_wake(Timestamp moment);
    [[nodiscard]] static Event display_brightness(Timestamp moment,
                                                  std::uint8_t brightness_percent);

    [[nodiscard]] static Event button_gesture(Timestamp moment, ButtonGesture gesture);

    [[nodiscard]] static Event modal_surface_dismissed(Timestamp moment);
};

// ⛔ THE UNION'S INITIALIZED MEMBER MUST BE ITS LARGEST. Event initializes `job`, so
// JobObservation has to remain at least as large as every other member — and StatusObservation is
// the one that has already overtaken it once. If this ever fails, swap which member carries the
// `{}` rather than deleting the assertion: the point is that a default-constructed Event leaves as
// little indeterminate storage as the language allows.
static_assert(sizeof(JobObservation) >= sizeof(StatusObservation),
              "Event initializes `job`, so it must be the union's largest member; move the {} to "
              "whichever member is largest rather than removing this check");

// A queue slot costs this much (§10.7, §21). Named so growth is a decision.
//
// ⛔ TIGHTENED FROM 512 TO 360 ON 2026-09-03, BECAUSE 512 DID NOT CATCH THE ONE CASE IT EXISTED
// FOR. The Printing-telemetry checkpoint embedded the general 348-byte JobObservation in the
// status payload, took `sizeof(Event)` from 360 to 464, and spent 1248 bytes of STARTUP HEAP
// across the twelve queue slots — comfortably inside a 512-byte budget, and larger than the
// device's entire measured internal-heap low-water of ~916 bytes, which is the pool xQueueCreate()
// draws from. A budget with that much slack in it is a budget that only fails after the damage is
// interesting.
//
// 360 is the size before that checkpoint. Printing telemetry is product functionality, present in
// debug and release, and this asserts that it costs the queue NOTHING: the endpoint-specific
// StatusJobTelemetry fits inside the union's existing high-water mark, which the general
// JobObservation sets.
//
// ⬅️ RAISING THIS IS ALLOWED, AND IS A DECISION WITH A NUMBER ATTACHED. Twelve slots, so every
// byte here is twelve bytes of FreeRTOS heap a product image cannot get back. Measure the heap
// headroom first.
static_assert(sizeof(Event) <= 360,
              "Event sizes the twelve-slot bounded event queue; every byte costs twelve bytes of "
              "FreeRTOS startup heap. See the note above before raising this.");

} // namespace prd::app_model
