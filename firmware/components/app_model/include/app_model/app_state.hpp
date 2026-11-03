// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

#include "app_model/device_data.hpp"
#include "app_model/observation.hpp"
#include "app_model/printer_data.hpp"
#include "app_model/state_layers.hpp"

// The single authoritative application state — Architecture Spec §7.1, §24.11.
//
// Layout follows the accepted separation rather than convenience. Six groups, and
// no field belongs to two of them:
//
//   device data      what the companion is (boot, configuration, network,
//                    display, OTA)
//   printer data     what the printer reported, normalized (§7.6 to §7.10)
//   subject state    UIAD 001 §6, one value, the semantic state of the printer
//   conditions       UIAD 001 §7 subject condition and §8 access condition
//   confidence       UIAD 001 §7, whether the subject state may be shown as current
//   device-owned     UIAD 001 §5 provisioning, §9 modal surface, §10 operations
//
// Subject state, condition and confidence are stored rather than recomputed on
// read: they are state in UIAD 001 §13's canonical hierarchy, and deriving them
// during projection would mean the projection needed the previous state to know
// whether Attention had replaced something. The reducer derives them from printer
// data once, when the observation arrives.
//
// Which of these layers is *presented* is a separate question, answered by
// presentation.hpp using §12's priority order. Nothing here is a screen.
//
// No secret is present, and none may be added: §7.3, §13.5 and §24.3 permit only
// has-credential flags and auth metadata in the state.

namespace prd::app_model {

// The structural origin the acknowledgement's motion starts from: WHICH POSITION ON UIDD 004 §2.2's
// ENERGY AXIS the operational form occupied at the moment observation was lost.
//
// ⛔ ABSTRACT ON PURPOSE — IT IS NOT CACHED SUBJECT DATA. It carries no progress, no temperature, no
// layer, no job name, no printer identity and no reading of any kind, and it cannot be used to
// redraw any of them. It says only which shape was on the panel.
//
// ⛔ SIX VALUES, BECAUSE THE AXIS HAS SIX POSITIONS AND EACH IS A DIFFERENT SHAPE. Two buckets
// ("resting" and "carrying") was a confirmed defect: it made Preparing and Paused jump instantly to
// Printing's closed maximum-energy cycle, and Completed and Stopped jump instantly to Ready's
// horizon, at the very instant of loss. Candidate D's accepted contract is that the form already on
// the panel leaves as ONE CONTINUOUS BODY, so there must be no endpoint jump at entry.
//
// ⛔ AND IT IS NOT A UI `Energy`. The magnitudes belong to the ui component and stay there — UIDD 004
// §13.1, "the value is not part of AppState". This names the position; scenes/unavailable_motion.cpp
// asks the production energy law what shape that position is.
enum class LossOriginForm : std::uint8_t {
    Ready,
    Stopped,
    Preparing,
    Completed,
    Paused,
    Printing,

    // ⬅️ NOT A POSITION ON THE AXIS, AND NOT A PRINTER STATE. AcquiringState has no operational
    // form: Production UI §9 routes it to its own Connecting screen, so nothing was on the panel to
    // carry out of. It is named explicitly rather than folded into `Ready` because those two are
    // different facts and the presentation layer now treats them differently — the accepted B2
    // entry belongs to the REAL Ready origin, and no owner decision extends it to a loss that
    // carried no form at all.
    //
    // ⛔ FOLDING IT INTO Ready WAS A CONFIRMED SCOPE LEAK. origin_form_of() fell through to Ready,
    // so B2's lift ran for AcquiringState too, and a comment asserting that was intentional was
    // written without an owner ruling. This value is what makes the two paths separable.
    NoOperationalForm,
};

// One episode of lost observation, and who owns its acknowledgement.
//
// ⛔ THIS EXISTS BECAUSE `Unavailable && DisplayPower::On` IS NOT AN EPISODE, AND TREATING IT AS
// ONE WAS A CONFIRMED DEFECT. Under that rule the acknowledgement reappeared whenever the panel
// happened to be lit: an unrelated activity wake replayed it, and a higher-priority surface
// clearing later revealed it as though the loss had just happened. Neither is one meaningful
// change acknowledged once (UI Behaviour Decision 001 §5.1).
//
// So ownership is state, decided by the reducer, and never re-derived from brightness, from a timer
// or from whatever scene is visible. The UI renders the phase this struct describes; it decides
// nothing.
struct LossEpisode {
    // An eligible loss of observation is open. Closed by recovery, never by display power.
    bool active{false};

