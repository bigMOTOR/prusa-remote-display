// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/observation.hpp"
#include "app_model/values.hpp"

// Printer-domain data: Architecture Spec §7.6 to §7.10.
//
// This is what the printer reported, normalized into product vocabulary. It is a
// separate layer from the UI architecture's SubjectState (state_layers.hpp), and
// the mapping between them is deliberately not one to one — see
// subject_from_printer_state() in reducer.hpp.
//
// No PrusaLink concept appears here: no endpoint, no JSON, no HTTP type, no
// vendor field name (§5.1, §5.8 — "The component exposes domain-oriented
// results, not raw cJSON objects").

namespace prd::app_model {

// Architecture Spec §7.7 normalized states. A superset of SubjectState on
// purpose: `attention` and `error` are printer states that become UI
// *conditions*, and `offline` is a printer state that becomes reduced
// *confidence*. Collapsing this into SubjectState would destroy exactly the
// distinction UIAD 001 §2 exists to preserve.
enum class PrinterState {
    Unknown,
    Idle,
    Ready,
    Preparing,
    Printing,
    Paused,
    Attention,
    Error,
    Finished,
    Stopped,
    Offline,
};

[[nodiscard]] std::string_view log_label(PrinterState state);

// §7.8 estimate quality. Drives the "~" marker in §17.7, not any arithmetic.
enum class EstimateQuality {
    Unknown,
    Approximate,
    Normal,
};

[[nodiscard]] std::string_view log_label(EstimateQuality quality);

// §7.10 alert severity and source.
enum class AlertSeverity {
    Info,
    Warning,
    Error,
};

[[nodiscard]] std::string_view log_label(AlertSeverity severity);

enum class AlertSource {
    PrinterState,
    StatusPrinter,
    ResponseMetadata,
    LocalConnection,
    Firmware,
};

[[nodiscard]] std::string_view log_label(AlertSource source);

// --- Composite printer state ----------------------------------------------

// §7.6. Every value except the resolved display_name may be absent.
struct PrinterIdentity {
    // Resolved by the projection's fallback rules; always present once known.
    BoundedString<32> display_name;

    BoundedString<32> reported_name;
    BoundedString<32> configured_alias;
    BoundedString<64> hostname;

    // §7.6 lists this. Populated by a later milestone, and never logged: the
    // project treats a device identifier as redactable (§13.5, and the evidence
    // rules in CONTRIBUTING.md).
    BoundedString<32> serial;

    BoundedString<24> firmware_version;
    BoundedString<24> api_version;
    BoundedString<32> model;
};

[[nodiscard]] bool operator==(const PrinterIdentity& lhs, const PrinterIdentity& rhs);
[[nodiscard]] bool operator!=(const PrinterIdentity& lhs, const PrinterIdentity& rhs);

// §7.7.
struct PrinterOperationalState {
    PrinterState normalized{PrinterState::Unknown};

    // §24.8: unknown enum values are preserved for diagnostics rather than
    // discarded. This holds the printer's own string whether or not it mapped.
    BoundedString<24> raw;

    bool is_busy{false};
    bool requires_attention{false};

    Timestamp observed_at{};

    // Whether any trustworthy state has ever been observed. This is what
    // separates UIAD 001 §6's AcquiringState — acquiring the *first* state — from
    // a routine retry while a valid state remains representable.
    bool ever_observed{false};
};

[[nodiscard]] bool operator==(const PrinterOperationalState& lhs,
                              const PrinterOperationalState& rhs);
[[nodiscard]] bool operator!=(const PrinterOperationalState& lhs,
                              const PrinterOperationalState& rhs);

// §7.9. Chamber values remain optional and unused by the MVP.
struct TemperatureState {
    TemperatureValue nozzle_current;
    TemperatureValue nozzle_target;
    TemperatureValue bed_current;
    TemperatureValue bed_target;
    TemperatureValue chamber_current;
    TemperatureValue chamber_target;

    Timestamp observed_at{};
};

[[nodiscard]] bool operator==(const TemperatureState& lhs, const TemperatureState& rhs);
[[nodiscard]] bool operator!=(const TemperatureState& lhs, const TemperatureState& rhs);

// §7.8.
struct PrintJob {
    bool present{false};

    BoundedString<24> id;

    // §7.8: "filename extension removal is presentation-only", so the original
    // is kept intact here and any trimming happens further out.
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

    // §7.8: locally inferred, because the printer does not report a start time
    // the device can trust.
    Timestamp locally_inferred_started_at{};
    Timestamp last_updated_at{};

    // §7.8: "missing job data does not immediately erase a recently active job".
    // The observation is recorded so expiry can act on it later, instead of the
    // job vanishing the moment one response omits it.
    bool reported_missing{false};
    Timestamp reported_missing_at{};
};

[[nodiscard]] bool operator==(const PrintJob& lhs, const PrintJob& rhs);
[[nodiscard]] bool operator!=(const PrintJob& lhs, const PrintJob& rhs);

// §7.1 lastCompletedJob, without specifying a type.
//
// Deliberately smaller than PrintJob: the residual context Production UI §9
// requires for Completed and Stopped is the name, the terminal progress and the
// layer reached. Copying a whole PrintJob would double the largest structure in
// the state to carry fields nothing reads.
struct CompletedJobSnapshot {
    bool present{false};

    BoundedString<24> id;
    BoundedString<64> display_name;

    // The terminal subject state this job reached: Completed or Stopped.
    PrinterState terminal_state{PrinterState::Unknown};

    ProgressValue progress;
    LayerProgress layers;

    Timestamp completed_at{};
};

[[nodiscard]] bool operator==(const CompletedJobSnapshot& lhs, const CompletedJobSnapshot& rhs);
[[nodiscard]] bool operator!=(const CompletedJobSnapshot& lhs, const CompletedJobSnapshot& rhs);

// §7.10.
struct PrinterAlert {
    bool active{false};

    AlertSeverity severity{AlertSeverity::Info};
    AlertSource source{AlertSource::PrinterState};

    BoundedString<24> code;
    BoundedString<48> title;
    BoundedString<96> message;
    BoundedString<64> help_url;

    // §7.10: false unless the action comes from verified API data or a tested
    // project compatibility rule. §24.18 forbids inventing printer instructions.
    bool is_actionable{false};

    Timestamp observed_at{};
};

[[nodiscard]] bool operator==(const PrinterAlert& lhs, const PrinterAlert& rhs);
[[nodiscard]] bool operator!=(const PrinterAlert& lhs, const PrinterAlert& rhs);

} // namespace prd::app_model
