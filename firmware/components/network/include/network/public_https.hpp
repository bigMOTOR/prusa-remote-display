// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace prd::network {

// A credential-free, certificate-verified public HTTPS stream. This is a
// deliberately separate surface from network::get(): callers cannot supply a
// header, so a printer API key can never enter this path, and its redirect rules
// cannot weaken the credential-bearing LAN client.
enum class PublicMedia : std::uint8_t {
    Json,
    Binary,
    // GitHub's versioned REST contract. This closed value adds the current
    // official version header without opening a generic caller-header channel.
    GithubJson,
};

enum class PublicHttpsOutcome : std::uint8_t {
    Ok,
    HttpStatus,
    ConnectFailed,
    Timeout,
    Cancelled,
    ResponseTooLarge,
    SinkRejected,
    RedirectRejected,
    TransportFailed,
    Rejected,
};

[[nodiscard]] std::string_view log_label(PublicHttpsOutcome outcome);

using PublicAbandonPredicate = bool (*)(void* context);
using PublicChunkSink = bool (*)(void* context, const std::uint8_t* bytes, std::size_t size);

// Called before the initial request (`redirect_count == 0`, previous empty) and
// before every redirect. The policy therefore sees the whole authority chain,
// rather than trusting a URL merely because the first hop was allowed.
using PublicUrlPolicy = bool (*)(void* context, std::string_view previous,
                                 std::string_view candidate, std::uint8_t redirect_count);

struct PublicHttpsRequest {
    std::string_view url;
    PublicMedia media{PublicMedia::Json};
    std::size_t maximum_body_bytes{0};
    std::uint32_t total_budget_ms{30000};
    std::uint32_t slice_ms{1000};
    std::uint32_t connect_budget_ms{5000};
    std::uint8_t maximum_redirects{1};
    PublicUrlPolicy url_policy{nullptr};
    void* url_policy_context{nullptr};
    PublicAbandonPredicate abandon{nullptr};
    void* abandon_context{nullptr};
    PublicChunkSink sink{nullptr};
    void* sink_context{nullptr};
    // Optional validation-only trust anchor. Normal requests use the ESP CA
    // bundle; this pointer never supplies a client credential or private key.
    const char* certificate_pem{nullptr};
};

struct PublicHttpsResponse {
    PublicHttpsOutcome outcome{PublicHttpsOutcome::Rejected};
    std::uint16_t status{0};
    std::uint32_t latency_ms{0};
    std::size_t body_length{0};
    std::uint8_t redirects{0};
};

// Performs one bounded GET and streams each body chunk directly to the caller.
// The ESP certificate bundle is the normal trust store; a validation request can
// supply one explicit public CA PEM instead. There is one wall-clock budget across
// connect, all redirects, headers and body. No URL or body content is logged.
[[nodiscard]] PublicHttpsResponse get_public_https(const PublicHttpsRequest& request);

} // namespace prd::network
