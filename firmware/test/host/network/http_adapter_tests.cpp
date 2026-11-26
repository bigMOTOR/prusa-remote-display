// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "network/http.hpp"

#include "diagnostics/log.hpp"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "vendor_log_policy.hpp"

#include <cstdio>

struct TestHttpClient {
    int header_status{};
    int body_error{};
    int parsed_status{};
    int read_calls{};
    int timeout_ms{};
    std::int64_t now_us{};
};

namespace {
TestHttpClient client;

bool check_body_failure(const int status, const int body_error,
                        const prd::network::HttpOutcome expected_outcome,
                        const int expected_reads) {
    client = {};
    client.header_status = status;
    client.body_error = body_error;

    prd::network::HttpRequest request;
    request.url = "http://127.0.0.1/fixture";
    request.total_budget_ms = 100;
    request.slice_ms = 50;
    char storage[4]{};
    prd::network::BodyBuffer body{storage, sizeof(storage)};
    const auto response = prd::network::get(request, body);

    const auto label = prd::network::log_label(expected_outcome);
    const auto actual = prd::network::log_label(response.outcome);
    if (response.outcome != expected_outcome || client.read_calls != expected_reads ||
        body.view() != "{" || response.body_length != 1) {
        std::fprintf(stderr,
                     "FAIL: expected %s after partial body and %d reads, got %s and %d reads\n",
                     label.data(), expected_reads, actual.data(), client.read_calls);
        return false;
    }
    if (response.status != status) {
        std::fprintf(stderr, "FAIL: %s after headers lost HTTP status (expected %d, got %u)\n",
                     label.data(), status, static_cast<unsigned>(response.status));
        return false;
    }

    std::printf("PASS: %s after headers preserved HTTP %d (%d reads)\n", label.data(), status,
                client.read_calls);
    return true;
}
} // namespace

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*) {
    return &client;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*) {
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int) {
    return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t handle, const int timeout_ms) {
    handle->timeout_ms = timeout_ms;
    return ESP_OK;
}
std::int64_t esp_http_client_fetch_headers(esp_http_client_handle_t handle) {
    handle->parsed_status = handle->header_status;
    return 4;
}
int esp_http_client_get_status_code(esp_http_client_handle_t handle) {
    return handle->parsed_status;
}
int esp_http_client_read(esp_http_client_handle_t handle, char* body, int) {
    if (++handle->read_calls == 1) {
        body[0] = '{';
        return 1;
    }

    if (handle->body_error == -ESP_ERR_HTTP_EAGAIN) {
        handle->now_us += static_cast<std::int64_t>(handle->timeout_ms) * 1000;
    }
    return handle->body_error;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) {
    return false;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t) {
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t) {
    return ESP_OK;
}
std::int64_t esp_timer_get_time() {
    return client.now_us;
}
namespace prd::diagnostics {
void log(LogLevel, std::string_view, std::string_view) {}
} // namespace prd::diagnostics
namespace prd::network::vendor_log_policy {
void apply() {}
} // namespace prd::network::vendor_log_policy

int main() {
    using prd::network::HttpOutcome;
    // The partial body is followed by two timed-out slices that exhaust the total budget.
    const bool timeout = check_body_failure(503, -ESP_ERR_HTTP_EAGAIN, HttpOutcome::Timeout, 3);
    const bool transport_failed =
        check_body_failure(403, ESP_FAIL, HttpOutcome::TransportFailed, 2);
    return timeout && transport_failed ? 0 : 1;
}
