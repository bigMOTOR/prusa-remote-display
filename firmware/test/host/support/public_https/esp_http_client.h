// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "esp_err.h"
#include <cstdint>

// Only the API surface consumed by the real public_https.cpp. No SDK networking
// or TLS implementation is modelled; init captures the actual caller config.
struct TestHttpClient;
using esp_http_client_handle_t = TestHttpClient*;
enum esp_http_client_event_id_t { HTTP_EVENT_ON_HEADER };
enum esp_http_client_method_t { HTTP_METHOD_GET };
struct esp_http_client_event_t {
    esp_http_client_event_id_t event_id;
    void* user_data;
    char* header_key;
    char* header_value;
};
struct esp_http_client_config_t {
    const char* url{};
    esp_http_client_method_t method{};
    int buffer_size_tx{};
    int timeout_ms{};
    bool disable_auto_redirect{};
    int max_redirection_count{};
    const char* cert_pem{};
    esp_err_t (*crt_bundle_attach)(void*){};
    esp_err_t (*event_handler)(esp_http_client_event_t*){};
    void* user_data{};
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
std::int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
std::int64_t esp_http_client_get_content_length(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char*, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
