// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <string_view>

namespace prd::ota {

enum class ReleaseResource : std::uint8_t { Metadata, Manifest, Image };

struct ReleaseRoute {
    ReleaseResource resource{ReleaseResource::Metadata};
};

// Initial URL policy: exact GitHub API endpoint for metadata, or an exact asset
// name under this repository's /releases/download/ path. HTTPS, authority, port,
// userinfo, query and fragment are checked rather than delegated to the client.
[[nodiscard]] bool release_url_allowed(ReleaseResource resource, std::string_view url);

// Callback for network::PublicUrlPolicy. Metadata never redirects. An asset may
// make exactly one transition from its validated github.com URL to GitHub's
// release-assets CDN; direct CDN entry and every further authority hop are denied.
[[nodiscard]] bool release_url_policy(void* context, std::string_view previous,
                                      std::string_view candidate, std::uint8_t redirect_count);

} // namespace prd::ota
