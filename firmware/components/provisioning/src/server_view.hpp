// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string>
#include <string_view>

#include "app_model/bounded_string.hpp"

#include "provisioning/server.hpp"
#include "request_validation.hpp"
#include "surface.hpp"

// WHAT ONE REQUEST SEES — Architecture Spec §10.5 and §10.7.
//
// INTERNAL HEADER of the provisioning component. Pure — no ESP-IDF, no socket, no queue — so the
// status body, the printer-stage admission and the coherence rule below are host-tested against the
// same code the firmware runs.
//
// ⛔ WHAT THIS CLOSES, AND IT WAS THREE INDEPENDENT READS RATHER THAN ONE. Until 2026-08-24 the HTTP
// task read the server's published state directly out of three file-scope objects that the
// application task wrote whenever provisioning moved:
//
//   - `Surfaces g_surfaces`, whose BoundedString members are replaced field by field, so a request
//     arriving during a station-address change could read a new length against old bytes and
//     mis-classify which of the device's own surfaces it came in on;
//   - `volatile SetupState g_setup_state`, used as though `volatile` were synchronisation;
//   - `std::string g_setup_reason` — a HEAP OBJECT written by one task and read by another, the most
//     reachable of the three, because publish() fires on every provisioning state change while the
//     browser is polling /api/status.
//
// CONFIG_FREERTOS_UNICORE is not set, so those two tasks genuinely run at the same instant on
// different cores. Worse than the undefined behaviour, though, was the TEARING BETWEEN the objects:
// handle_submit() read the surfaces for its arrival check, read them again inside the gate, and then
// read the setup state for stage admission — three reads of three independently updated values, so
// one request could be answered half from before a transition and half from after it.
//
// The repair is one immutable snapshot per request. The application task owns a master copy and
// pushes it whole through a depth-one overwrite queue; a handler takes ONE copy at entry and answers
// entirely out of it. So a response is the state as it was before the transition or as it is after —
// never a mixture — and no synchronisation primitive is held while an HTTP response is sent.
//
// ⬅️ WHY A SNAPSHOT AND NOT A MUTEX. §10.7 asks for bounded queues and "no shared mutable domain
// state across tasks"; a lock would have kept the sharing, and the obvious place to take it is
// around a handler, which is the one place a primitive must never be held. app_core already hands
// this component a submission the same way — one depth-one overwrite queue, copied under the queue's
// own critical section — so this is that pattern in the other direction rather than a new mechanism.

namespace prd::provisioning {

// Everything the HTTP task may know about the flow, as one value.
//
// TRIVIALLY COPYABLE, because it crosses a FreeRTOS queue by value: BoundedStrings and scalars, no
// pointer, nothing owned, no heap. That is also what retires the std::string the setup reason used
// to be — a heap object shared across tasks cannot be made safe by being small.
struct ServerSnapshot {
    Surfaces surfaces{};
    SetupState setup_state{SetupState::Editing};

    // Owner ruling D-3's reason text. The longest label provisioning_flow can produce is
    // "printer-unusable-response" at 25 bytes; 32 is headroom, not a target.
    app_model::BoundedString<32> setup_reason;
    app_model::BoundedString<32> update_version;
    bool update_installing{false};
};

// Whether a printer-stage submission may be accepted.
//
// ⛔ DEFENCE IN DEPTH, NOT THE DEFENCE. app_core re-checks this on the application task and is
// authoritative — §10.5 forbids this component from mutating application state, and a decision taken
// here can only ever be advice. What it buys is a proper HTTP answer instead of a silent no-op, and
// it stops a caller reaching step two by skipping step one.
//
// It is a function of the snapshot the request was answered from, so the admission and the response
// body cannot disagree about which state they saw.
[[nodiscard]] bool admits_printer_stage(Stage stage, SetupState state);

// The CSRF token minted for this page.
//
// ⛔ A DISTINCT TYPE BECAUSE THE MISTAKE IS SILENT AND SECURITY-RELEVANT. The version and the token
// are both string views, and only their order at the call site tells them apart — swapping them
// would publish the session token in the `version` field of a response anybody on the segment can
// fetch, and put the firmware version where the page looks for its token. Neither the compiler nor a
// reviewer reliably catches that. Same remedy escaping.hpp's FieldName and backoff.hpp's Jitter were
// introduced for, and clang-tidy's bugprone-easily-swappable-parameters is what asked for it here.
struct CsrfToken {
    std::string_view text;
};

// The GET /api/status body, built from ONE snapshot.
//
// ⛔ PRESENCE ONLY, NEVER A VALUE. §5.11: the web UI "must never return stored secrets to the
// browser". No stored SSID, Wi-Fi password, API key or printer password appears here, and neither
// does the setup access point's own password — its holder already knows it, and a response would put
// it in browser history and cache.
//
// The CSRF token IS returned, and is not a secret in that sense: it is minted for this page, and no
// other origin can read this response, because no CORS header is ever sent.
//
// ⬅️ BUT ONLY ON THE SURFACE THAT CAN USE IT — Milestone 3.9. On the persistent LAN surface no
// mutation is permitted at all (owner ruling A), so the token there would authorise nothing.
// Withholding it costs the page nothing it can act on and removes one value from a response that
// anybody on the segment can now fetch. This is defence in depth rather than the defence itself: the
// surface refusal in the gate is what actually stops the mutation.
//
// ⬅️ AND THE SNAPSHOT IS A PARAMETER, WHICH IS THE COHERENCE GUARANTEE ITSELF. There is no global
// for this function to reach, so every field it writes necessarily describes the same instant. The
// version and the token are the caller's because neither is part of the published flow state: the
// build is fixed for the life of the image, and the token belongs to the HTTP task alone.
[[nodiscard]] std::string status_body(const ServerSnapshot& snapshot, Surface arrival,
                                      std::string_view version, CsrfToken token);

// --- What POST /api/setup is allowed to answer -----------------------------
//
// ⛔ WHY THIS IS A FUNCTION AND NOT THE ORDER OF TWO STATEMENTS. The handler used to compose its
// response from the grammar alone and call the sink afterwards, so `"accepted": true` was decided
// before anything had been handed to app_core and no later failure could unsay it. Putting the rule
// here makes it a tested property instead of a property of which line comes first — and the one
// thing this checkpoint must never allow to regress is a response that claims a validation has begun
// when nothing received the submission.
enum class SubmitAnswer : std::uint8_t {
    // The body did not validate. The per-field report is the answer, and no handoff was attempted.
    Report,

    // The body validated AND app_core took it. The per-field report is the answer, and it may say
    // accepted.
    Accepted,

    // The body validated and could NOT be handed over. 503, a fixed body, and no field report —
    // there is nothing for the user to correct.
    Unavailable,
};

// `handoff` is only meaningful when `validated` is true; a caller that did not validate must not
// have called the sink at all, and passing any value here cannot make this return Accepted.
[[nodiscard]] SubmitAnswer submit_answer(bool validated, SubmissionHandoff handoff);

// The complete body for SubmitAnswer::Unavailable.
//
// FIXED AT COMPILE TIME, WHICH IS THE POINT. Nothing a caller submitted can reach it, so there is no
// echo to review and no path for §13.5's forbidden values to appear in a 503.
inline constexpr std::string_view kUnavailableBody = R"({"accepted":false,"error":"unavailable"})";

} // namespace prd::provisioning
