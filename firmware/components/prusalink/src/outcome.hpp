// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"
#include "network/http.hpp"

#include "decode.hpp"
#include "request.hpp"

// Transport result + HTTP status + decode result → the product's connection phase
// (Architecture Spec §5.8's `prusalink_error`, against §16.1's taxonomy and §7.5's
// phases).
//
// Pure, and the only place the mapping exists. §16.1 requires its categories to
// "remain distinct in state and UI", which is a property of one total function over
// the inputs rather than of scattered checks at call sites — and it is why this can
// be proved exhaustively on the host.
//
// ⛔ NOTHING HERE CAN CARRY A SECRET. The inputs are two enums and a number. The
// error label it returns is chosen from a fixed set of literals below, so no value
// from the printer, the configuration or the credential store can reach a log through
// this path.

namespace prd::prusalink {

// What one completed attempt amounted to.
struct AttemptOutcome {
    app_model::ConnectionPhase phase{app_model::ConnectionPhase::PrinterUnavailable};

    // A short fixed label for §7.5's lastError. Empty when the attempt succeeded.
    std::string_view error;

    // Whether the response was usable end to end: transport, status and decode.
    bool usable{false};
};

// Classifies a completed attempt.
//
// `decode` is only consulted when the transport and status were good, because a
// decode result is meaningless for a response that never arrived.
[[nodiscard]] AttemptOutcome classify(network::HttpOutcome transport, std::uint16_t status,
                                      DecodeError decode);

// Classifies a target that was refused before any I/O.
//
// ⛔ A REFUSAL IS NOT A FAILED REQUEST. Nothing was sent, so nothing about the printer was learned;
// what happened is that the configured destination is not one a credential may go to (§13.5) or is
// not resolvable at all. It maps to a phase the user can act on rather than to "unreachable", which
// would send them to check cables for a configuration problem.
[[nodiscard]] AttemptOutcome classify_target(TargetVerdict verdict);

// How long to wait before the next attempt.
//
// ⬅️ DELIBERATELY NOT §11.1's FULL SCHEDULE. This checkpoint reads only idle/ready,
// so the state-dependent active intervals and §11.3's 2→30 s backoff ladder have
// nothing to act on yet and are deferred to the checkpoint that introduces the job
// lifecycle. What is NOT deferred is §16.2's prohibition: unauthorized and unsupported
// API "must not use aggressive retries", so those get an infrequent probe instead of
// the normal interval. Hammering a printer that has already refused us is the failure
// mode that rule exists to prevent, and it would be visible to the owner as log spam
// and printer load.
[[nodiscard]] std::chrono::milliseconds next_delay(app_model::ConnectionPhase phase);

// Which request a polling cycle just completed. The two are paced differently, which is the whole
// reason this distinction exists as a type rather than as a bool at the call site.
enum class PollRequest : std::uint8_t {
    // `/api/version` — §11.2's "initial capability probe".
    VersionProbe,

    // `/api/v1/status` — §11.2's "primary recurring endpoint".
    Status,
};

// The pause after one polling cycle. ZERO means the next request follows immediately.
//
// ⛔ THE DEFECT THIS CLOSES — audit item 7. run_polling() waited next_delay() after EVERY cycle,
// including the successful version probe that opens the run. §11.1 gives idle/ready 5 s, so the
// first `/api/v1/status` was issued roughly five seconds after the printer had already answered
// `/api/version` — and §15.1 does not schedule it that way: step 19 probes the version, step 21
// FETCHES INFO AND STATUS, and only step 22 "enter[s] normal polling". The interval belongs after
// the initial status, not between the version and it.
//
// It also set the floor on audit item 4's window: for those five seconds the model had a new
// printer configured and no observation of it, so anything the previous printer had left behind was
// what the panel showed. Clearing that state is item 4's job; not waiting five seconds to replace it
// is this one's.
//
// ⬅️ ONLY A SUCCESSFUL PROBE EARNS THE IMMEDIATE FOLLOW-UP. A version probe that failed learned
// nothing about the printer, so it keeps §11.3's and §16.2's existing pacing — including the
// infrequent probe that "must not use aggressive retries" for unauthorized and unsupported API.
//
// PURE, so the order-of-requests decision is host-tested. The worker's loop cannot be: it is
// FreeRTOS and a socket. What that loop must still do — re-check supersession before issuing the
// follow-up — is a binding to be READ, not something this function can promise.
[[nodiscard]] std::chrono::milliseconds poll_pause(PollRequest request,
                                                   const AttemptOutcome& outcome);

} // namespace prd::prusalink
