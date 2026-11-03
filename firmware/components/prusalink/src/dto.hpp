// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/bounded_string.hpp"

// The raw shape of the two PrusaLink responses this checkpoint reads, one struct per
// endpoint (Architecture Spec §5.8's `prusalink_dto`).
//
// §9.2: "DTOs mirror the external contract and may be awkward. Domain models must be
// clean and product-oriented." So these carry the vendor's own field names and the
// vendor's own optionality, and nothing here is a domain type: the normalizer is the
// only thing that turns one of these into an observation.
//
// EVERY OPTIONAL VALUE HAS AN EXPLICIT `_known` COMPANION. §9.3 requires absent and
// zero to stay distinguishable, and a bed at 0 °C is a real reading while an absent
// bed temperature is not a reading at all. A sentinel would conflate them.

namespace prd::prusalink {

// `GET /api/v1/status` (API Research Report §4.3).
//
// Only the `printer` object is required by the published specification, and within it
// only `state` (§5.1). Everything else is optional and may be absent on any firmware.
// ⛔ THE PAIRED FIELD ORDER COSTS 40 BYTES OF PADDING, AND IT IS KEPT ON PURPOSE.
// clang-analyzer-optin.performance.Padding is right about the arithmetic: grouping the seven
// doubles, then the two strings, then the nine flags takes this struct from 168 bytes to 128.
// Measured, not estimated. What it costs is the property the comment above this struct exists to
// state — that every optional value has its `_known` companion beside it — and that property is
// why absent and zero have never yet been confused in this decoder.
//
// The 40 bytes are 0.49 % of the prusalink worker's 8192-byte stack, which is the only place a
// StatusDto ever lives: one short-lived local inside one poll cycle, on a stack that already
// carries kMaxBody's 2048-byte response buffer. There is no array of these and none is stored.
// So the trade is a readability property that has prevented a real class of defect against half a
// percent of one task's stack, and it is not close.
//
// ⬅️ REVISIT IF EITHER SIDE CHANGES: if a StatusDto is ever kept, queued or arrayed, or if the
// worker's stack headroom becomes tight, group the fields and delete this note. Suppressed here
// rather than disabled in .clang-tidy, per Bootstrap Spec §12.3 rule 4, so new code elsewhere is
// still held to the check.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct StatusDto {
    // The printer's own state string, kept verbatim. §5.1: the raw value must be
    // preserved for diagnostics whether or not it maps to a known state.
    app_model::BoundedString<24> printer_state;

    bool temp_nozzle_known{false};
    double temp_nozzle{0.0};

    bool target_nozzle_known{false};
    double target_nozzle{0.0};

    bool temp_bed_known{false};
    double temp_bed{0.0};

    bool target_bed_known{false};
    double target_bed{0.0};

    // --- The optional `job` object (API Research Report §4.3, §6.1) --------------
    //
    // ⛔ WHETHER THE OBJECT WAS THERE IS ITS OWN FACT, SEPARATE FROM EVERY FIELD IN IT. §4.3 makes
    // `job` optional and Buddy omits it entirely when no job exists, so "the printer told us there
    // is no job" and "the printer sent a job whose values we could not use" are different
    // statements about the machine. §9.3's absent-versus-zero rule applied one level up: collapsing
    // them would let a finished print keep its last figures alive because the object that stopped
    // being sent looked the same as one that arrived empty.
    bool job_present{false};

    // ⬅️ THE IDENTITY ONLY, AND DELIBERATELY NOT THE NAME OR THE FILE. `apply_job()` treats a
    // changed id as a different job and carries nothing over, which is the whole reason this
    // checkpoint reads it. Display name, original file name and source path belong to
    // `GET /api/v1/job`, which is a separately bounded endpoint and is not read here.
    //
    // Kept as text because that is what the domain's `JobObservation::id` is. The OpenAPI contract
    // types it as an integer and v6.8.1 emits a number; a string spelling is accepted too, because
    // §3.2 tolerance costs nothing here and a wrong guess about the JSON type would silently lose
    // the identity that job-change detection depends on.
    bool job_id_known{false};
    app_model::BoundedString<24> job_id;

    // ⛔ PERCENT, 0...100 — NOT THE LEGACY FRACTION. §6.1 is explicit that `/api/v1/status.job`
    // reports percent and gives 42.0 as its example, while legacy `/api/job.progress.completion`
    // has used 0.88 for the same idea on Buddy firmware, and that "the v1 and legacy models must
    // not share an unqualified progress decoder". This field is the v1 one. Nothing here or
    // downstream may reinterpret 0.88 as 88 %.
    bool progress_known{false};
    double progress_percent{0.0};

    // Seconds. v6.8.1 emits `time_remaining` only when it has a valid estimate, so absence here is
    // the ordinary case rather than a fault.
    bool time_remaining_known{false};
    double time_remaining_seconds{0.0};

    // Seconds spent printing so far. The domain calls it elapsed.
    bool time_printing_known{false};
    double time_printing_seconds{0.0};

    // ⛔ NO LAYER FIELDS, AND THAT IS NOT AN OMISSION. The status endpoint exposes no current or
    // total layer. `axis_z` is a head position in millimetres and dividing it by a guessed layer
    // height would be an invented value, which §24.18 forbids and which no accepted Source asks
    // for. Layer information stays unknown until an endpoint that actually reports it is read.
};

// `GET /api/version` (API Research Report §4.1).
//
// ⬅️ ALL FIVE REQUIRED FIELDS, added 2026-08-23. §4.1's required set is api, version, printer, text
// and firmware, and requiring only two of them meant `{"api":..,"firmware":..}` from any unrelated
// service passed validation and was promoted to an active printer configuration. Requiring the full
// set is what makes the preflight an identification and not just a liveness check.
struct VersionDto {
    app_model::BoundedString<24> api;

    // The PrusaLink version.
    app_model::BoundedString<24> version;

    // The printer or platform version.
    app_model::BoundedString<24> printer;

    // The human-readable product string. Used only as an identity hint, never as a compatibility
    // gate — §4.1: "do not hard-code compatibility solely from the firmware version string."
    app_model::BoundedString<32> text;

    app_model::BoundedString<24> firmware;
};

} // namespace prd::prusalink
