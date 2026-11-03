// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_client.hpp"

#include <algorithm>
#include <string_view>

#include "ota/release_policy.hpp"
#ifdef PRD_OTA_LAN_MODE
#include "ota_validation_ca.hpp"
#endif

namespace prd::ota {
namespace {

constexpr std::uint32_t kMetadataBudgetMs = 15000;
constexpr std::uint32_t kMetadataConnectBudgetMs = 5000;
constexpr std::uint32_t kMetadataSliceMs = 1000;

bool collect(void* context, const std::uint8_t* bytes, const std::size_t size) {
    auto* const body = static_cast<ReleaseBodyBuffer*>(context);
    if (body == nullptr || bytes == nullptr || size > body->bytes.size() - body->length) {
        return false;
    }
    std::copy_n(bytes, size, body->bytes.data() + body->length);
    body->length += size;
    return true;
}

} // namespace

ReleaseFetchResult discover_latest_release(const ReleaseFetch fetch, void* const fetch_context,
                                           ReleaseBodyBuffer& body,
                                           const network::PublicAbandonPredicate abandon,
                                           void* const abandon_context) {
    body.length = 0;
    ReleaseFetchResult result;
    if (fetch == nullptr) {
        result.transport.outcome = network::PublicHttpsOutcome::Rejected;
        return result;
    }

    ReleaseRoute route{ReleaseResource::Metadata};
    network::PublicHttpsRequest request;
    request.url = release_discovery_url();
#ifdef PRD_OTA_LAN_MODE
    request.media = network::PublicMedia::Json;
    request.certificate_pem = kPrdOtaValidationCaPem;
#else
    request.media = network::PublicMedia::GithubJson;
#endif
    request.maximum_body_bytes = body.bytes.size();
    request.total_budget_ms = kMetadataBudgetMs;
    request.slice_ms = kMetadataSliceMs;
    request.connect_budget_ms = kMetadataConnectBudgetMs;
    request.maximum_redirects = 0;
    request.url_policy = release_url_policy;
    request.url_policy_context = &route;
    request.abandon = abandon;
    request.abandon_context = abandon_context;
    request.sink = collect;
    request.sink_context = &body;

    result.transport = fetch(fetch_context, request);
    if (result.transport.outcome != network::PublicHttpsOutcome::Ok ||
        result.transport.body_length != body.length) {
        result.status = ReleaseFetchStatus::TransportFailed;
        result.candidate = {};
        return result;
    }

    const std::string_view payload{reinterpret_cast<const char*>(body.bytes.data()), body.length};
    result.release = decode_release(payload, result.candidate);
    result.status = result.release == ReleaseStatus::Decoded ? ReleaseFetchStatus::Found
                                                             : ReleaseFetchStatus::ReleaseRejected;
    return result;
}

} // namespace prd::ota
