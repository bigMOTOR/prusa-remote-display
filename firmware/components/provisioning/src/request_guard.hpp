// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "surface.hpp"

// The §14.4 gate every mutating request passes through — owner ruling D9, extended by
// Milestone 3.9's owner rulings A and B.
//
// PURE, AND HOST-TESTED. It takes what the adapter read out of the request — the surface it
// arrived on, the Origin header, the Content-Type, the declared body length, the offered
// token — and the clock, and answers whether the request may proceed. No socket, no ESP-IDF
// type, no global.
//
// THAT SPLIT IS THE POINT. A security check written inside a handler is exercised only by
// pointing a browser at a board, which means the interesting cases — a missing Origin, a
// token of the wrong length, a mutation offered over the LAN — are the ones nobody runs.
// Here they are a table.
//
// ⛔ EVERY REQUEST THIS GATE SEES IS A MUTATING REQUEST. There is no `mutating` argument
// because there is no caller for which the answer would be "no": the gate exists for
// POST /api/setup, and owner ruling A's surface rule therefore applies unconditionally
// inside it. A future read-only endpoint does not pass through here at all.

namespace prd::provisioning {

class SessionToken;

// Why a request was refused, one value per rule, so the response can name the rule and a
// test can assert which one fired.
enum class GateOutcome : std::uint8_t {
    Allow,

    // ⬅️ OWNER RULING A, MILESTONE 3.9. The request arrived on the persistent station/LAN
    // surface, which is read-only until M3.10 implements R7's short-lived physical-presence
    // authorization. Checked FIRST: it is the cheapest fact, it costs no rate-limit budget,
    // and a request refused here never reaches the token comparison.
    SurfaceNotPermitted,

    // §14.4 CSRF. Origin absent, or not one of this device's own active origins.
    OriginMissing,
    OriginMismatch,

    // D9: JSON only. A cross-origin form cannot send this content type without a preflight,
    // and no preflight is answered — so the auto-submitting form never reaches a handler.
    ContentTypeUnsupported,

    // §14.4 "limit request sizes". Refused on the declared length, before a byte is read.
    BodyTooLarge,
    BodyMissing,

    TokenMissing,
    TokenMismatch,

    // §14.4 rate limiting.
    TooManyRequests,
};

[[nodiscard]] std::string_view log_label(GateOutcome outcome);

// The HTTP status each outcome answers with. Kept beside the outcome so a handler cannot
// invent one, and so the mapping is assertable.
[[nodiscard]] int status_for(GateOutcome outcome);

// --- Limits -----------------------------------------------------------------
//
// The largest legitimate submission is an SSID (32) plus a passphrase (63) plus a host
// (64) plus a port (5) plus an API key (64) — 228 bytes of value, plus JSON syntax and
// field names, comfortably under 512. 1024 is that with room to spare and still far below
// anything that would pressure the heap.
inline constexpr std::size_t kMaxBodyBytes = 1024;

inline constexpr std::string_view kJsonContentType = "application/json";

// --- Rate limiting (§14.4) --------------------------------------------------
//
// §14.4's own target is "rate-limit authentication tests", and Milestone 3.7 has no
// authentication and no printer connection test — both arrive later. What can be bounded
// now is how fast a joined client may submit, which caps the work one client can cause.
//
// WORKING BASELINE, NOT PRODUCT POLICY: a token bucket of 5, refilled one per second. A
// person correcting a typo is never throttled; a script is.
inline constexpr std::uint32_t kRateBurst = 5;
inline constexpr std::uint32_t kRateRefillIntervalMs = 1000;

class RateLimiter {
  public:
    // `now_ms` is a monotonic millisecond count. Supplied by the caller so the policy is
    // testable without waiting.
    [[nodiscard]] bool allow(std::uint32_t now_ms);

    void reset();

    [[nodiscard]] std::uint32_t available() const {
        return tokens_;
    }

  private:
    std::uint32_t tokens_{kRateBurst};
    std::uint32_t last_refill_ms_{0};
    bool started_{false};
};

// --- The gate ---------------------------------------------------------------

// What the adapter read off the request. Views into the request's own buffers.
struct RequestFacts {
    // Which of the device's surfaces accepted the connection. READ FROM THE SOCKET, NEVER
    // FROM A HEADER: a Host or Origin header is written by the caller, so authorizing on one
    // would let anybody on the LAN claim to have arrived on the setup access point. The
    // adapter resolves this from the accepted connection's own local address.
    Surface arrival{Surface::Unknown};

    std::string_view origin;
    std::string_view content_type;
    std::string_view token;
    std::size_t content_length{0};
};

// `surfaces` is the set of addresses the device currently answers on (owner ruling B).
//
// Checked in this order on purpose: the arrival surface first, because it is the cheapest
// fact and the one owner ruling A turns on; then the cheap header rules; then the body limit
// before anything is read; the token next to last; and the rate limit last, so a refused
// request spends none of it. A caller that fails an earlier rule never reaches the
// constant-time comparison, which is one fewer way to measure it.
[[nodiscard]] GateOutcome evaluate(const RequestFacts& facts, const Surfaces& surfaces,
                                   const SessionToken& token, RateLimiter& limiter,
                                   std::uint32_t now_ms);

} // namespace prd::provisioning
