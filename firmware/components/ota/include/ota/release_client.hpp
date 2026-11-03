// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "network/public_https.hpp"
#include "ota/release_discovery.hpp"

namespace prd::ota {

// The discovery response is intentionally caller-owned. A 32 KiB JSON bound must not become a
// hidden stack allocation on the future OTA worker task, and no unbounded heap string is needed.
struct ReleaseBodyBuffer {
    std::array<std::uint8_t, kReleaseBodyLimit> bytes{};
    std::size_t length{0};
};

enum class ReleaseFetchStatus : std::uint8_t { Found, TransportFailed, ReleaseRejected };

struct ReleaseFetchResult {
    ReleaseFetchStatus status{ReleaseFetchStatus::TransportFailed};
    network::PublicHttpsResponse transport;
    ReleaseStatus release{ReleaseStatus::InvalidField};
    ReleaseCandidate candidate;
};

// Injectable production-shaped transport. Host tests exercise the complete request, streaming
// collector and decoder boundary; the target overload below supplies the real ESP-IDF HTTPS call.
using ReleaseFetch = network::PublicHttpsResponse (*)(void* context,
                                                      const network::PublicHttpsRequest& request);

[[nodiscard]] ReleaseFetchResult
discover_latest_release(ReleaseFetch fetch, void* fetch_context, ReleaseBodyBuffer& body,
                        network::PublicAbandonPredicate abandon = nullptr,
                        void* abandon_context = nullptr);

// Target adapter: credential-free certificate-verified ESP-IDF HTTPS, exact GitHub endpoint,
// closed GitHub media/version headers and no redirects for metadata.
[[nodiscard]] ReleaseFetchResult
discover_latest_release(ReleaseBodyBuffer& body, network::PublicAbandonPredicate abandon = nullptr,
                        void* abandon_context = nullptr);

} // namespace prd::ota