    // ⛔ THE AUTOMATIC ENTRY ACKNOWLEDGEMENT IS SPENT — used OR forfeited. Once true this episode
    // never presents again on its own, however many polls, wakes or recessions follow. That is
    // "exactly one per loss episode".
    //
    // ⬅️ AND IT DELIBERATELY DOES NOT BLOCK AN EXPLICIT REQUEST. Automatic entry and a person
    // pressing the button are different causes, and the accepted contract gives the press its own
    // permission: "a short press from Off while still Unavailable presents the same
    // acknowledgement". A single permanent `suppressed` flag conflated the two and silently
    // withdrew the press's permission for the rest of the episode.
    bool automatic_spent{false};

    // Whether the acknowledgement is on the panel right now.
    bool presenting{false};

    // Which structural origin this episode's motion starts from. Captured once, at entry.
    LossOriginForm origin{LossOriginForm::Ready};
};

[[nodiscard]] bool operator==(const LossEpisode& lhs, const LossEpisode& rhs);
[[nodiscard]] bool operator!=(const LossEpisode& lhs, const LossEpisode& rhs);

// The next identity for an acknowledgement presentation.
//
// ⛔ THE WRAP SKIPS ZERO, DELIBERATELY. Zero is the reserved "nothing is acknowledged" value: the
// projection publishes it when no acknowledgement is on the panel, and a render task therefore
// treats a zero as "hide", not as an identity to compare against. Letting the counter wrap onto zero
// would, once in 2^32 presentations, publish a live acknowledgement that a consumer reads as none.
//
// Constexpr and free so the wrap is a host-testable fact rather than a claim in a comment — a test
// cannot reach 2^32 presentations through the reducer, but it can call this with 0xFFFFFFFF.
[[nodiscard]] constexpr std::uint32_t next_presentation_generation(const std::uint32_t current) {
    const std::uint32_t next = current + 1U;
    return next == 0U ? 1U : next;
}

struct AppState {
    // §7.1: increases after every accepted state mutation. Maintained by the
    // reducer; a consumer compares revisions to know whether to re-project.
    std::uint32_t revision{0};

    // When the last accepted mutation happened, and the timestamp of the last
    // event seen at all. The second exists so time can be clamped forward: an
    // event whose timestamp moves backwards is counted as out-of-order rather
    // than allowed to rewind the ageing calculation.
    Timestamp last_change_at{};
    Timestamp last_event_at{};

    // §8.1's request-generation bookkeeping: the highest sequence accepted for
    // each observation stream. An observation whose sequence is not greater is a
    // stale or duplicated response and is rejected.
    // The generation whose snapshots the watermark below belongs to. A snapshot from a different
    // generation is a different worker and starts its own ordering.
    std::uint32_t last_printer_generation{0};

    std::uint32_t last_printer_sequence{0};
    std::uint32_t last_job_sequence{0};

    // --- Device data -------------------------------------------------------

    BootStatus boot;
    ConfigurationState configuration;
    NetworkState network;
    DisplayState display;
    OtaState ota;

    // --- Device-owned flows (UIAD 001 §5, §9, §10) -------------------------

    ProvisioningPhase provisioning{ProvisioningPhase::Inactive};
    ModalSurface modal_surface{ModalSurface::None};

    // The facts. device_operation below is derived from these three by the
    // reducer, the same way subject state is derived from printer state — so the
    // resolution order in UIAD 001 §10 lives in one function instead of being
    // re-decided by every event that touches an operation.
    DeviceFaultReason device_fault{DeviceFaultReason::None};
    bool recovery_mode{false};
    bool factory_resetting{false};

    // ⛔ A CONFIRMED RESET EXECUTED AND DID NOT FINISH — audit item 5, 2026-08-28. TERMINAL: nothing
    // clears it but an actual boot, and it is what stops the executor being asked to erase again.
    //
    // ⬅️ WHY IT IS NOT DERIVED FROM device_fault. The halt used to read `device_fault ==
    // FactoryResetIncomplete`, and that field is a single mutable slot five reducer paths assign. A
    // NetworkObserved already sitting on the application queue when the reset failed reached the
    // next drain, replaced the reason, and the very next tick answered Step::Erase — the destructive
    // executor, re-entered. Measured against the real reducer, not supposed.
    //
    // ⛔ SO THE EXECUTION FACT AND THE PRESENTATION REASON ARE NOW SEPARATE THINGS, deliberately.
    // fault_holds_device() keeps the panel truthful; this keeps the executor stopped. Neither is
    // derived from the other, because they answer different questions and one of them is
    // destructive.
    bool factory_reset_failed{false};

