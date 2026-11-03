// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Which destinations may receive a credentialed request.
//
// ⛔ THE RULE THIS ENFORCES IS NOT NEGOTIABLE. Architecture Spec §13.5: credentials must never be
// "sent outside the LAN". §3.1 adds that normal operation must remain LAN-only, that no external
// server is required, and that "the display initiates all connections to the printer". A PrusaLink
// request carries the printer's API key in a header, so the destination of that request is a
// security decision and not a convenience.
//
// ⬅️ WHY SYNTAX IS NOT ENOUGH, AND WHAT REPLACED IT. The previous check accepted any string made
// of letters, digits, dots and hyphens — which accepts `api.example.com` and `8.8.8.8` just as
// readily as `192.168.1.50`. A host string cannot answer the question at all: only an ADDRESS can,
// and only the address the request will actually be sent to. So the policy below is stated over a
// resolved IPv4 address, applied immediately before credentialed I/O, and the request is then
// issued to that literal address rather than to the name — which is also what closes the rebinding
// window between the check and the connection.
//
// ⬅️ IPv4 ONLY, DELIBERATELY. Nothing in this product has been tested against an IPv6 printer, the
// two endpoints of API Research Report §19 need no more, and a policy that admitted a family it
// could not classify would be a hole rather than a feature. An IPv6 destination is refused, and
// that refusal is honest.

namespace prd::network::lan_policy {

// A resolved IPv4 address, in network byte order: octets[0] is the first number written.
struct Ipv4 {
    std::array<std::uint8_t, 4> octets{};

    [[nodiscard]] bool operator==(const Ipv4& other) const {
        return octets == other.octets;
    }
};

// What kind of destination an address is. Ordered by nothing; each is a distinct decision.
enum class Destination : std::uint8_t {
    // 10/8, 172.16/12, 192.168/16 — RFC 1918. The ordinary case: a home or workshop LAN.
    Private,

    // 169.254/16 — RFC 3927 link-local, which is what a printer and display fall back to on a
    // segment with no DHCP server. Still the local link, so still permitted.
    LinkLocal,

    // 127/8. Refused: nothing on this device serves PrusaLink, so a loopback destination is either
    // a misconfiguration or an attempt to reach a local proxy that could forward the key onward.
    Loopback,

    // 0.0.0.0/8 — "this host", never a destination.
    Unspecified,

    // 224/4 multicast, and 255.255.255.255. A credential must never be broadcast.
    Multicast,

    // 100.64/10 carrier NAT, 192.0.2/24 and friends, and everything globally routable. Refused.
    Public,
};

[[nodiscard]] std::string_view log_label(Destination destination);

// Whether a credentialed request may be sent to this address.
//
// True for Private and LinkLocal only. Everything else — including loopback — is refused, and the
// refusal is total: there is no override, no "advanced" flag and no per-request exception, because
// any of those would become the way the rule is bypassed.
[[nodiscard]] bool permits(Destination destination);

[[nodiscard]] Destination classify(const Ipv4& address);

[[nodiscard]] inline bool permits(const Ipv4& address) {
    return permits(classify(address));
}

// Parses a dotted-quad literal. False for anything that is not exactly four decimal octets —
// including a leading zero, which some resolvers read as octal and which would let `010.0.0.1`
// mean two different addresses to the checker and to the network stack.
[[nodiscard]] bool parse_ipv4(std::string_view text, Ipv4& out);

// Renders an address back to a dotted quad, for use as the authority of a URL.
[[nodiscard]] std::size_t render_ipv4(const Ipv4& address, char* out, std::size_t capacity);

// Whether a host string is a name that may be resolved, as opposed to an address literal.
//
// A name is accepted for RESOLUTION only. Whether the request may then be sent is decided by
// classify() on the resulting address, never by the name — §13.5 cannot be enforced against a
// string. Single-label names and `.local` names are the LAN-shaped forms this product documents
// (§5.7's "mDNS-assisted hostname resolution"); a multi-label public-looking name is still allowed
// to resolve, because refusing it by shape would be the syntax check that already failed once.
[[nodiscard]] bool is_resolvable_name(std::string_view host);

// --- Resolution (adapter; implemented in resolve.cpp) ----------------------

enum class ResolveOutcome : std::uint8_t {
    // An address is available in `address`. Either the host was a literal, or the resolver
    // answered. Says nothing about whether the address is permitted — classify() decides that.
    Ok,

    // The name does not resolve: no A record, or the resolver says it does not exist.
    NotFound,

    // The resolver could not be asked, or failed. Distinct from NotFound because one is the
    // network's answer and the other is the absence of one.
    Failed,

    // The host string is neither an address literal nor a resolvable name. Nothing was asked.
    Refused,
};

[[nodiscard]] std::string_view log_label(ResolveOutcome outcome);

struct Resolution {
    ResolveOutcome outcome{ResolveOutcome::Failed};
    Ipv4 address;
};

// Turns a configured host into the single address a request will be sent to.
//
// ⬅️ ONE RESOLUTION, USED ONCE. The caller must send the request to the address this returns,
// rendered as a literal, and never to the name — which is what makes the policy check binding. A
// second resolution at connect time could answer differently, and a name that resolved to
// 192.168.1.50 for the check and to a public address for the connection is precisely the DNS
// rebinding this arrangement exists to prevent.
//
// Restricted to IPv4 (AF_INET). An IPv6-only name reports NotFound, which is honest: this product
// has no tested IPv6 path.
[[nodiscard]] Resolution resolve(std::string_view host);

} // namespace prd::network::lan_policy
