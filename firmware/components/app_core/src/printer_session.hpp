// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/device_data.hpp"
#include "prusalink/prusalink.hpp"

#include "provisioning_flow.hpp"

// WHICH WORKER THE APPLICATION TASK IS LISTENING TO, AND WHICH CANDIDATE IT IS TESTING.
//
// ⛔ THE TWO DEFECTS THIS CLOSES, AND THEY WERE BOTH IN THE BINDING RATHER THAN THE DECISIONS.
//
// FIRST, THE VALIDATION IDENTITY WAS COMPUTED TWICE FROM TWO DIFFERENT SOURCES. The worker was
// handed the STAGED profile while the application recorded the ACTIVE one, and `next_profile_id()`
// guarantees those differ on every transaction — so every genuine verdict was discarded as
// belonging to another attempt and every setup timed out reporting an unreachable printer. The fix
// is not a better comparison; it is that the value exists exactly once. transaction_for() is the
// only way to make one, it is made from the staged pair, and both the record and the worker get
// that same object.
//
// SECOND, LIVENESS WAS DECIDED ON THE PRODUCING TASK. The worker checked its generation and then
// posted three separate application events; a stop() between the check and the posts left a
// superseded worker's connection, identity and snapshot to be applied by the reducer afterwards —
// refreshing `observed_at` and presenting a dead worker's reading as current. The fix moves the
// authority to the consumer: the worker enqueues one complete generation-stamped Report, and the
// application task compares it against the session IT armed before translating anything.
//
// ⬅️ WHY THE IDENTITY LIVES HERE AND NOT IN app_model. A worker generation is transport bookkeeping.
// §7 has no place for it, and making the reducer ask the PrusaLink component what is live would put
// a transport dependency inside a pure function. The application task already owns both sides of
// this question, so it owns the record.
//
// ⬅️ AND WHY THE CHECK IS NOT "ASK THE TRANSPORT". prusalink::start() reports the generation it
// created and this task stores it. Nothing queries a shared current-generation global — the
// component no longer exposes one — so the check is a comparison of two values this task owns,
// which is what makes it host-testable without a transport stub.

namespace prd::app_core::printer_session {

// The application task's record of one live worker.
//
// `armed` is separate from a zero generation on purpose: "nothing is running" and "something is
// running whose generation happens to be zero" must not be the same state, and kNoGeneration is
// only ever a placeholder.
struct Session {
    bool armed{false};
    prusalink::Purpose purpose{prusalink::Purpose::Poll};
    prusalink::Generation generation{prusalink::kNoGeneration};
    prusalink::Transaction transaction{};
};

// Why a report may or may not be acted on. Every rejection is a distinct fact because they are
// distinct events for a maintainer: no session at all is a lifecycle mistake, a superseded
// generation is the normal race, and a transaction mismatch is a candidate mix-up.
enum class Ingress : std::uint8_t {
    // The session armed this worker and the report is from it. Translate it.
    Accept,

    // ⛔ AN EXCLUSIVE OPERATION OWNS THE PRODUCT STATE — a confirmed factory reset. Checked FIRST,
    // before the session is even looked at, because it must reject a report that matches the armed
    // session perfectly: a queued validation SUCCESS whose activation would write storage the reset
    // has just erased. See exclusive_operation.hpp for the execution path this closes.
    ExclusiveOperation,

    // Nothing is armed for this purpose. Whatever produced the report, this task is not listening.
    NoSession,

    // The armed session is for the other purpose. A Poll report can never settle a provisioning
    // transaction and a Validate report can never colour operational state; this is the check that
    // makes that structural rather than incidental.
    PurposeMismatch,

    // A different worker. The outgoing worker's last report lands here, which is the whole point.
    GenerationSuperseded,

    // Same worker, different candidate. Unreachable while a worker's launch context is immutable,
    // and checked anyway: it is the one mistake a promotion cannot survive.
    TransactionMismatch,
};

[[nodiscard]] std::string_view log_label(Ingress verdict);

// Arms a session. The generation must come from the prusalink::StartReport of the worker this
// session is for — never from a shared global, and never from a second call.
[[nodiscard]] Session armed(prusalink::Purpose purpose, prusalink::Generation generation,
                            prusalink::Transaction transaction);

// ⛔ THE AUTHORITATIVE CHECK. Runs on the application task, against the session that task armed,
// before any part of the report becomes an observation or settles a transaction. A producer-side
// check is an optimisation that saves queue traffic; this is the one that decides.
//
// `exclusive_operation` comes from exclusive::printer_reports_permitted() and outranks identity:
// while a factory reset owns the device, a report that matches the armed session exactly is still
// refused. The barrier in app_core also disarms both sessions before the first destructive step, so
// this is the second of two independent reasons such a report cannot be acted on — and the one that
// still holds if anything ever re-arms a session mid-reset.
[[nodiscard]] Ingress classify(const Session& session, const prusalink::Report& report,
                               bool exclusive_operation);

// The transaction identity of a validation attempt — the ONE construction site.
//
// ⛔ FROM THE STAGED PAIR, NEVER FROM ACTIVE STORAGE. The candidate under test is by construction
// not the active record: §13.3's split stages both halves and activates neither until the printer
// has answered, so the active record still names the PREVIOUS profile. Reading it here is what
// produced the mismatch described at the top of this file.
//
// Returns an invalid transaction (profile 0) when nothing was staged, so a caller that skipped
// staging cannot arm a session that matches anything.
[[nodiscard]] prusalink::Transaction transaction_for(const provisioning_flow::StagedPair& staged,
                                                     std::uint32_t attempt);

// --- The report channel ----------------------------------------------------

// What happened to a report the worker tried to hand over.
enum class Publication : std::uint8_t {
    // On the queue. The application task will classify it.
    Queued,