    // When the factory-reset confirmation was armed — Milestone 3.10, owner ruling A.
    //
    // ⛔ THIS IS A SECURITY PRECONDITION, NOT BOOKKEEPING. A reset is confirmed by a SECOND
    // very-long press, and only within kFactoryResetConfirmationWindow of the first. Storing
    // the arming moment is what makes "within the window" a fact the reducer can check
    // rather than a timer somebody has to remember to cancel: a stale confirmation cannot be
    // completed even if it is still on screen, because the check happens at the moment of
    // the second press. Zero whenever no confirmation is armed.
    Timestamp factory_reset_armed_at{};

    DeviceOperation device_operation{DeviceOperation::None};

    // --- Printer data (§7.5 to §7.10) --------------------------------------
    //
    // printer_connection is kept separate from printer on purpose. §24.5:
    // connection state must never be inferred from printer operational state.

    PrinterConnectionState printer_connection;
    PrinterIdentity printer_identity;
    PrinterOperationalState printer;
    TemperatureState temperatures;
    PrintJob current_job;
    CompletedJobSnapshot last_completed_job;
    PrinterAlert alert;

    // --- Subject, conditions, confidence (UIAD 001 §6 to §8) ---------------

    SubjectState subject{SubjectState::AcquiringState};
    SubjectCondition subject_condition{SubjectCondition::None};
    Confidence confidence{Confidence::Unavailable};
    AccessCondition access_condition{AccessCondition::None};

    // --- The lost-observation episode (Production UI §9, UIAD 001 §11.1) ----

    LossEpisode loss_episode;

    // Increments once every time an acknowledgement presentation STARTS, and is NEVER reset.
    //
    // ⛔ OUTSIDE LossEpisode BECAUSE THE EPISODE IS RESET, AND THAT WAS A DEFECT. Recovery clears
    // the episode wholesale, so a counter living inside it returned to zero and the next episode's
    // first presentation published generation 1 again. If the UI queue coalesced the intervening
    // recovery presentation, the render task saw the same generation twice and never restarted the
    // motion — a second genuine loss would have drawn nothing.
    //
    // ⛔ A GENERATION, NOT A TIMESTAMP. An absolute moment in the projection makes the projection
    // time-dependent; the fixture suite's rebase test proves it. Unsigned wrap-around is defined
    // behaviour and is the intended overflow semantics: after 2^32 presentations the counter wraps,
    // and consumers compare for INEQUALITY rather than ordering, so a wrap is just another change.
    //
    // Zero means "no presentation has ever begun", and the projection publishes zero for "no
    // acknowledgement is on the panel" — so next_presentation_generation() below skips zero on the
    // wrap and a live generation is never zero.
    std::uint32_t loss_presentation_generation{0};

    // --- Diagnostics (§7.1, §19.2) -----------------------------------------

    DiagnosticsSummary diagnostics;
};

// Whether two states are the same *product* state.
//
// Excluded from the comparison: revision, last_change_at, last_event_at, the two
// observation sequences and the diagnostics counters. The timestamps and revision
// are bookkeeping about the change itself, so including them would make every
// state trivially unequal to its predecessor. The sequences and the counters are
// excluded so that a rejected duplicate observation — or a fresh sequence
// carrying identical data — stays
// idempotent: it is worth recording that a duplicate arrived, but the product is
// showing exactly what it was showing before, and publishing a new revision for
// it would wake every observer for nothing (§10.7's coalescing intent, and UI
// Behaviour Decision 001 §4 — "Routine polling, retries and small telemetry
// changes shall not become visible").
[[nodiscard]] bool content_equals(const AppState& lhs, const AppState& rhs);

// Full equality, bookkeeping included. Used by the replay tests, where two
// independently reduced sequences must agree down to the revision counter.
[[nodiscard]] bool operator==(const AppState& lhs, const AppState& rhs);
[[nodiscard]] bool operator!=(const AppState& lhs, const AppState& rhs);

// The state is copied whenever a snapshot is taken, so its size is a budget item
// (§21) rather than an accident. This bound exists to make growth a deliberate
// decision: adding a field that breaks it should prompt a look at whether the field
// belongs in the state at all, not a reflex increase of the number.
//
// Measured at 2000 bytes, so there is roughly 20 % headroom. Most of it is bounded
// strings — the printer identity, the job's three filename fields and the alert text
// account for over half — and trimming a capacity is the cheaper answer than raising
// this. The bound holds identically on the host and on the target: every member is a
// fixed-size value type, with no pointer and no size_t-shaped field.
static_assert(sizeof(AppState) <= 2560, "AppState is copied per snapshot; keep it bounded");

} // namespace prd::app_model
