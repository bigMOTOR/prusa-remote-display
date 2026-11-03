// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/lan.hpp"

#include <cstring>
#include <string>

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "diagnostics/log.hpp"
#include "diagnostics/tags.hpp"

// The resolver behind §5.7's "DNS and mDNS-assisted hostname resolution".
//
// Deliberately one function and no cache. §19's endpoint set needs a single address per poll
// cycle, and a cache would be a second place the answer could go stale — the caller already pins
// the address for the life of one request, which is the property that matters.
//
// ⛔ IT RESOLVES; IT DOES NOT DECIDE. Whether the resulting address may receive a credential is
// lan_policy::classify()'s answer, and this function never consults it. Keeping the two apart is
// what lets the policy be proved exhaustively on the host while the resolver stays a thin adapter.
//
// ⛔ NOTHING HERE LOGS THE HOST OR THE ADDRESS. The printer's address is the owner's network
// topology and a serial log is the artifact most likely to be shared, so the line below carries
// the outcome and nothing else.

namespace prd::network::lan_policy {

std::string_view log_label(const ResolveOutcome outcome) {
    switch (outcome) {
    case ResolveOutcome::Ok:
        return "ok";
    case ResolveOutcome::NotFound:
        return "not-found";
    case ResolveOutcome::Failed:
        return "failed";
    case ResolveOutcome::Refused:
        return "refused";
    }

    return "unknown";
}

Resolution resolve(const std::string_view host) {
    Resolution result;

    // A literal needs no resolver, and must not be handed to one: a resolver could answer
    // differently for a name that happens to look like an address.
    if (parse_ipv4(host, result.address)) {
        result.outcome = ResolveOutcome::Ok;
        return result;
    }

    if (!is_resolvable_name(host)) {
        result.outcome = ResolveOutcome::Refused;
        return result;
    }

    // getaddrinfo needs a terminator; the name is not a credential and dies with this call.
    const std::string name{host};

    addrinfo hints{};
    hints.ai_family = AF_INET; // §The IPv4-only decision in lan.hpp.
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* answers = nullptr;
    const int status = getaddrinfo(name.c_str(), nullptr, &hints, &answers);

    if (status != 0 || answers == nullptr) {
        if (answers != nullptr) {
            freeaddrinfo(answers);
        }

        // EAI_NONAME is the resolver's own "no such host"; anything else is a failure to ask.
        result.outcome = status == EAI_NONAME ? ResolveOutcome::NotFound : ResolveOutcome::Failed;

        diagnostics::log(diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
                         "resolve outcome=" + std::string{log_label(result.outcome)});
        return result;
    }

    // ⬅️ THE FIRST A RECORD, AND ONLY THE FIRST. A name with several addresses would otherwise
    // invite a retry loop that walks them, and each walked address is another destination the
    // policy has to be applied to. One address per resolution keeps the check and the connection
    // talking about the same thing.
    bool found = false;

    for (const addrinfo* entry = answers; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == nullptr) {
            continue;
        }

        const auto* in = reinterpret_cast<const sockaddr_in*>(entry->ai_addr);
        std::uint32_t raw = 0;
        std::memcpy(&raw, &in->sin_addr, sizeof(raw));

        // s_addr is network byte order, so the first octet written is the low byte.
        result.address.octets[0] = static_cast<std::uint8_t>(raw & 0xFFU);
        result.address.octets[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
        result.address.octets[2] = static_cast<std::uint8_t>((raw >> 16U) & 0xFFU);
        result.address.octets[3] = static_cast<std::uint8_t>((raw >> 24U) & 0xFFU);
        found = true;
        break;
    }

    freeaddrinfo(answers);

    result.outcome = found ? ResolveOutcome::Ok : ResolveOutcome::NotFound;

    diagnostics::log(
        diagnostics::LogLevel::Info, diagnostics::tag::kNetwork,
        "resolve outcome=" + std::string{log_label(result.outcome)} +
            (found ? " class=" + std::string{log_label(classify(result.address))} : std::string{}));

    return result;
}

} // namespace prd::network::lan_policy
