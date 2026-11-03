// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

#include "app_model/bounded_string.hpp"
#include "app_model/device_data.hpp"
#include "network/lan.hpp"

// Request construction (Architecture Spec §5.8's first responsibility, and its `prusalink_auth`).
//
// Pure and host-tested, because a URL is easy to get subtly wrong and a wrong one fails as
// "printer unreachable" — the least informative failure the product has.
//
// ⛔ A URL IS BUILT FROM AN ADDRESS, NEVER FROM A NAME. That is the §13.5 fix, not a tidying: a
// configured host is resolved once, the resulting address is checked against the LAN policy, and
// the request is issued to that address as a literal. Building from the name would leave a window
// in which DNS answers differently for the check and for the connection, and the credential would
// follow the second answer. There is deliberately no overload that accepts a host string.

namespace prd::prusalink {

// The two paths this checkpoint uses. API Research Report §19 lists four endpoints in the MVP set;
// `/api/v1/info` and `/api/v1/job` belong to later checkpoints and are deliberately absent rather
// than declared and unused.
inline constexpr std::string_view kVersionPath = "/api/version";
inline constexpr std::string_view kStatusPath = "/api/v1/status";

// The API-key header, per API Research Report §10.2's confirmed ecosystem behaviour.
inline constexpr std::string_view kApiKeyHeader = "X-Api-Key";

// PrusaLink is plain local HTTP (§10.5, §13.6: "traffic may not be encrypted on the LAN", and the
// product documents that it is for trusted networks). The scheme is fixed here rather than
// configurable: a TLS printer endpoint is not part of the accepted MVP, and offering the choice
// would imply a guarantee this build cannot make.
inline constexpr std::string_view kScheme = "http://";

// The default port when the configuration records none. `config::ConfigV1` stores 0 for "unset".
inline constexpr std::uint16_t kDefaultPort = 80;

// Longest URL this component will build: scheme, a dotted quad, a port, and the longest path.
inline constexpr std::size_t kMaxUrl = 64;

// Why a target may not be used. Every value names something a person can act on, which is what
// lets a failed validation say why without echoing anything submitted.
enum class TargetVerdict : std::uint8_t {
    Ok,

    // The stored host is neither an address literal nor a resolvable name.
    HostUnusable,

    // The name does not resolve, or the resolver could not be reached.
    NotResolved,

    // ⛔ It resolved, and the answer is not on the LAN. §13.5 forbids sending the credential
    // there, so nothing is attempted — this is a refusal, not a failed request.
    NotOnLan,
};

[[nodiscard]] std::string_view log_label(TargetVerdict verdict);

// Decides whether a resolution may receive a credentialed request.
//
// Pure, so the whole policy is provable on the host: the adapter's only job is to produce the
// Resolution, and this function alone decides what may be done with it.
[[nodiscard]] TargetVerdict verdict_for(const network::lan_policy::Resolution& resolution);

// Builds an absolute URL for `path` on an already-approved address.
//
// False when the path is not absolute or the result would not fit. It takes an Ipv4 rather than a
// string, so there is no code path by which an unapproved authority can be expressed at all.
[[nodiscard]] bool build_url(const network::lan_policy::Ipv4& address, std::uint16_t port,
                             std::string_view path, app_model::BoundedString<kMaxUrl>& out);

// Whether a stored authentication method can be used by this checkpoint.
//
// Only ApiKey. §10.3 accepts exactly that for the first hardware slice and keeps Digest as a
// pre-release goal, so a configuration recording Digest is reported as unusable rather than
// silently attempted without credentials.
[[nodiscard]] bool auth_supported(app_model::AuthMethod method);

} // namespace prd::prusalink
