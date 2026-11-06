// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "public_https_limits.hpp"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// The three formatting calls and truncation checks in pinned ESP-IDF v5.5.4,
// components/esp_http_client/esp_http_client.c:1653-1677. This models request-line
// capacity only: it does not claim to exercise TLS, a socket or the whole SDK.
bool request_line(const std::string& url, const int capacity, std::string& output) {
    const auto path_start = url.find('/', sizeof("https://") - 1);
    const auto query_start = url.find('?', path_start);
    const auto path = url.substr(path_start, query_start - path_start);
    const auto query =
        query_start == std::string::npos ? std::string{} : url.substr(query_start + 1);
    std::vector<char> buffer(static_cast<std::size_t>(capacity));
    int length = std::snprintf(buffer.data(), buffer.size(), "%s %s", "GET", path.c_str());
    if (length < 0 || length >= capacity) {
        return false;
    }
    if (query_start != std::string::npos) {
        length +=
            std::snprintf(buffer.data() + length, buffer.size() - static_cast<std::size_t>(length),
                          "?%s", query.c_str());
        if (length >= capacity) {
            return false;
        }
    }
    length +=
        std::snprintf(buffer.data() + length, buffer.size() - static_cast<std::size_t>(length),
                      " %s\r\n", "HTTP/1.1");
    if (length >= capacity) {
        return false;
    }
    output.assign(buffer.data(), static_cast<std::size_t>(length));
    return true;
}

bool check(const std::string& url) {
    using namespace prd::network::public_https_limits;
    std::string line;
    const auto expected =
        "GET " + url.substr(url.find('/', sizeof("https://") - 1)) + " HTTP/1.1\r\n";
    if (!request_line(url, kTransmitBufferSize, line) || line != expected) {
        std::fprintf(stderr, "production TX buffer truncated %zu-byte URL\n", url.size());
        return false;
    }
    if (request_line(url, 512, line)) {
        std::fprintf(stderr, "fixture failed to reproduce old 512-byte limit\n");
        return false;
    }
    std::printf("URL=%zu request-line=%zu production-TX=%d PASS; old-TX=512 REJECTED\n", url.size(),
                expected.size(), kTransmitBufferSize);
    return true;
}

} // namespace

int main() {
    using namespace prd::network::public_https_limits;
    // Synthetic, non-secret CDN shape: no real signed query, account or asset ID.
    const std::string base =
        "https://release-assets.githubusercontent.com/github-production-release-asset/"
        "1/fixture?sv=2018-11-09&sr=b&sp=r&sig=synthetic%2Fvalue&jwt=";
    // Include the observed address length and fixtures beyond the former 1024-byte
    // cap, leaving room for longer synthetic CDN signatures without real secrets.
    for (const std::size_t size : std::array<std::size_t, 4>{930, 1100, 1536, kMaximumUrl}) {
        auto url = base;
        url.append(size - url.size(), 'a');
        if (!check(url)) {
            return 1;
        }
    }
    // Also cover a maximum-size path without a query, which fails in the first
    // snprintf rather than the signed-query append.
    std::string path = "https://release-assets.githubusercontent.com/";
    path.append(kMaximumUrl - path.size(), 'a');
    return check(path) ? 0 : 1;
}
