// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "app_model/events.hpp"
#include "app_model/observation.hpp"
#include "app_model/reducer.hpp"
#include "app_store/store.hpp"

#include "decode.hpp"

// A deterministic timeline of `GET /api/v1/status` RESPONSE BODIES, for driving the product from
// the PrusaLink boundary without a printer.
//
// ⛔ WHAT THESE BODIES ARE, EXACTLY. They are SYNTHETIC, written to the shape the official contract
// documents: the Prusa-Link-Web OpenAPI specification for `/api/v1/status`, and the field set
// Prusa-Firmware-Buddy v6.8.1's status renderer emits. ⛔ THEY ARE NOT CAPTURED FROM A PRINTER.
// No real Printing response body has ever been captured by this project, and nothing here may be
// cited as evidence of what a CORE One actually sends. A real capture, redacted into a
// firmware-specific fixture, remains the final compatibility evidence; this is an instrument for
// looking at the product, not a substitute for that fixture.
//
// ⛔ AND IT IS NOT A PRUSALINK SERVER. There is no socket, no HTTP, no request routing and no
// query handling. It is a list of bodies and the times to stamp them with. The whole point is that
// everything downstream of the body is the PRODUCTION path: prusalink::decode_status(),
// prusalink::status_from(), the real event, the real store and reducer,
// the real projection and the real UI. A simulator that reimplemented any of those would be
// testing itself.
//
// WHY IT LIVES HERE AND WHY IT IS PURE. Same reason walk_time_base.hpp does: the flow that drives
// it is gated behind a Kconfig symbol and cannot be compiled by the host suite, but the timeline
// and its bodies must be. So the DATA and the stepping arithmetic live in this translation unit,
// free of ESP-IDF, of any clock and of any gate — and the host suite drives the very same bytes
// through the very same decoder that the device does, rather than through a copy of them.
//
// TIME IS PRINTER TIME, NOT BENCH TIME. `offset_ms` is the instant an observation is stamped with,
// measured from the run's base. It is deliberately not how long the bench waits between steps: the
// print these bodies describe takes 67 minutes and the review takes about 5, so the timeline
// compresses the print and the driver paces the panel — 18 seconds per state, which is a length a
// person can actually read a percentage, a remaining time and two temperature pairs in. Nothing
// here reads a clock.

namespace prd::validation {

// What the bodies below are shaped to. Logged once at the start of a run, so a capture of that run
// records what it was driven by — and records that it was not a printer.
inline constexpr std::string_view kStatusTimelineProvenance =
    "synthetic, shaped to Prusa-Link-Web OpenAPI + Prusa-Firmware-Buddy v6.8.1; NOT a capture";

struct StatusStep {
    // Milliseconds from the run's base. Strictly increasing across the timeline, because the
    // reducer clamps an event that arrives behind the last one it accepted (§8.1).
    std::int64_t offset_ms{0};

    // What this step is for, for the bench log. Empty when the index is out of range, which is the
    // only way step_at() reports that.
    std::string_view label;

    // The response body, exactly as a printer would have answered with it.
    std::string_view body;
};

[[nodiscard]] std::size_t status_timeline_step_count();

[[nodiscard]] StatusStep status_timeline_step_at(std::size_t index);

// --- Applying one body, through the production seam --------------------------

// What one body did.
struct StatusApplication {
    // The production decoder's own verdict. Reported rather than assumed: a fixture this project
    // ships could itself be malformed, and a run that silently dispatched nothing would look
    // exactly like a product that ignored the response.
    prusalink::DecodeError decoded{prusalink::DecodeError::None};

    // Whether the store accepted an event. False when the decode failed, and also when the store
    // itself refused the dispatch — its reentrancy guard. Not the reducer's staleness answer: a
    // stale reading is accepted and changes nothing, which is what `mutated` reports.
    bool dispatched{false};

    // Whether the store's revision moved. An accepted reading may legitimately change nothing.
    bool mutated{false};

    // What the response said about a job — `false` is the affirmative "no job", not silence.
    bool job_reported{false};
};

// Decodes one `GET /api/v1/status` body and applies it through THE PRODUCTION SEAM.
//
// ⛔ THE SAME SEAM app_core USES, NOT A MIRROR OF IT. It calls prusalink::decode_status() and
// prusalink::status_from(), then dispatches exactly one app_model::Event::printer_status — which is
// the whole of what app_core::apply_poll_report() does with a poll report. Nothing here assembles
// the printer and job halves, orders two dispatches, or decides acceptance; those all belong to
// status_from() and the reducer's apply_status().
//
// ⬅️ WHY IT LIVES IN THIS PURE TRANSLATION UNIT. The diagnostic flow that drives the timeline on
// the device is behind a Kconfig gate and cannot be host-compiled, and a host test that rebuilt
// this wiring itself would be testing its own copy. This is host-compiled, so the bench instrument
// and the test suite run the identical code — which is the only way "the simulator exercises the
// production path" is a fact rather than a claim.
[[nodiscard]] StatusApplication apply_status_body(app_store::Store& store, std::string_view body,
                                                  app_model::Timestamp moment,
                                                  app_model::SnapshotOrder order);

} // namespace prd::validation
