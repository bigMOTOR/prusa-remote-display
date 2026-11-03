// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Whether this firmware's HTTP client may follow a redirect. It may not.
//
// ⛔ THE HAZARD THIS CLOSES. A PrusaLink request carries the printer's API key in an `X-Api-Key`
// header (API Research Report §10.2). If the client followed a 3xx, it would re-send that header to
// whatever authority the `Location` header named — a host the owner never configured, chosen by
// whatever answered on the printer's address. §13.5 forbids sending credentials outside the LAN and
// §13.6's threat model assumes an unencrypted local network, so a redirect is the one response that
// can turn a local request into an exfiltration. No Accepted Project Source requires following one:
// §14.3's flow probes two fixed paths on a configured host, and §19's endpoint set names no
// redirecting endpoint.
//
// ⬅️ WHAT ACTUALLY CONTAINS IT — THREE BARRIERS, AND A FOURTH THAT DOES NOT EXIST.
//
//   1. `disable_auto_redirect` is set from kFollowRedirects, under a static assertion in http.cpp.
//   2. Redirect following lives only in esp_http_client_set_redirection(), reached only from
//      esp_http_client_perform()'s state machine (esp_http_client.c:1104-1107, whose
//      `process_again` is commented "used only in the blocking mode when
//      esp_http_client_perform() is called"). This firmware drives the client through
//      open()/fetch_headers()/read() and calls perform() nowhere, so no redirect is ever followed
//      regardless of configuration.
//   3. No code in this firmware reads a `Location` header, and the only URL the client is given is
//      one built from a resolved, LAN-policy-approved address — see network/lan.hpp. A response
//      cannot name the next destination.
//
// ⛔ AND THE BARRIER THAT IS NOT ONE. `max_redirection_count == 0` does NOT mean "no
// redirections". esp_http_client.c:522-523 replaces a zero with DEFAULT_MAX_REDIRECT, which is
// **10**. An earlier version of this header asserted the opposite; the assertion was removed rather
// than reworded, because a false barrier in a security argument is worse than a missing one. The
// setting below is kept explicit so that a future switch to perform() inherits the intent, but it
// is barrier 1 and 2 that do the work.

namespace prd::network::redirect_policy {

// Automatic redirect following. False, and http.cpp fails to compile if the client is ever
// configured otherwise.
inline constexpr bool kFollowRedirects = false;

// What to write into `esp_http_client_config_t::max_redirection_count`.
//
// One, not zero — precisely because zero means "use the default of 10". One is the smallest value
// the field can carry that actually constrains anything, and it is only ever reached if someone
// later switches to perform(); with open()/read() no redirect is followed at all.
inline constexpr std::uint8_t kMaxRedirectionSetting = 1;

// Whether a status code may be followed rather than reported.
//
// Total over every status: the answer is always no. It exists as a function so a test can state the
// property over the whole range instead of over the codes somebody remembered.
[[nodiscard]] constexpr bool may_follow(std::uint16_t /*status*/) {
    return kFollowRedirects;
}

// Whether a status is a redirect, and therefore something this firmware must classify as a bounded
// failure rather than act on.
[[nodiscard]] constexpr bool is_redirect(const std::uint16_t status) {
    return status >= 300 && status < 400;
}

static_assert(!kFollowRedirects, "redirects must not be followed: the API key is a header");
static_assert(kMaxRedirectionSetting != 0,
              "zero is read by ESP-IDF as 'use the default of 10'; it must not be written");
static_assert(!may_follow(301) && !may_follow(302) && !may_follow(307) && !may_follow(308),
              "no redirect status may be followable");

} // namespace prd::network::redirect_policy
