// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "app_model/bounded_string.hpp"

// Which of the device's own surfaces a request arrived on, and which origins it may carry —
// owner rulings A and B of Milestone 3.9.
//
// PURE, AND HOST-TESTED. No socket, no ESP-IDF type, no global. The adapter reads the local
// address off the accepted connection and asks this; every decision is here.
//
// WHY ONE TYPE ANSWERS BOTH QUESTIONS. They are keyed on the same data. The device knows a
// small set of addresses it currently answers on; "is this Origin one of mine" and "did this
// request come in on the setup access point" are two reads of that set. Splitting them would
// mean two places that have to agree about which addresses are live, and the interesting
// failure is precisely them disagreeing.
//
// ⛔ MEMBERSHIP IS EXACT AND STAYS EXACT — owner ruling B. Milestone 3.7 compared the Origin
// header against one string; a persistent interface answers on more than one address, so the
// comparison is widened to an explicit set of origins the device genuinely owns. It is NOT
// relaxed: no wildcard, no `*`, no prefix test, no suffix test, no substring. A slot that is
// not currently active matches nothing at all. Accepting an arbitrary Origin would delete
// ruling D9's CSRF protection rather than extend it, and the whole point of the widening is
// that the set grows by one address the device was actually given.

namespace prd::provisioning {

// The scheme every origin carries. Plain HTTP: PrusaLink itself is local HTTP and
// Architecture Spec §13.6 already documents that LAN traffic is not encrypted. Named once
// so the comparison below cannot drift from the origin the server advertises.
inline constexpr std::string_view kOriginScheme = "http://";

// Which surface accepted the connection.
enum class Surface : std::uint8_t {
    // The local address could not be determined. Treated as the most restrictive surface —
    // see permits_mutation().
    Unknown,

    // The provisioning access point: the device's own network, joined deliberately by
    // someone within radio range of it.
    SetupAccessPoint,

    // The user's LAN, reached through the station interface. Created by owner ruling R6,
    // which makes the web UI a persistent local management interface.
    StationLan,
};

[[nodiscard]] std::string_view log_label(Surface surface);

// Whether a request that mutates sensitive configuration may proceed on this surface.
//
// ⬅️ OWNER RULING A, MILESTONE 3.9: ONLY THE SETUP ACCESS POINT. R7 requires explicit
// authorization for any endpoint that mutates sensitive configuration, and its preferred
// mechanism — short-lived authorization established by deliberate physical presence — is
// built at M3.10 together with the button gesture it shares with R1's re-entry. Until that
// exists, the station surface refuses those requests outright. THIS IS R7'S TEMPORARY
// IMPLEMENTATION, NOT A WEAKENING OF IT: refusing is what R7 asks for in the absence of
// authorization, and the alternative — accepting a credential change from anyone on the LAN
// behind nothing but CSRF and a rate limit — is the thing R7 exists to forbid.
//
// UNKNOWN REFUSES. A surface that cannot be identified is not the access point, and the two
// mistakes are not symmetrical: refusing a legitimate mutation strands a user who can retry,
// while permitting an unidentified one is the failure R7 names.
[[nodiscard]] bool permits_mutation(Surface surface);

// The addresses the device answers on, as they stand.
//
// Written by the application task and read by the HTTP task, on the same footing as the
// setup state that server.cpp publishes: one writer, one reader, and values that are only
// ever replaced wholesale.
class Surfaces {
  public:
    // Each setter takes a bare host — an IPv4 literal such as "192.168.4.1", or a name such
    // as "display-0c01.local" — never an origin. The scheme is this type's business, so a
    // caller cannot introduce a second spelling of the same address.
    //
    // AN EMPTY ARGUMENT CLEARS THE SLOT, which is how "no longer active" is expressed:
    // ruling B allows each origin only "while each is genuinely active", and the station
    // address is genuinely absent before DHCP and after a disconnect.
    void set_access_point(std::string_view host);
    void set_station(std::string_view host);
    void set_hostname(std::string_view host);

    void clear();

    // Exact membership over the active slots. False for an empty origin, and false for
    // every origin when nothing is active.
    [[nodiscard]] bool allows_origin(std::string_view origin) const;

    // Which surface a connection whose local address is `local_address` arrived on.
    //
    // The access point is identified by its own address rather than the station being
    // identified by the absence of one: the AP address is the value this device chose and
    // can compare against, while a station address is whatever DHCP handed out.
    [[nodiscard]] Surface classify(std::string_view local_address) const;

    [[nodiscard]] std::string_view access_point() const {
        return access_point_.view();
    }

    [[nodiscard]] std::string_view station() const {
        return station_.view();
    }

    [[nodiscard]] std::string_view hostname() const {
        return hostname_.view();
    }

  private:
    // An IPv4 literal is at most 15 characters.
    app_model::BoundedString<16> access_point_;
    app_model::BoundedString<16> station_;

    // "display-0c01.local" is 18. The capacity is headroom, not a target.
    app_model::BoundedString<64> hostname_;
};

// Whether `origin` is exactly the origin of `host`, i.e. kOriginScheme followed by `host`
// and nothing else.
//
// THE LENGTH IS CHECKED FIRST, which is what makes a prefix or substring match impossible:
// "http://192.168.4.1.example.com" and "http://192.168.4.1:8080" both fail on length before
// any character is compared. An empty host matches nothing, so an inactive slot cannot be
// satisfied by an empty Origin header.
[[nodiscard]] bool origin_matches_host(std::string_view origin, std::string_view host);

// The address the panel shows for the setup page, derived from whichever access-point address
// is currently active. Empty when there is none — and the scene hides a row whose value is
// absent, so a device with no access point shows no address rather than a wrong one.
//
// ⛔ DERIVED, NEVER REMEMBERED. Milestone 3.9's G2 correction removed a cached copy of this
// string because the cache outlived the address; Milestone 3.11 T4's F-B then found that the
// SOURCE it derives from was itself being read before it was populated. Both failures were the
// same mistake at different depths, which is why the derivation lives here, next to the set it
// reads, and is host-tested rather than reasoned about at a call site.
[[nodiscard]] std::string setup_url_for(std::string_view access_point);

} // namespace prd::provisioning
