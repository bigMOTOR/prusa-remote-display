// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "app_model/events.hpp"
#include "app_model/printer_data.hpp"

#include "prusalink/transaction.hpp"

// The printer integration's public surface (Architecture Spec §5.8).
//
// One sequential request worker, started with a target and stopped again — §10.4's task,
// whose responsibilities are exactly "execute printer HTTP requests sequentially, manage
// request cancellation and timeout, apply polling schedule, post typed results to app_task".
//
// ⬅️ TWO PURPOSES, AND THE DIFFERENCE IS THE CONTRACT. §14.3 orders the configuration flow:
// join Wi-Fi, probe `/api/version`, test authentication, fetch `/api/v1/status`, and only
// then "save as active" and "transition to normal UI". §13.3 says the same thing about
// storage: write the candidate, commit it, read it back, **test the connection**, and only
// then mark it active. So a candidate the device has never spoken to must not be a source of
// operational data. Validate answers "does this configuration work"; Poll answers "what is
// the printer doing", and is only ever run against a configuration already marked active.
//
// ⬅️ WHAT IT DOES NOT EXPOSE. No backend interface, no transport selector, no capability
// registry. ADR-0010 defers Prusa Connect Local and records why nothing is abstracted for it
// in advance: the observation boundary is already the seam, and a second concrete backend is
// the evidence that would justify more structure.
//
// ⛔ READ-ONLY. Two GET paths, and no code path in this component can issue any other method
// or follow a redirect. API Research Report §19 places starting, pausing, stopping, uploading
// and temperature changes outside the MVP.

namespace prd::prusalink {

// Why the worker is running.
enum class Purpose : std::uint8_t {
    // §14.3 steps 6–8 and §13.3 step 5: one bounded pass over `/api/version` and
    // `/api/v1/status` that answers whether a candidate configuration works.
    //
    // ⛔ PUBLISHES NO OBSERVATIONS AT ALL. A validation run reports only a verdict. Nothing
    // it learns reaches the store, so a candidate cannot colour the printer state, the
    // connection state or the panel while it is still a candidate.
    Validate,

    // Ongoing operational polling. Only ever started against a configuration already marked
    // active, and the only purpose that publishes domain observations.
    Poll,
};

[[nodiscard]] const char* log_label(Purpose purpose);

// The outcome of a Validate run. One run produces exactly one of these and then the worker
// stops: a candidate that failed must not be retried in a loop against the printer.
enum class Verdict : std::uint8_t {
    // No verdict yet, or this report is not from a Validate run.
    None,

    // Both probes answered, authentication was accepted, and a required printer state
    // decoded. §13.3 step 6 may now mark the configuration active.
    Succeeded,

    // Any other outcome. `failure_phase` carries which §7.5 phase it was, so the caller can
    // tell the user something actionable without inventing a reason.
    Failed,
};

[[nodiscard]] const char* log_label(Verdict verdict);

// Where to reach the printer, and with what.
//
// ⛔ CARRIES A CREDENTIAL. `api_key` is secret material (§13.2 places it in SecretConfigV1).
// start() moves a copy into a launch context owned solely by the worker it creates; the worker
// wipes and frees that context before exiting. There is no shared slot for a credential to sit in,
// so the key's lifetime is exactly one worker's, it is never in AppState, and it is never logged.
// The report below has no field that could carry it.
struct Target {
    app_model::BoundedString<64> host;

    // 0 means unset; the request builder substitutes the default port.
    std::uint16_t port{0};

    app_model::AuthMethod auth_method{app_model::AuthMethod::None};

    app_model::BoundedString<64> api_key;

    Purpose purpose{Purpose::Validate};

    // ⬅️ WHICH CANDIDATE THIS WORKER IS FOR. Immutable for the worker's life and stamped onto every
    // report, so a verdict can only ever settle the transaction that asked for it. A late answer
    // for candidate A cannot promote, fail or settle candidate B — the audit's B5.
    Transaction transaction{};
};

// What one worker cycle established.
//
// Poll fills the three observation slots; Validate fills the verdict and nothing else. The
// invariant is asserted by a host test rather than left to a reader's care, because it is the
// mechanism by which an unvalidated candidate is kept out of the store.
//
// The payload types are `app_model`'s own observation structs, unchanged. app_core wraps them
// into events and posts them, per §4.2: "app_core coordinates services and converts service
// output into application events."
// What one status response said about a job.
//
// ⛔ THREE ANSWERS, AND THE THIRD IS WHY THIS IS NOT A BOOL. "No usable status response arrived",
// "the printer reports no job" and "here is the job" are three different facts, and only the
// middle one is a statement about the printer that the model must act on: §7.8's "missing job data
// does not immediately erase a recently active job" needs to KNOW the data is missing before it
// can start that clock. Folding Absent into None would leave a finished print's last figures
// looking current for as long as the connection held, which is precisely the failure the
// distinction exists to prevent.
enum class JobTelemetry : std::uint8_t {
    // Nothing was said. No usable status response this cycle, or this is a Validate report.
    None,

