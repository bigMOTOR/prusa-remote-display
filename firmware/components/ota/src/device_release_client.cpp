// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "ota/release_client.hpp"

namespace prd::ota {
namespace {

network::PublicHttpsResponse fetch(void*, const network::PublicHttpsRequest& request) {
    return network::get_public_https(request);
}

} // namespace

ReleaseFetchResult discover_latest_release(ReleaseBodyBuffer& body,
                                           const network::PublicAbandonPredicate abandon,
                                           void* const abandon_context) {
    return discover_latest_release(fetch, nullptr, body, abandon, abandon_context);
}

} // namespace prd::ota
