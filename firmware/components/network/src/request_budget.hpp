// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// HOW A REQUEST'S WALL-CLOCK BUDGET IS DIVIDED BETWEEN ITS PHASES.
//
// ⛔ THE DEFECT THIS CLOSES. ESP-IDF's `timeout_ms` is per-operation, so the "total request timeout"
// §11.4 asks for has to be built here. It was — for the header and body loops, which re-enter and
// re-check. But the CONNECT phase is one call: `esp_http_client_open()` was handed the same 400 ms
// slice the read loop uses and was never retried, so a printer that needed 500 ms to complete a TCP
// handshake was reported unreachable with 3.6 seconds of budget unspent. §11.4 specifies
// "approximately 2-3 s" for connect; the effective value was 400 ms.
//
// ⬅️ AND THE FIX IS AN ALLOWANCE, NOT A RETRY. In the pinned v5.5.4 the value handed to
// esp_http_client_open() reaches esp_tls's connect as both the select() bound (esp_tls.c:409-422) and
// the socket's SO_RCVTIMEO/SO_SNDTIMEO (esp_tls.c:279-290), so one call with a larger allowance is
// exactly a longer connect. Retrying instead would mean reopening a client whose transport state
// after a failed connect ESP-IDF does not define — more moving parts, for a worse guarantee.
//
// ⛔ AND THE ALLOWANCE NEVER OUTRANKS THE TOTAL. Every function here is clamped by what remains of
// the absolute deadline, so a phase budget can shorten a phase but can never let a request outlive
// its total. That is the property the header and body loops already had and the connect phase did
// not.
//
// PURE, SO IT IS PROVED WITHOUT A SOCKET. The host suite links no ESP-IDF, so this arithmetic is the
// only part of the transport that can be proved deterministically — which is precisely why the
// decision lives here rather than inline in the adapter.

namespace prd::network::budget {

// The three numbers that bound a request, grouped rather than passed as three bare uint32s.
//
// ⛔ GROUPED BECAUSE TRANSPOSING THEM IS A REAL DEFECT, NOT A STYLE POINT. Swapping the total and the
// connect allowance would give the whole request 2.5 s and the connect phase 4 s — the exact class of
// mistake this file exists to correct, made silently by argument order.
struct Allowances {
    // The whole exchange: connect through last body byte.
    std::uint32_t total_ms{4000};

    // The most any single ordinary blocking call may take before control returns to the caller.
    std::uint32_t slice_ms{400};

    // The connect phase's own allowance. §11.4's "approximately 2-3 s" applies here and nowhere else.
    std::uint32_t connect_ms{2500};
};

// Whether the absolute deadline has passed.
[[nodiscard]] bool exhausted(const Allowances& allowances, std::uint32_t elapsed_ms);

// What remains of the absolute deadline. Zero once it has passed.
[[nodiscard]] std::uint32_t remaining_ms(const Allowances& allowances, std::uint32_t elapsed_ms);

// The timeout for one ordinary blocking call: a slice, clamped to what remains.
//
// Never zero — ESP-IDF reads a non-positive timeout as "block", which is the one behaviour a
// deadline cannot tolerate. One millisecond is returned instead, and the caller's own deadline check
// is what ends the exchange.
[[nodiscard]] std::uint32_t slice_timeout_ms(const Allowances& allowances,
                                             std::uint32_t elapsed_ms);

// The timeout for the single connect call: the connect allowance, clamped to what remains.
[[nodiscard]] std::uint32_t connect_timeout_ms(const Allowances& allowances,
                                               std::uint32_t elapsed_ms);

// Whether a failed connect used up the whole allowance it was given.
//
// ⬅️ WHY THIS EXISTS. ESP-IDF returns ESP_ERR_HTTP_CONNECT for a refused connection and for one that
// ran out of time, so the error code alone cannot tell §16.1's "connection refused" from its
// "timeout". The clock can: a refusal comes back in milliseconds, and a connect that consumed its
// entire allowance timed out. Reporting the two as one would send a user to check cables for a
// printer that is simply slow to answer.
[[nodiscard]] bool connect_spent(const Allowances& allowances, std::uint32_t elapsed_ms);

} // namespace prd::network::budget
