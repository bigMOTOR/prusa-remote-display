// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "request.hpp"

#include <array>
#include <cstdio>

namespace prd::prusalink {

std::string_view log_label(const TargetVerdict verdict) {
    switch (verdict) {
    case TargetVerdict::Ok:
        return "ok";
    case TargetVerdict::HostUnusable:
        return "host-unusable";
    case TargetVerdict::NotResolved:
        return "not-resolved";
    case TargetVerdict::NotOnLan:
        return "not-on-lan";
    }

    return "unknown";
}

TargetVerdict verdict_for(const network::lan_policy::Resolution& resolution) {
    switch (resolution.outcome) {
    case network::lan_policy::ResolveOutcome::Refused:
        return TargetVerdict::HostUnusable;

    case network::lan_policy::ResolveOutcome::NotFound:
    case network::lan_policy::ResolveOutcome::Failed:
        return TargetVerdict::NotResolved;

    case network::lan_policy::ResolveOutcome::Ok:
        break;
    }

    // ⛔ THE §13.5 GATE. Everything above only established that an address exists; this is the line
    // that decides whether a credential may be sent to it. Default-deny: lan_policy::permits()
    // admits private and link-local and nothing else, so a range nobody has taught it about is
    // refused rather than allowed.
    return network::lan_policy::permits(resolution.address) ? TargetVerdict::Ok
                                                            : TargetVerdict::NotOnLan;
}

bool build_url(const network::lan_policy::Ipv4& address, const std::uint16_t port,
               const std::string_view path, app_model::BoundedString<kMaxUrl>& out) {
    out.clear();

    if (path.empty() || path.front() != '/') {
        return false;
    }

    // Rendered from the four octets, so the authority is exactly the address the policy approved —
    // there is no string from the configuration or from a response anywhere in it.
    std::array<char, 16> authority{};
    const std::size_t authority_length =
        network::lan_policy::render_ipv4(address, authority.data(), authority.size());

    if (authority_length == 0) {
        return false;
    }

    const std::uint16_t effective = port == 0 ? kDefaultPort : port;

    std::array<char, kMaxUrl + 1> buffer{};

    const int written = std::snprintf(
        buffer.data(), buffer.size(), "%.*s%.*s:%u%.*s", static_cast<int>(kScheme.size()),
        kScheme.data(), static_cast<int>(authority_length), authority.data(),
        static_cast<unsigned>(effective), static_cast<int>(path.size()), path.data());

    if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size()) {
        return false;
    }

    return out.assign(std::string_view{buffer.data(), static_cast<std::size_t>(written)});
}

bool auth_supported(const app_model::AuthMethod method) {
    switch (method) {
    case app_model::AuthMethod::ApiKey:
        return true;

    case app_model::AuthMethod::None:
    case app_model::AuthMethod::Digest:
        return false;
    }

    return false;
}

} // namespace prd::prusalink
