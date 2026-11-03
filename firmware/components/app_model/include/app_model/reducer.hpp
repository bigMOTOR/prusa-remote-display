// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "app_model/app_state.hpp"
#include "app_model/events.hpp"
#include "app_model/observation.hpp"
#include "app_model/state_layers.hpp"

// The deterministic reducer — Architecture Spec §8.1, §24.11.
//
//     previous state + event → next state
//
// PURITY IS THE CONTRACT. No ESP-IDF, no FreeRTOS, no LVGL, no clock read, no
// I/O, no static mutable state, no allocation. The same inputs produce the same
// output on every call, on the host and on the target, in any order of test
// execution. Everything time-dependent arrives as an explicit timestamp on the
// event or as a ConfidenceAged event; nothing here asks what time it is.
//
// This is what makes the state machine testable without hardware (§3.4, §20.2)
// and what lets a fixture replay produce a byte-identical state.

namespace prd::app_model {

// The reduction policy. Passed in rather than compiled in so a test can state its
// own thresholds, and so tuning after Milestone 4's polling validation touches one
// declaration instead of the reducer body.
struct ReducerPolicy {
    FreshnessPolicy freshness;
};

// How long an armed factory-reset confirmation may be completed for — Milestone 3.10, owner
// ruling A. A WORKING BASELINE, not product policy.
//
// Long enough to press deliberately a second time; short enough that a confirmation opened by
// accident cannot sit waiting for a stray press hours later. Exposed so a test asserts the
// boundary against the value the reducer actually uses rather than against a copy of it.
inline constexpr Timestamp kFactoryResetConfirmationWindow = std::chrono::seconds{15};

[[nodiscard]] AppState reduce(const AppState& previous, const Event& event,
                              const ReducerPolicy& policy);

// Overload using the default policy, which is what production uses.
[[nodiscard]] AppState reduce(const AppState& previous, const Event& event);

// --- Derivations, exposed because they carry the load-bearing rules ---------
//
// These are the three mappings that UIAD 001 exists to keep separate. They are
// declared here rather than hidden in the translation unit so a test can pin each
// rule directly, without having to construct a whole state to reach it.

// Architecture Spec §7.7 printer state → UIAD 001 §6 subject state.
//
// Deliberately NOT one to one, and the gaps are the point:
//
//   Attention, Error  → the subject is UNCHANGED. These become conditions
//                       (UIAD 001 §7: "They never replace Ready, Preparing,
//                       Printing or Paused").
//   Offline           → the subject is UNCHANGED. This becomes reduced
//                       confidence, never a subject state.
//   Unknown           → the subject is unchanged once one has been resolved;
//                       AcquiringState only while none ever has been (§6:
//                       "not used for routine retries").
//   Finished          → Completed.
//
// `previous` is therefore required: three of the eleven inputs mean "keep what
// you had", and a mapping without it would silently lose the subject.
// Whether a printer snapshot may be applied, given what was last accepted.
//
// ⬅️ GENERATION FIRST, SEQUENCE SECOND, AND THE ORDER IS THE WHOLE POINT (§8.1). Within one worker
// the sequence must strictly increase, which is what makes a duplicate or a late arrival harmless.
// Across workers it must NOT: a replacement worker counts from one, and requiring it to beat the
// previous worker's watermark is what left the panel blind for the length of the previous run.
//
// ⛔ THIS DOES NOT DECIDE WHETHER THE PRODUCING WORKER IS STILL CURRENT. It cannot: the reducer is
// pure and holds no notion of a live worker, and giving it one would put a transport dependency
// inside a pure function. The application task decides, on its own task, before dispatching: it
// compares each report's generation and transaction against the session IT armed
// (app_core/src/printer_session.hpp) and translates nothing from a report that fails. So a delayed
// report from an old worker never reaches here. The division is deliberate — equality against the
// armed session upstream, change-detection for ordering here — and it is also why generation
// wraparound is harmless: nothing here compares generations for magnitude.
// A snapshot's position in the stream: which worker produced it, and where in that worker's run.
//
// The two travel together because they are only meaningful together, and because two bare uint32
// parameters side by side are trivially transposable at a call site — a swap that would silently
// invert the whole ordering rule.
struct SnapshotOrder {
    std::uint32_t generation{0};
    std::uint32_t sequence{0};
};

[[nodiscard]] bool snapshot_is_current(SnapshotOrder incoming, SnapshotOrder last);

[[nodiscard]] SubjectState subject_from_printer_state(SubjectState previous, PrinterState reported);

// UIAD 001 §7 subject condition, from the printer state and any active alert.
// Error outranks Attention; an alert can raise a condition that the state field
// alone did not.
[[nodiscard]] SubjectCondition condition_from(PrinterState reported, bool requires_attention,
                                              const PrinterAlert& alert);

// UIAD 001 §7 confidence, from observation freshness.
//
// Unavailable when nothing has ever been observed, so a fresh boot cannot present
// a default-constructed state as current (§24.17, and §7 "Unavailable means no
// previous Subject State may be presented as current").
[[nodiscard]] Confidence confidence_from(DataFreshness freshness, bool ever_observed);

} // namespace prd::app_model
