// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/public_https.hpp"
#include "public_https_limits.hpp"
#include "vendor_log_policy.hpp"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include <cstdio>
#include <string>
#include <vector>

struct TestHttpClient {
    esp_http_client_config_t config{};
    int status{200};
};

namespace {
TestHttpClient client;
std::vector<int> transmitted_buffers;
std::vector<std::string> requested_urls;
std::string redirect_location;
int cleaned = 0;

bool expect(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

std::string sized_url(const std::size_t size) {
    std::string value = "https://release-assets.githubusercontent.com/fixture?synthetic=";
    value.append(size - value.size(), 'a');
    return value;
}

prd::network::PublicHttpsResponse perform(const std::string& url, const std::string& location) {
    transmitted_buffers.clear();
    requested_urls.clear();
    redirect_location = location;
    cleaned = 0;
    prd::network::PublicHttpsRequest request;
    request.url = url;
    request.maximum_body_bytes = 1024;
    request.sink = [](void*, const std::uint8_t*, std::size_t) { return true; };
    // Test boundary policy only; the production OTA allowlist has its own suite.
    request.url_policy = [](void*, std::string_view, std::string_view, std::uint8_t) {
        return true;
    };
    return prd::network::get_public_https(request);
}

bool transmitted(const std::size_t count) {
    bool ok = expect(transmitted_buffers.size() == count, "actual init call count");
    for (const int value : transmitted_buffers) {
        ok = expect(value == 4096, "actual esp_http_client_init buffer_size_tx must be 4096") && ok;
    }
    return expect(cleaned == static_cast<int>(count), "every client cleaned") && ok;
}
} // namespace

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    transmitted_buffers.push_back(config->buffer_size_tx);
    requested_urls.emplace_back(config->url);
    client.config = *config;
    client.status = requested_urls.size() == 1 && !redirect_location.empty() ? 302 : 200;
    return &client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*) {
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int) {
    return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int) {
    return ESP_OK;
}
std::int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle) {
    if (handle->status == 302) {
        char key[] = "Location";
        esp_http_client_event_t event{HTTP_EVENT_ON_HEADER, handle->config.user_data, key,
                                      redirect_location.data()};
        handle->config.event_handler(&event);
    }
    return 0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
    return handle->status;
}
std::int64_t esp_http_client_get_content_length(esp_http_client_handle_t) {
    return 0;
}
int esp_http_client_read(esp_http_client_handle_t, char*, int) {
    return 0;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) {
    return true;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t) {
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t) {
    ++cleaned;
    return ESP_OK;
}
esp_err_t esp_crt_bundle_attach(void*) {
    return ESP_OK;
}
std::int64_t esp_timer_get_time() {
    return 0;
}
namespace prd::diagnostics {
void log(LogLevel, std::string_view, std::string_view) {}
} // namespace prd::diagnostics
namespace prd::network::vendor_log_policy {
void apply() {}
} // namespace prd::network::vendor_log_policy

int main() {
    using prd::network::PublicHttpsOutcome;
    // Literal assertions keep accidental changes to the shared constants visible.
    bool ok = expect(prd::network::public_https_limits::kMaximumUrl == 2048, "URL cap 2048");
    const auto maximum = sized_url(2048);
    ok = expect(perform(maximum, {}).outcome == PublicHttpsOutcome::Ok, "URL 2048 accepted") && ok;
    ok = transmitted(1) && ok;
    ok = expect(requested_urls == std::vector<std::string>{maximum}, "full URL reaches init") && ok;
    ok = expect(perform(sized_url(2049), {}).outcome == PublicHttpsOutcome::Rejected,
                "URL 2049 rejected before init") &&
         ok;
    ok = transmitted(0) && ok;
    const std::string initial = "https://github.com/fixture";
    ok = expect(perform(initial, maximum).outcome == PublicHttpsOutcome::Ok,
                "Location 2048 followed") &&
         ok;
    ok = transmitted(2) && ok;
    ok = expect(requested_urls == std::vector<std::string>{initial, maximum},
                "full Location reaches redirected init") &&
         ok;
    ok = expect(perform(initial, sized_url(2049)).outcome == PublicHttpsOutcome::RedirectRejected,
                "Location 2049 rejected") &&
         ok;
    ok = transmitted(1) && ok;
    if (ok) {
        std::puts("actual adapter: TX=4096; URL/Location 2048 accepted, 2049 rejected PASS");
    }
    return ok ? 0 : 1;
}