    // There is no channel at all — the queue could not be allocated at startup. §16.4 asks for a
    // typed report rather than a silent degradation, and this is distinct from a full queue because
    // the causes and the remedies are different.
    NoChannel,

    // The channel exists and was full. §10.7: counted and reported, never silent.
    Dropped,
};

[[nodiscard]] std::string_view log_label(Publication publication);

[[nodiscard]] Publication publication_of(bool channel_present, bool accepted);

// Whether a publication lost a report. Both failure modes do; they are still reported separately.
[[nodiscard]] bool lost(Publication publication);

// Everything one validation attempt owns in RAM, so it is forgotten in ONE act.
//
// ⛔ THE FIELDS WERE FOUR SEPARATE GLOBALS AND THAT WAS THE BUG SURFACE. A new submission cleared
// the identity and the deadline but left the staged pair behind; settlement cleared three of the
// four. Supersession, settlement, timeout, retry and a new submission all now assign a default
// Validation, so "consistently" is a property of the type rather than of five call sites.
struct Validation {
    Session session{};
    provisioning_flow::StagedPair staged{};

    // 0 means no attempt is being timed. Monotonic microseconds, in the caller's clock.
    std::int64_t deadline_us{0};
};

// Whether the attempt has run out of time. False when nothing is being timed, so a disarmed
// Validation can never expire.
[[nodiscard]] bool expired(const Validation& validation, std::int64_t now_us);

// --- Which printer a polling worker was launched against -------------------

// The four fields that say WHICH PRINTER a configuration names — audit item 4, owner ruling 1 of
// 2026-08-24: an endpoint, an authentication identity, a credential generation, or the presence of a
// printer configuration at all.
//
// ⛔ WHY A POLLING SESSION MUST REMEMBER THIS, AND WHY THE REDUCER CANNOT COVER IT. The reducer
// forgets the previous printer's observations the moment the configuration changes — but the WORKER
// that produced them is still running, still polling the OLD address, and `g_poll.session` is still
// armed for its generation. Its next report therefore matches the armed session exactly, classifies
// as Accept, and repopulates everything the boundary just cleared. Clearing state that something is
// about to refill is not clearing it.
//
// printer_supervise() already stops a worker that is no longer ELIGIBLE to poll — no configuration,
// no network, provisioning open, a reset running. What it had no way to notice is a worker that is
// still eligible and pointed at the wrong printer. This is that fact, in the form the application
// task can check without reading storage on every tick: the identity is already in AppState.
//
// ⬅️ NOT THE TRANSACTION PROFILE, THOUGH THAT WAS THE OBVIOUS CANDIDATE. `Transaction::profile`
// records which stored pair a worker polls and would detect a reprovisioned printer — but the profile
// id lives in the config record, not in AppState, so consulting it would mean two NVS reads on every
// application tick. These four fields are already in the state the task holds.
struct Target {
    app_model::BoundedString<64> address;
    app_model::AuthMethod auth_method{app_model::AuthMethod::None};
    std::uint32_t credential_generation{0};

    // ⛔ PART OF THE IDENTITY, NOT A CONVENIENCE. Removing a printer configuration and adding one
    // back at the same address with the same credential is still a new target: nothing observed
    // before the removal describes what is there now.
    bool configured{false};
};

[[nodiscard]] bool operator==(const Target& lhs, const Target& rhs);
[[nodiscard]] bool operator!=(const Target& lhs, const Target& rhs);

// The identity a configuration names. The ONE place the four fields are read together, so the
// launch site and the supervision check cannot disagree about what identity means.
[[nodiscard]] Target target_of(const app_model::ConfigurationState& configuration);

// Everything one polling worker owns in RAM, so it is forgotten in ONE act.
//
// ⬅️ THE SAME SHAPE AS Validation ABOVE, FOR THE SAME REASON. Two separate globals — the session and
// the identity it was launched for — would have to be assigned together at four sites, and the
// comment on Validation records what happened the last time this project tried that: some sites
// cleared three of the four fields. Pairing them makes "consistently" a property of the type.
struct Polling {
    Session session{};
    Target target{};
};

// Whether a worker launched for `polling.target` is polling a printer this device is no longer
// configured for.
//
// False for an unarmed session — there is nothing to supersede — which is why the whole Polling
// record is the argument rather than a bare Target.
[[nodiscard]] bool target_superseded(const Polling& polling,
                                     const app_model::ConfigurationState& configuration);

} // namespace prd::app_core::printer_session