    // The response carried no usable `job` object. §4.3 makes it optional and v6.8.1 omits it
    // when no job exists.
    Absent,

    // The response carried a job. Its values may still individually be unknown.
    Present,
};

[[nodiscard]] const char* log_label(JobTelemetry telemetry);

struct Report {
    Purpose purpose{Purpose::Validate};

    // ⛔ IDENTITY, REVALIDATED BY THE APPLICATION TASK BEFORE ANYTHING IS ACTED ON. `generation` is
    // the publishing worker's lease and `transaction` is the candidate it was launched for. A check
    // inside the worker cannot be atomic with the callback that follows it, so the receiving side
    // checks too — which is what makes the remaining window harmless rather than merely narrow.
    Generation generation{kNoGeneration};
    Transaction transaction{};

    // --- Validate only ---
    Verdict verdict{Verdict::None};

    // Why a Validate run failed, in §7.5's vocabulary. Meaningless unless `verdict` is Failed.
    app_model::ConnectionPhase failure_phase{app_model::ConnectionPhase::PrinterUnavailable};

    // --- Poll only ---
    //
    // Three independent facts rather than one blob: §7.5's closing rule keeps connection state
    // and printer operational state separate, and a cycle can legitimately produce a connection
    // fact with no snapshot — which is precisely the failure case that must not leave stale data
    // looking current.
    bool connection_present{false};
    app_model::ConnectionObservation connection;

    bool identity_present{false};
    app_model::PrinterIdentity identity;

    // ⛔ ONE SLOT FOR THE WHOLE STATUS READING, NOT TWO. `status` carries the printer half and the
    // optional job half of one `GET /api/v1/status` body, and app_core turns it into ONE event —
    // `Event::printer_status` — so the store mutates once, projects once and decides acceptance
    // once.
    //
    // ⬅️ IT REPLACED `snapshot_present` + `snapshot` + a separate job slot on 2026-09-03, and the
    // separation was not merely untidy. Two events are two projections, so the panel showed a
    // composition no response described; and the terminal-completion snapshot reads the store's
    // current job, so a direct PRINTING 79% → FINISHED 100% response recorded the completion at
    // 79 %. See app_model::StatusObservation.
    //
    // ⬅️ AND IT IS ASSEMBLED IN ONE PLACE. `normalize.hpp`'s status_from() is the only thing that
    // builds one of these, and the poll worker, the diagnostic timeline and the host replay all
    // call it. A second hand-written assembly is how the three would come to disagree.
    bool status_present{false};
    app_model::StatusObservation status;
};

// Called from the worker task, once per cycle for Poll and once in total for Validate. Must
// not block and must not dispatch into the store — §10.2 keeps the application task as the
// store's only consumer, so an implementation posts to a queue instead.
using ReportSink = void (*)(const Report& report);

// What a start attempt produced.
//
// ⬅️ THE GENERATION IS RETURNED RATHER THAN PUBLISHED, AND THAT IS THE POINT. The caller needs to
// know which lease it just created so it can record the session it will judge reports against.
// Handing it back makes that a value the application task owns; exposing a shared
// "current generation" instead is what let the previous design put the liveness check on the
// producing task, where it could not be atomic with the publication that followed it.
struct StartReport {
    bool started{false};

    // The lease the new worker holds. kNoGeneration when nothing started.
    Generation generation{kNoGeneration};
};

// Starts the worker. `started` is false when a worker is already alive — including one that has
// been asked to stop and has not yet noticed — when the target is unusable (no host, an
// unsupported authentication method, a missing key), or when the task could not be created.
//
// A refusal is not an error the caller must handle specially: the supervisor tries again on
// its next tick, which costs nothing and blocks nobody.
[[nodiscard]] StartReport start(const Target& target, ReportSink sink);

// Asks the worker to stop, and supersedes its lease immediately.
//
// ⛔ DOES NOT WAIT. Returns at once, so the application task never blocks for a socket
// timeout. Superseding the lease is what makes that safe: from this call onward the outgoing
// worker cannot publish anything, whatever it is in the middle of. Safe to call when nothing
// is running, and idempotent.
void stop();

// Whether a worker is alive. True for a worker that has been asked to stop and has not yet
// finished its current request.
[[nodiscard]] bool running();

// ⛔ THERE IS DELIBERATELY NO current_generation(). It existed, and exposing it was the mistake: the
// liveness check was performed by asking this component from whichever task happened to be
// publishing, which cannot be atomic with the publication. The generation now travels one way only
// — out of start(), into the application task's session record, and back in on every Report — so the
// check is a comparison of two values the deciding task owns. Nothing to query means nothing to
// query at the wrong moment.

} // namespace prd::prusalink
